/**
 * @file account.c
 * @brief The two exchanges with the user table.
 *
 * Phase 4e (#55). The query shapes are #47's, the transaction and the
 * semaphore are #44's, the connection and its deadline are
 * include/libtetrisdb/socket/db.h's. Contract in tauth_priv.h.
 *
 * THE SEMAPHORE IS WHAT MAKES USERNAMES UNIQUE, NOT THE TRANSACTION. SimpleDB's
 * HeapFile.insertTuple appends a brand new page when every existing page is
 * full, and a freshly appended page carries no lock anyone conflicts with - so
 * two concurrent registrations of the same name both commit, with no deadlock,
 * no <<END retry>> and no error. The transaction underneath is a second layer,
 * not the guard. Every future writer of this table must take
 * "/tetrish_register" or duplicates come back silently; the full argument is
 * beside the schema in include/libtetrisauth/provision.h.
 *
 * TWO BUDGETS, DELIBERATELY. The wait for the semaphore is not charged against
 * db_timeout. At 254 simultaneous registrations the tail waits about 1.2 s for
 * its turn (#44 section 8), and charging that against the exchange's deadline
 * would 500 the tail of a herd on a perfectly healthy server. The clock that
 * matters starts at tdb_socket_open(), which is after the semaphore is held.
 *
 * PBKDF2 RUNS OUTSIDE BOTH. 65 ms of hashing inside the critical section would
 * multiply every queued registration's wait by the herd size, and inside the
 * deadline it would spend a third of the budget before a byte moved.
 */

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "libtetrisutil/limits.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisauth/provision.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/db.h"

#include "tauth_priv.h"

/* Recreated by tetrisdb start (ADR 0002 step 3) so a session killed while
 * holding it cannot wedge registration permanently. Opened with O_CREAT here
 * as well, because a session must not be the thing that fails when it is the
 * first to arrive. */
#define REG_SEM_NAME "/tetrish_register"

/*
 * How long to wait for a turn at registration, in ms, and NOT an rc key.
 *
 * #44 asked for one and #60 closed the key set without it, which is the right
 * answer: the number is a property of how long a serialized transaction takes
 * (4.9-6.7 ms) times how many clients can exist (254), and an operator has no
 * information with which to tune it. 5 s is four times the measured 1.24 s
 * worst case.
 */
#define REG_WAIT_MS 5000

/* One poll of the semaphore, jittered so a herd released together does not
 * re-collide on a fixed period. */
#define REG_POLL_MIN_US 1000
#define REG_POLL_SPAN_US 4000

/* #44 section 3: a small N, because the library's deadline is the real bound,
 * and a short fixed backoff, because transactions take 4.9-6.7 ms and
 * exponential backoff has no dynamic range at that scale (#47 decision 12). */
#define TXN_ATTEMPTS 3
#define TXN_BACKOFF_US 5000

/* A select of one row plus SimpleDB's narration. A body that does not fit is
 * reported by tdb_row_count() as "not a table" rather than as zero rows, so
 * an undersized buffer here is a 500 and never a wrong answer. */
#define BODY_MAX 4096

#define SALT_HEX_LEN 32
#define DIGEST_HEX_LEN 64

/* A quoted username: two quotes, a NUL, and room for the doubling that #47
 * decision 9 says can never happen. */
#define QUOTED_MAX (MAX_PLAYER_NAME * 2 + 4)

/* xorshift seeded per process, for the poll jitter only. Not rand(): this is a
 * forked process that also mints tokens, and quietly sharing a PRNG's state
 * with anything cryptographic is a habit worth not having. */
static unsigned jitter_us(void) {
  static unsigned state;

  if (state == 0)
    state = (unsigned)getpid() * 2654435761u + 1u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return REG_POLL_MIN_US + state % REG_POLL_SPAN_US;
}

/** Opens the connection this operation owns, from the frozen config. NULL is
 * "no answer is coming", reported as 500 without asking which of the several
 * ways it failed - they are one thing to the client. */
static tdb_socket_t *conn_open(void) {
  tdb_socket_opts_t opts;
  const auth_conf_t *conf = auth_conf();

  tdb_socket_opts_default(&opts);
  snprintf(opts.sock, sizeof opts.sock, "%s", conf->db_sock);
  opts.timeout_ms = conf->db_timeout_ms;

  return tdb_socket_open(&opts);
}

/** sem_trywait in a jittered loop, bounded by REG_WAIT_MS. Returns the
 * semaphore, or NULL.
 * NOT sem_wait(): the textbook blocking wait is an unbounded hang holding a
 * client that has already handshaked, which is the exact failure every
 * deadline in this library exists to remove. NOT sem_timedwait() either -
 * macOS does not implement it and CI runs on macOS. NOT sem_init(): tetrisd
 * forks AND execs the session binary, so an unnamed semaphore in memory does
 * not survive, and macOS does not implement that one either. */
static sem_t *reg_acquire(void) {
  sem_t *sem = sem_open(REG_SEM_NAME, O_CREAT, 0600, 1);
  if (sem == SEM_FAILED) {
    log_send(LOG_ERROR, "auth: sem_open(" REG_SEM_NAME ") failed: %s",
             strerror(errno));
    return NULL;
  }

  for (int waited_us = 0; waited_us < REG_WAIT_MS * 1000;) {
    if (sem_trywait(sem) == 0)
      return sem;

    unsigned nap = jitter_us();
    usleep(nap);
    waited_us += (int)nap;
  }

  log_send(LOG_ERROR,
           "auth: no turn at " REG_SEM_NAME " within %d ms; registration "
           "answered 500",
           REG_WAIT_MS);
  sem_close(sem);
  return NULL;
}

static void reg_release(sem_t *sem) {
  if (sem == NULL)
    return;
  sem_post(sem);
  sem_close(sem);
}

/* One field of one row as a NUL-terminated copy, so it can be handed to
 * strtoll. Returns 0, or -1 when it does not fit - the fields this is used on
 * are integers written by this file. */
static int field_text(const char *src, size_t len, char *dst, size_t cap) {
  if (len == 0 || len >= cap)
    return -1;
  memcpy(dst, src, len);
  dst[len] = '\0';
  return 0;
}

/** The id for the next row: max(id) + 1, or 1 on an empty table.
 * Returns -1 when the reply is not a table at all, which the caller turns into
 * a 500 rather than a guess: an id chosen from a misread reply collides with a
 * real account, and a token's sub would then name the wrong person. A negative
 * stored value is treated as an empty table, because that is one way an
 * aggregate over no rows can print. */
static long long next_id(const char *body) {
  int rows = tdb_row_count(body);
  if (rows < 0)
    return -1;
  if (rows == 0)
    return 1;

  const char *f[1];
  size_t len[1];
  char text[32];

  if (tdb_row_fields(body, 0, f, len, 1) < 1 ||
      field_text(f[0], len[0], text, sizeof text) != 0)
    return -1;

  char *end;
  long long v = strtoll(text, &end, 10);
  if (*end != '\0')
    return -1;
  return v < 0 ? 1 : v + 1;
}

acct_status_t account_login(const char *name, const cred_t *c, long long *id) {
  char quoted[QUOTED_MAX];
  char sql[256];
  char body[BODY_MAX];

  /* Cannot fire after cred_name(): the allowlist has no quote in it. Kept
   * because the validator and this statement are separate code that can
   * drift, and because it turns the failure into a stored oddity instead of
   * SQL injection - a doubled quote in user.dat is proof the validator was
   * bypassed (#47 decision 9). */
  tdb_quote(quoted, sizeof quoted, name);

  /* An explicit projection, not select *: the field positions below come from
   * this line rather than from the catalog, so a column added later cannot
   * shift them (#47 decision 11). */
  snprintf(sql, sizeof sql,
           "select id, salt, digest, iters from " TETRISAUTH_DB_TABLE
           " where name = %s;",
           quoted);

  tdb_socket_t *conn = conn_open();
  if (conn == NULL)
    return ACCT_UNAVAILABLE;

  /* One re-exec on TDB_RETRY. An auto-commit select is still a transaction and
   * can be picked as the deadlock victim while a registration upgrades a page
   * it is scanning, so this is insurance against a spurious failure on a valid
   * password rather than a response to a seen failure (#44 section 4). */
  tdb_status_t st = tdb_socket_exec(conn, sql, body, sizeof body);
  if (st == TDB_RETRY)
    st = tdb_socket_exec(conn, sql, body, sizeof body);

  /* Closed before the hash, not after. A connection is one of the runner's
   * --sessions slots and holding it through 65 ms of PBKDF2 queues somebody
   * else's login behind arithmetic. The row is already in body, which is this
   * frame's. */
  tdb_socket_close(conn);

  if (st != TDB_OK) {
    log_send(LOG_ERROR, "auth: login query failed (tdb status %d)", (int)st);
    return ACCT_UNAVAILABLE;
  }

  int rows = tdb_row_count(body);
  if (rows < 0) {
    log_send(LOG_ERROR, "auth: the login reply carried no table");
    return ACCT_UNAVAILABLE;
  }
  if (rows == 0)
    /* #47 decision 13 made 404 and 401 distinguishable, so there is no dummy
     * salt and no PBKDF2 on this path. The early return IS the decision, not a
     * timing leak to close: deleting it to equalise latency would restore a
     * property the design gave up on purpose, and leaving it in place without
     * knowing that is how it gets deleted next year. */
    return ACCT_NO_USER;

  const char *f[4];
  size_t len[4];
  int fields = tdb_row_fields(body, 0, f, len, 4);
  if (fields != 4) {
    /* Fields are POSITIONAL. More or fewer than the projection asked for is a
     * schema disagreement, and reading on would compare a digest against a
     * salt with nothing anywhere reporting it. */
    log_send(LOG_ERROR, "auth: a user row has %d fields, expected 4", fields);
    return ACCT_UNAVAILABLE;
  }
  if (len[1] != SALT_HEX_LEN || len[2] != DIGEST_HEX_LEN) {
    log_send(LOG_ERROR, "auth: a user row has a malformed salt or digest");
    return ACCT_UNAVAILABLE;
  }

  char text[32];
  if (field_text(f[0], len[0], text, sizeof text) != 0) {
    log_send(LOG_ERROR, "auth: a user row has an unreadable id");
    return ACCT_UNAVAILABLE;
  }
  long long uid = strtoll(text, NULL, 10);

  if (field_text(f[3], len[3], text, sizeof text) != 0) {
    log_send(LOG_ERROR, "auth: a user row has an unreadable iteration count");
    return ACCT_UNAVAILABLE;
  }
  /* THIS ROW's iteration count, never the configured one. That is what the
   * column is for: raising auth_pbkdf2_iters must not make every existing
   * account fail to verify, and SimpleDB has no UPDATE to migrate them
   * with. */
  int iters = (int)strtol(text, NULL, 10);
  if (iters < 1) {
    log_send(LOG_ERROR, "auth: a user row has an unusable iteration count");
    return ACCT_UNAVAILABLE;
  }

  char want[DIGEST_HEX_LEN + 1];
  if (cred_hash(c, f[1], len[1], iters, want, sizeof want) != 0) {
    log_send(LOG_ERROR, "auth: PBKDF2 failed on the login path");
    return ACCT_UNAVAILABLE;
  }

  int match = CRYPTO_memcmp(want, f[2], DIGEST_HEX_LEN) == 0;
  OPENSSL_cleanse(want, sizeof want);

  if (!match)
    return ACCT_BAD_PASSWORD;

  *id = uid;
  return ACCT_OK;
}

/** One attempt at the registration transaction on an open connection.
 * Returns the tdb status of whatever ended it, so the caller can tell "run it
 * again" (TDB_RETRY) from "give up", and reports the outcome through *taken
 * and *id.
 * THE WHOLE TRANSACTION IS THE RETRY UNIT, never the insert alone: an aborted
 * transaction is gone, so resubmitting the insert by itself would run it
 * outside the existence check that makes it safe. */
static tdb_status_t register_txn(tdb_socket_t *conn, const char *quoted,
                                 const char *salt_hex, const char *digest_hex,
                                 int iters, int *taken, long long *id) {
  char sql[512];
  char body[BODY_MAX];
  tdb_status_t st;

  *taken = 0;

  st = tdb_socket_exec(conn, "set transaction read write;", NULL, 0);
  if (st != TDB_OK)
    return st;

  /* The id allocation is the first statement of the transaction, so the scan
   * that reads max(id) holds its locks until the insert that uses it commits
   * (#47 decision 4). Outside the transaction it would be a probe whose answer
   * expires before it is used. */
  st = tdb_socket_exec(conn, "select max(id) from " TETRISAUTH_DB_TABLE ";",
                       body, sizeof body);
  if (st != TDB_OK)
    return st;

  long long next = next_id(body);
  if (next < 0) {
    log_send(LOG_ERROR, "auth: could not read max(id) from the registration "
                        "reply");
    return TDB_ERROR;
  }

  snprintf(sql, sizeof sql,
           "select id from " TETRISAUTH_DB_TABLE " where name = %s;", quoted);
  st = tdb_socket_exec(conn, sql, body, sizeof body);
  if (st != TDB_OK)
    return st;

  int rows = tdb_row_count(body);
  if (rows < 0) {
    log_send(LOG_ERROR, "auth: the existence check carried no table");
    return TDB_ERROR;
  }
  if (rows > 0) {
    /* Intent, not a requirement: closing the connection rolls back anything
     * left open, so the outcome is the same if this statement never lands. It
     * is here because a reader following the transaction should see where it
     * ends. */
    (void)tdb_socket_exec(conn, "rollback;", NULL, 0);
    *taken = 1;
    return TDB_OK;
  }

  /* salt and digest go in unquoted-by-tdb_quote because this file generated
   * them a few lines ago and they are hex by construction. The username is
   * the only value here that came from outside. */
  snprintf(sql, sizeof sql,
           "insert into " TETRISAUTH_DB_TABLE
           " values (%lld, %s, '%s', '%s', %d, %lld);",
           next, quoted, salt_hex, digest_hex, iters, (long long)time(NULL));
  st = tdb_socket_exec(conn, sql, NULL, 0);
  if (st != TDB_OK)
    return st;

  st = tdb_socket_exec(conn, "commit;", NULL, 0);
  if (st != TDB_OK)
    return st;

  *id = next;
  return TDB_OK;
}

acct_status_t account_register(const char *name, const cred_t *c,
                               long long *id) {
  const auth_conf_t *conf = auth_conf();
  char quoted[QUOTED_MAX];
  char salt_hex[SALT_HEX_LEN + 1];
  char digest_hex[DIGEST_HEX_LEN + 1];
  acct_status_t result = ACCT_UNAVAILABLE;

  tdb_quote(quoted, sizeof quoted, name);

  if (cred_new_salt(salt_hex, sizeof salt_hex) != 0) {
    log_send(LOG_ERROR, "auth: RAND_bytes failed while making a salt");
    return ACCT_UNAVAILABLE;
  }
  /* Before the semaphore and before the connection, so the 65 ms nobody can
   * avoid is spent while holding neither. */
  if (cred_hash(c, salt_hex, strlen(salt_hex), conf->pbkdf2_iters, digest_hex,
                sizeof digest_hex) != 0) {
    log_send(LOG_ERROR, "auth: PBKDF2 failed on the registration path");
    return ACCT_UNAVAILABLE;
  }

  sem_t *sem = reg_acquire();
  if (sem == NULL)
    return ACCT_UNAVAILABLE;

  tdb_socket_t *conn = conn_open();
  if (conn == NULL) {
    reg_release(sem);
    return ACCT_UNAVAILABLE;
  }

  for (int attempt = 0; attempt < TXN_ATTEMPTS; attempt++) {
    int taken = 0;
    tdb_status_t st = register_txn(conn, quoted, salt_hex, digest_hex,
                                   conf->pbkdf2_iters, &taken, id);

    if (st == TDB_RETRY) {
      usleep(TXN_BACKOFF_US);
      continue;
    }
    if (st != TDB_OK) {
      log_send(LOG_ERROR, "auth: registration failed (tdb status %d)", (int)st);
      break;
    }
    result = taken ? ACCT_TAKEN : ACCT_OK;
    break;
  }

  tdb_socket_close(conn);
  reg_release(sem);
  return result;
}
