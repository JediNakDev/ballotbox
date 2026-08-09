#ifndef LIBTETRISAUTH_TAUTH_PRIV_H
#define LIBTETRISAUTH_TAUTH_PRIV_H

/**
 * @file tauth_priv.h
 * @brief The entry point's own parts, split out of tauth.c.
 *
 * Beside its .c files and NOT in include/, for the reason
 * src/libtetrissh/common.h is: nothing outside this library may call any of
 * it. tetrisauth.h stays exactly #55's three functions.
 *
 * The split is by what a reader is looking for, not by layer:
 *
 *   tauth.c       the seam: the file-static block, the recv/reply loop, the
 *                 method arms, both scrubs, the attempt counter.
 *   authconf.c    the .tetrishrc read: which keys, their ranges, the
 *                 load-until-it-works rule.
 *   credential.c  the body split, the password bounds, PBKDF2 and hex.
 *   account.c     the two database exchanges: the login projection and the
 *                 registration transaction under the semaphore.
 *
 * The line that matters is between the last two: credential.c is pure and
 * testable with nothing running, account.c needs a runner, and #54's sections
 * fall on it.
 *
 * NONE OF THESE HOLD STATE ABOUT WHETHER A DEPENDENCY IS REACHABLE.
 * authconf.c's frozen config is not an exception: it caches a SUCCESS, never a
 * failure, so an operator's fix reaches a live session on its next attempt.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* === authconf.c: the .tetrishrc values this library reads ================= */

/**
 * The five keys bin/session takes out of .tetrishrc.
 *
 * Three are libtetrisauth's own and two are the launcher's, read here because
 * the login path is the other end of them. Full tables and their owners are in
 * docs/libtetrisauth.md.
 */
typedef struct {
  int max_attempts;       /**< auth_max_attempts, 1..100. */
  int token_ttl;          /**< auth_token_ttl, 60..31536000 seconds. */
  int pbkdf2_iters;       /**< auth_pbkdf2_iters, 1..10000000. */
  int db_timeout_ms;      /**< db_timeout, in milliseconds. */
  char db_sock[PATH_MAX]; /**< db_ipc, used as written. */
} auth_conf_t;

/** The configuration in force. NEVER NULL: before any successful load it holds
 * the built-in defaults, which is what makes auth_max_attempts govern the
 * guest-reachable path even when the file could not be read at all. */
const auth_conf_t *auth_conf(void);

/**
 * Reads the file if it has not been read successfully yet.
 *
 * Called at the top of the LOGIN and REGISTER arms and nowhere else, so GUEST
 * and gameplay are untouched by a missing config (#60 decision 3) and a guest
 * still reaches a playable game.
 *
 * @returns 0 if usable values are in force, -1 if the file could not be read
 *          or carried an out-of-range value. On -1 the caller answers 500, the
 *          defaults stay in force, and the next attempt tries the file again -
 *          so creating or fixing it repairs live sessions with no restart.
 *          Once a load succeeds the values FREEZE, so a running session never
 *          sees its attempt cap change underneath it.
 */
int auth_conf_load(void);

/* === credential.c: the body, the username rules, the hash ================ */

/** The two fields of a LOGIN or REGISTER body, as pointers INTO the received
 * frame. Nothing is copied and nothing is NUL-terminated, which is what lets
 * one explicit scrub of the frame cover the plaintext (#48 decision 7). */
typedef struct {
  const char *user;
  size_t user_len;
  const char *pass;
  size_t pass_len;
} cred_t;

/**
 * Splits a credential body at the FIRST LF.
 *
 * Splitting at the first LF is what leaves the password charset completely
 * unconstrained.
 *
 * @param body      Received frame body.
 * @param body_len  Its length.
 * @param out       Receives the two slices.
 * @returns 0, or -1 when there is no body, no LF, or an empty field - all 400.
 */
int cred_split(const uint8_t *body, uint32_t body_len, cred_t *out);

/**
 * Validates the username and folds it to lowercase into dst.
 *
 * The rule itself is libtetrisutil/playername.h's, shared with the client's form
 * validator. There is deliberately no truncation: two players could truncate
 * to the same roster name, so an over-long name is refused at REGISTER rather
 * than silently renamed here.
 *
 * @param c    Split credentials.
 * @param dst  Receives the folded name.
 * @param cap  Must be MAX_PLAYER_NAME.
 * @returns 0, or -1 for a name outside the allowlist or the length, which is
 *          400.
 */
int cred_name(const cred_t *c, char *dst, size_t cap);

/** #47 decision 8's 8..128 bytes, enforced at REGISTER ONLY. LOGIN hashes
 * whatever it is handed, because rejecting a short password there would make
 * every existing account unloggable the day the minimum is raised. */
int cred_password_ok_for_register(const cred_t *c);

/**
 * PBKDF2-HMAC-SHA256 of the password against a hex salt.
 *
 * @param c             Split credentials.
 * @param salt_hex      Salt as lowercase hex.
 * @param salt_hex_len  Its length.
 * @param iters         Iteration count. A parameter rather than a constant
 *                      because it comes from the ROW on login and from the rc
 *                      file on registration, and those differ the day the
 *                      count is raised - which is why #47 gave the table an
 *                      iters column.
 * @param out_hex       Receives 64 lowercase hex characters plus a NUL.
 * @param cap           Capacity of out_hex.
 * @returns 0, or -1 on a malformed salt, iters below 1, or a small buffer.
 */
int cred_hash(const cred_t *c, const char *salt_hex, size_t salt_hex_len,
              int iters, char *out_hex, size_t cap);

/** 16 random bytes as 32 lowercase hex characters plus a NUL. cap must be at
 * least 33. Returns 0 or -1. */
int cred_new_salt(char *out_hex, size_t cap);

/* === account.c: the database ============================================= */

/**
 * What the database said about one credential pair.
 *
 * ACCT_UNAVAILABLE is every way the exchange can fail to produce an answer -
 * no socket, refused connect, expired deadline, a reply that is not a table -
 * because they are one thing to the client ("the account service is
 * unreachable"), answered with 500 and a guest fallback.
 */
typedef enum {
  ACCT_OK = 0,
  ACCT_NO_USER,      /**< LOGIN: no row with that name. 404. */
  ACCT_BAD_PASSWORD, /**< LOGIN: row found, digest mismatch. 401. */
  ACCT_TAKEN,        /**< REGISTER: the name is already registered. 409. */
  ACCT_UNAVAILABLE   /**< No answer within the deadline. 500. */
} acct_status_t;

/**
 * Looks up name and verifies the password against the stored digest, at the
 * iteration count THAT ROW was written at.
 *
 * Returns ACCT_NO_USER without hashing anything: #47 decision 13 made 404 and
 * 401 distinguishable, so the dummy hash that used to hide a miss behind equal
 * latency is not built. The timing split is intended behaviour, not a leak.
 *
 * @param name  Folded username.
 * @param c     Split credentials.
 * @param id    Receives the user id on ACCT_OK.
 */
acct_status_t account_login(const char *name, const cred_t *c, long long *id);

/**
 * Registers name, serialized against every other session process by the named
 * semaphore.
 *
 * PBKDF2 runs BEFORE the semaphore is taken, so 65 ms of hashing is not held
 * against everybody else's turn and the database deadline covers only the
 * exchange.
 *
 * @param name  Folded username.
 * @param c     Split credentials.
 * @param id    Receives the new user id on ACCT_OK.
 */
acct_status_t account_register(const char *name, const cred_t *c,
                               long long *id);

#endif /* LIBTETRISAUTH_TAUTH_PRIV_H */
