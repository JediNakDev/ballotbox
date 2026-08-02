/*
 * sink.c - the receive path of tetrislogd: socket, log file, and the loop.
 *
 * Everything a datagram passes through on its way to a line of text lives
 * here. The loop itself is single-threaded and blocking: one datagram is one
 * unit of work, ordering across senders is whatever the kernel queued, and a
 * flush after every record means a crash loses at most the record in flight.
 * Concurrency would buy nothing - the bottleneck is the disk, and datagram
 * delivery is already serialised by the socket's receive queue.
 */

#include "tetrislogd/tetrislogd.h"

#include "libtetrisdb/tetrisdb.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Receive queue depth. A burst from every session worker at once (a wave of
 * game-over records, say) must not be dropped just because the daemon is
 * mid-fsync, so ask for far more than the default. Advisory: the kernel may
 * clamp it, and failure is not fatal. */
#define LOGD_RCVBUF (256 * 1024)

/* Set by signal handlers, read by the loop. sig_atomic_t is the only type the
 * standard lets a handler touch here. */
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reopen;

void logd_stop(void) { g_stop = 1; }

void logd_reopen(void) { g_reopen = 1; }

/*
 * Bind an AF_UNIX datagram socket at path.
 *
 * A stale socket file from a previous run must be removed before bind(), but
 * only if it really is a socket: refusing to unlink anything else keeps a
 * mistyped --socket from destroying a real file.
 */
static int sink_bind(const char *path) {
  struct sockaddr_un addr;
  struct stat st;

  if (strlen(path) >= sizeof(addr.sun_path)) {
    fprintf(stderr, "tetrislogd: socket path too long (max %zu): %s\n",
            sizeof(addr.sun_path) - 1, path);
    return -1;
  }

  if (lstat(path, &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      fprintf(stderr, "tetrislogd: refusing to replace non-socket %s\n", path);
      return -1;
    }
    if (unlink(path) < 0) {
      fprintf(stderr, "tetrislogd: cannot remove stale socket %s: %s\n", path,
              strerror(errno));
      return -1;
    }
  } else if (errno != ENOENT) {
    fprintf(stderr, "tetrislogd: cannot stat %s: %s\n", path, strerror(errno));
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    fprintf(stderr, "tetrislogd: socket: %s\n", strerror(errno));
    return -1;
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

  int bufsz = LOGD_RCVBUF;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, strlen(path) + 1);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "tetrislogd: bind %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }

  /* bind() applies the umask, which would typically leave the socket
   * unwritable by other users. Every local tetriSH process must be able to
   * report, so widen it explicitly; the socket only ever accepts fixed-size
   * records and grants no read access to the log itself. */
  if (chmod(path, 0666) < 0)
    fprintf(stderr, "tetrislogd: chmod %s: %s (senders may be denied)\n", path,
            strerror(errno));

  return fd;
}

/* Open (or reopen) the log file in append mode. "-" means stdout, which is
 * what a supervisor that captures our output wants. Returns NULL on failure. */
static FILE *sink_open_log(const char *path) {
  if (strcmp(path, "-") == 0)
    return stdout;

  /* O_APPEND makes every write atomic with respect to other appenders, so a
   * rotation script or a second daemon cannot overwrite our records. 0640:
   * logs carry usernames and addresses, so keep them off world-readable. */
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
  if (fd < 0) {
    fprintf(stderr, "tetrislogd: open %s: %s\n", path, strerror(errno));
    return NULL;
  }

  FILE *f = fdopen(fd, "a");
  if (f == NULL) {
    fprintf(stderr, "tetrislogd: fdopen %s: %s\n", path, strerror(errno));
    close(fd);
    return NULL;
  }
  return f;
}

static void sink_close_log(FILE *f) {
  if (f != NULL && f != stdout)
    fclose(f);
}

/*
 * Make one sender-supplied string safe to write into a line-oriented log.
 *
 * A record arrives from another process, so its text is untrusted: an embedded
 * newline would let a sender forge whole log lines, and a bare escape sequence
 * would execute in whoever tails the file. Replace every C0 control byte and
 * DEL with '.', leaving bytes >= 0x80 alone so UTF-8 survives intact.
 */
static void sanitise(char *s) {
  for (; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 || c == 0x7f)
      *s = '.';
  }
}

/* === The database mirror ===
 *
 * Every record that reaches the file also becomes one row in SimpleDB. The
 * file remains the source of truth: it is written first, synchronously, and a
 * database that is off, unreachable or wedged changes nothing about it.
 *
 * Only the receive loop touches this struct, so next_id needs no lock - the
 * worker thread inside libtetrisdb never looks at it, it only sees finished
 * statements.
 */

/* Column list for the log table, as it appears in catalog.txt. Changing this
 * does not migrate an existing table (see tdb_ensure_table): delete log.dat
 * and the catalog line first. */
#define LOGD_DB_TABLE "log"
#define LOGD_DB_SCHEMA "id int, pid int, ts int, status string, msg string"

/* SimpleDB's Type.STRING_LEN. Longer text is silently cut by StringField, so
 * truncate here instead, where the cut is visible and deliberate - the full
 * text is still in the log file. */
#define LOGD_DB_STRING_MAX 128

typedef struct {
  tdb_t *db;    /* NULL when mirroring is off or failed to start */
  long next_id; /* id for the next row; see mirror_seed_id() */
} logd_mirror_t;

/*
 * Pick the id to start from out of the response to "select max(id) from log".
 *
 * The body is the parser's tabular output: a header line, a line of dashes,
 * then the value. Scanning for the dashes and taking the first integer after
 * them is enough, and anything unexpected (an empty table prints no value at
 * all, a failed probe prints an error) falls back to 1 - at worst ids restart,
 * which costs a duplicate, since SimpleDB has no uniqueness constraints to
 * violate anyway.
 */
static long mirror_seed_id(const char *body) {
  const char *p = body;
  int after_rule = 0;

  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

    if (after_rule) {
      char *end;
      long v = strtol(p, &end, 10);
      if (end != p && v >= 0)
        return v + 1;
    } else if (len >= 3 && strncmp(p, "---", 3) == 0) {
      after_rule = 1;
    }

    if (eol == NULL)
      break;
    p = eol + 1;
  }
  return 1;
}

/* Bring up the mirror: create the table if this is a first run, start the
 * child, and seed the id sequence from whatever is already stored. A failure
 * is reported and then tolerated - the daemon's job is the log file. */
static void mirror_open(logd_mirror_t *mirror, const logd_opts_t *opts) {
  char body[1024];

  mirror->db = NULL;
  mirror->next_id = 1;

  if (!opts->db_enable)
    return;

  if (tdb_ensure_table(opts->db.dir, LOGD_DB_TABLE, LOGD_DB_SCHEMA) < 0) {
    fprintf(stderr, "tetrislogd: database mirroring disabled\n");
    return;
  }

  mirror->db = tdb_start(&opts->db, "select max(id) from " LOGD_DB_TABLE ";",
                         body, sizeof(body));
  if (mirror->db == NULL) {
    fprintf(stderr, "tetrislogd: database mirroring disabled\n");
    return;
  }
  mirror->next_id = mirror_seed_id(body);
}

/*
 * Queue one record as an INSERT.
 *
 * m->msg has already been sanitised of control characters by the caller, but
 * it is still sender-supplied text, so it goes through tdb_quote() rather
 * than straight into the statement - a message containing a quote must cost a
 * doubled quote in storage, never a second statement.
 */
static void mirror_write(logd_mirror_t *mirror, const log_msg_t *m,
                         time_t now) {
  if (mirror->db == NULL)
    return;

  /* Level names are fixed-width for the file's columns ("INFO "); the
   * database wants the bare word. */
  char status[16];
  snprintf(status, sizeof(status), "%s", log_level_str(m->level));
  for (size_t i = strlen(status); i > 0 && status[i - 1] == ' '; i--)
    status[i - 1] = '\0';

  char text[LOGD_DB_STRING_MAX + 1];
  snprintf(text, sizeof(text), "%s", m->msg);

  /* Worst case every character is a quote and doubles. */
  char q_status[sizeof(status) * 2 + 3];
  char q_text[sizeof(text) * 2 + 3];
  tdb_quote(q_status, sizeof(q_status), status);
  tdb_quote(q_text, sizeof(q_text), text);

  char sql[LOGD_DB_STRING_MAX * 2 + 256];
  snprintf(sql, sizeof(sql),
           "insert into " LOGD_DB_TABLE " values (%ld, %d, %ld, %s, %s);",
           mirror->next_id, (int)m->pid, (long)now, q_status, q_text);

  /* Best-effort by contract: a full queue drops the row and counts it. */
  (void)tdb_submit(mirror->db, sql);
  mirror->next_id++;
}

/* Format the local time of now as "YYYY-MM-DD HH:MM:SS". */
static void stamp(time_t now, char *buf, size_t cap) {
  struct tm tm;

  /* localtime_r, not localtime: the loop is single-threaded today, but a
   * static-buffer call in a shared path is a trap for the next reader. */
  if (localtime_r(&now, &tm) == NULL ||
      strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm) == 0)
    snprintf(buf, cap, "0000-00-00 00:00:00"); /* clock unusable */
}

/*
 * Append one validated record, and mirror it into the database.
 *
 * The file write comes first and decides the return value: it is the one that
 * can fail in a way the daemon cannot continue past. One timestamp serves
 * both sinks, so a row and its line can never disagree about when a record
 * arrived. Returns 0, or -1 if the log file went bad.
 */
static int emit(FILE *log, const logd_opts_t *opts, logd_mirror_t *mirror,
                const log_msg_t *m) {
  time_t now = time(NULL);
  char ts[32];
  stamp(now, ts, sizeof(ts));

  if (fprintf(log, "[%s] [%s] pid=%d: %s\n", ts, log_level_str(m->level),
              (int)m->pid, m->msg) < 0 ||
      fflush(log) != 0) {
    fprintf(stderr, "tetrislogd: writing %s: %s\n", opts->log_path,
            strerror(errno));
    return -1;
  }

  mirror_write(mirror, m, now);

  if (opts->echo)
    fprintf(stderr, "[%s] [%s] pid=%d: %s\n", ts, log_level_str(m->level),
            (int)m->pid, m->msg);
  return 0;
}

/* Write our own record through the same path senders use, so daemon lifecycle
 * events appear in the log - and in the table - with identical formatting. */
static int emit_self(FILE *log, const logd_opts_t *opts, logd_mirror_t *mirror,
                     log_level_t level, const char *text) {
  log_msg_t m;

  memset(&m, 0, sizeof(m));
  m.pid = getpid();
  m.level = level;
  snprintf(m.msg, sizeof(m.msg), "%s", text);
  return emit(log, opts, mirror, &m);
}

/*
 * Handle at most one datagram. flags is 0 to wait for one, or MSG_DONTWAIT to
 * take one only if it is already queued.
 *
 * Returns 1 if a datagram was consumed, 0 if none was (queue empty, or a
 * signal interrupted the wait), -1 if the log file can no longer be written.
 */
static int serve_one(int fd, FILE *log, const logd_opts_t *opts,
                     logd_mirror_t *mirror, logd_stats_t *st, int flags) {
  /* One byte of slack past the struct: a datagram that fills it is larger
   * than a record and therefore malformed, which recvfrom() could not tell
   * us if the buffer were exactly sizeof(log_msg_t). */
  union {
    log_msg_t m;
    char raw[sizeof(log_msg_t) + 1];
  } buf;

  ssize_t n = recvfrom(fd, buf.raw, sizeof(buf.raw), flags, NULL, NULL);
  if (n < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
      return 0; /* signal arrived, or nothing queued */
    fprintf(stderr, "tetrislogd: recvfrom: %s\n", strerror(errno));
    return -1;
  }

  if (n != (ssize_t)sizeof(log_msg_t)) {
    /* Not one of our records: a foreign sender, a version mismatch, or a
     * truncated write. Count it and keep serving. */
    st->malformed++;
    return 1;
  }

  /* The sender promised a NUL-terminated msg. Enforce it here rather than
   * trusting it: everything downstream treats msg as a C string. */
  if (memchr(buf.m.msg, '\0', sizeof(buf.m.msg)) == NULL) {
    buf.m.msg[sizeof(buf.m.msg) - 1] = '\0';
    st->truncated++;
  }
  sanitise(buf.m.msg);

  if (log_level_rank(buf.m.level) > log_level_rank(LOG_ERROR))
    st->malformed++; /* unknown level: still logged, as "?????" */
  else if (log_level_rank(buf.m.level) < log_level_rank(opts->min_level)) {
    st->filtered++;
    return 1;
  }

  if (emit(log, opts, mirror, &buf.m) < 0)
    return -1;
  st->received++;
  return 1;
}

/* Upper bound on the shutdown drain, so a sender flooding the socket cannot
 * keep a terminating daemon alive indefinitely. */
#define LOGD_DRAIN_MAX 100000

int logd_run(const logd_opts_t *opts, logd_stats_t *stats) {
  logd_stats_t local = {0, 0, 0, 0, 0, 0};
  logd_mirror_t mirror;
  int rc = -1;

  if (stats != NULL)
    *stats = local;

  int fd = sink_bind(opts->socket_path);
  if (fd < 0)
    return -1;

  FILE *log = sink_open_log(opts->log_path);
  if (log == NULL) {
    close(fd);
    unlink(opts->socket_path);
    return -1;
  }

  /* Before the first record, so the startup banner is itself mirrored: a
   * run that wrote no rows should still be visible in the table. */
  mirror_open(&mirror, opts);

  char banner[LOG_MSG_MAX];
  snprintf(banner, sizeof(banner),
           "tetrislogd started: socket=%s level>=%s db=%s", opts->socket_path,
           log_level_str(opts->min_level),
           mirror.db != NULL ? opts->db.dir : "off");
  emit_self(log, opts, &mirror, LOG_INFO, banner);

  while (!g_stop) {
    if (g_reopen) {
      g_reopen = 0;
      /* Rotation: the old file may already have been renamed, so the
       * next record must go to a freshly opened path. Keep serving on
       * the old handle if the reopen fails - losing records is worse. */
      FILE *fresh = sink_open_log(opts->log_path);
      if (fresh != NULL) {
        sink_close_log(log);
        log = fresh;
        emit_self(log, opts, &mirror, LOG_INFO, "log file reopened (SIGHUP)");
      }
    }

    if (serve_one(fd, log, opts, &mirror, &local, 0) < 0)
      goto out;
  }

  /* Records the kernel already accepted were logged from the sender's point
   * of view - they must not be thrown away because a SIGTERM raced them.
   * Drain what is queued (bounded) before closing the file. */
  rc = 0;
  for (long i = 0; i < LOGD_DRAIN_MAX; i++) {
    int r = serve_one(fd, log, opts, &mirror, &local, MSG_DONTWAIT);
    if (r == 1)
      continue;
    if (r < 0)
      rc = -1; /* the log file broke: report a failed run */
    break;
  }

out:
  /* The database counters are read before the final banner so the banner
   * itself can carry them, and the mirror is stopped after it so the banner
   * is the last row of the run - the same ordering the file already has. */
  local.db_dropped = tdb_dropped(mirror.db);
  local.db_errors = tdb_errors(mirror.db);

  snprintf(banner, sizeof(banner),
           "tetrislogd stopping: received=%lu filtered=%lu malformed=%lu "
           "truncated=%lu db_dropped=%lu db_errors=%lu",
           local.received, local.filtered, local.malformed, local.truncated,
           local.db_dropped, local.db_errors);
  emit_self(log, opts, &mirror, LOG_INFO, banner);

  /* Drains the queue and closes the child's stdin, which is what makes
   * SimpleDB flush its dirty pages. Skipping it would lose whatever the
   * worker had not yet executed. The counters it returns are final, and
   * include the banner row queued just above. */
  tdb_stop(mirror.db, &local.db_dropped, &local.db_errors);

  sink_close_log(log);
  close(fd);
  /* Remove our socket so the next run does not have to reason about a stale
   * one, and so senders fail fast (ECONNREFUSED) instead of queueing into a
   * socket nobody reads. */
  unlink(opts->socket_path);

  if (stats != NULL)
    *stats = local;
  return rc;
}
