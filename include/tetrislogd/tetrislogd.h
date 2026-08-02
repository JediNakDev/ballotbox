#ifndef TETRISLOGD_H
#define TETRISLOGD_H

/*
 * tetrislogd.h - internal interface of the tetriSH logging daemon.
 *
 * tetrislogd owns one AF_UNIX datagram socket and one log file. Everything it
 * does is a consequence of one loop: read a datagram, validate it, append a
 * line, flush. Splitting that into a sink (this interface) and a main that
 * only parses arguments and installs signal handlers keeps the receive path
 * testable without a process boundary.
 *
 * The wire format lives in libcommon/logmsg.h, shared with every sender.
 */

#include <limits.h>

#include "libcommon/logmsg.h"
#include "libtetrisdb/tetrisdb.h"

/* What the daemon was asked to do. Owned by main, borrowed by the sink.
 * Fixed buffers (not const char *) so config.c can overwrite a field in
 * place while layering rc-file values under CLI values under defaults. */
typedef struct {
    char        socket_path[PATH_MAX]; /* AF_UNIX datagram socket to bind */
    char        log_path[PATH_MAX];    /* log file, or "-" for stdout */
    log_level_t min_level;             /* records ranking below this are dropped */
    int         echo;                  /* also mirror each record to stderr */

    /* Mirroring into SimpleDB. Off by default: the file sink is the source
     * of truth and must keep working on a machine with no JVM, so the
     * database is something an operator opts into. When on, tetrislogd is
     * the sole owner of the "log" table (libtetrisdb/tetrisdb.h, invariant
     * 2) - no other process may read or write it while the daemon runs. */
    int         db_enable;
    tdb_opts_t  db;
} logd_opts_t;

/*
 * Overlay directives from the rc file at rc_path onto *opts, replacing only
 * the fields the file mentions. A field the file does not mention is left as
 * the caller set it - so calling this after seeding *opts with the built-in
 * defaults gives exactly "default, unless the rc file says otherwise". A
 * missing file is not an error: *opts is left untouched.
 *
 * Recognised directives: log_ipc (socket path), log_path (log file path),
 * log_level (minimum severity: debug|info|warn|error), log_db (on|off),
 * log_db_dir (directory holding catalog.txt and log.dat), log_db_jar
 * (simpledb.jar), log_db_java (java binary), log_db_queue (pending
 * statements before records are dropped). Every other line -
 * including PATH= and bare commands, which the same file may carry for the
 * shell - is silently ignored: this rc file is shared with tetrish's
 * .tetrishrc, and tetrislogd only picks out the keys it understands.
 */
void logd_load_rc(const char *rc_path, logd_opts_t *opts);

/* Counters reported at shutdown, so an operator can tell a quiet log from a
 * lossy one. */
typedef struct {
    unsigned long received;  /* well-formed records appended */
    unsigned long filtered;  /* dropped by min_level */
    unsigned long malformed; /* wrong datagram size, or unknown level */
    unsigned long truncated; /* msg was not NUL-terminated by the sender */
    unsigned long db_dropped;/* records the database queue could not take */
    unsigned long db_errors; /* INSERTs SimpleDB rejected */
} logd_stats_t;

/*
 * Bind the socket, open the log file, and serve until logd_stop() is called
 * (from a signal handler) or an unrecoverable error occurs. The socket file is
 * removed on the way out.
 *
 * Returns 0 on a clean shutdown, -1 if setup failed. Statistics are written to
 * *stats (if non-NULL) whether or not the run succeeded.
 */
int logd_run(const logd_opts_t *opts, logd_stats_t *stats);

/*
 * Ask the running logd_run() to return. Async-signal-safe: it only writes a
 * volatile sig_atomic_t flag, which the receive loop notices when recvfrom()
 * fails with EINTR.
 */
void logd_stop(void);

/*
 * Ask the running logd_run() to reopen its log file before the next record.
 * This is how log rotation works: move the file aside, then SIGHUP the daemon.
 * Async-signal-safe, same mechanism as logd_stop().
 */
void logd_reopen(void);

#endif /* TETRISLOGD_H */
