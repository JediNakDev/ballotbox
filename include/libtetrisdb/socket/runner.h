#ifndef LIBTETRISDB_SOCKET_RUNNER_H
#define LIBTETRISDB_SOCKET_RUNNER_H

/**
 * @file socket/runner.h
 * @brief Starting the shared SocketRunner, and knowing when it is up.
 *
 * The other half of libtetrisdb/socket/db.h: that header connects to a runner,
 * this one puts a runner there to connect to. Separate headers because they
 * have separate callers - every session process connects, exactly one one-shot
 * command starts.
 *
 * MECHANICS HERE, POLICY IN THE CALLER. What lives here is what "start a
 * SocketRunner" means to anyone: the argv, the fork, the detach, the lock and
 * waiting for it to answer. What stays in bin/tetrisdb is everything true only
 * of a tetriSH installation - refusing when catalog.txt already declares a
 * table named log, recreating libtetrisauth's registration semaphore, creating
 * the tables, provisioning the JWT secret, unlinking the stale socket, the exit
 * codes and the operator's report. Full ordering in docs/adr/0002.
 *
 * The reason the spawn is here rather than in the launcher is ADR 0001's:
 * resolving and validating the java/jar pair has to be shared with
 * tdb_start(), or the version trap gets two homes again.
 *
 * NOTHING HERE MAY BE CACHED. tdb_runner_wait() decides what an operator is
 * told and what a one-shot command exits with. It gates nothing, and no
 * component may hold state describing whether the runner is reachable: a
 * probe's answer expires immediately, and code that remembers it can refuse a
 * login that would have worked (#52 decision 6).
 */

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

/** Complete db_ namespace, kept beside the parser that owns its meaning.
 * NULL-terminated so validators and drift tests cannot disagree about a
 * separately maintained count. */
extern const char *const tdb_rc_keys[];

/** How to start a runner. Fixed buffers so an rc overlay can rewrite a field
 * in place, matching tdb_opts_t and tdb_socket_opts_t. */
typedef struct {
    char dir[PATH_MAX];  /**< Data directory holding catalog.txt and *.dat. */
    char jar[PATH_MAX];  /**< simpledb.jar for the classpath. */
    char java[PATH_MAX]; /**< java binary, resolved through PATH. */
    char ipc[PATH_MAX];  /**< Unix socket to bind; the db_ipc rc key. */
    int  sessions;       /**< Concurrent sessions served; 0 = the default. */
    int  recover;        /**< Run the startup recovery pass; see below. */
} tdb_runner_opts_t;

/**
 * Seeds *opts with libtetrisdb/socket/conf.h's defaults.
 *
 * Unlike tdb_opts_default(), dir defaults to var/db: the one-shot launcher
 * protects that directory with an inherited flock and refuses a catalog that
 * declares tetrislogd's table, so its two guards replace the pipe module's
 * need to leave ownership unnamed.
 *
 * RECOVERY DEFAULTS ON AND SHOULD STAY ON. SimpleDB flushes a transaction's
 * pages before writing its commit record, so a crash in that window leaves
 * pages belonging to a transaction that never committed, and the startup pass
 * undoes exactly those. It is a no-op on a clean log. The field exists only
 * because SimpleDB's write-ahead log is a file named "log" in the CURRENT
 * DIRECTORY, shared by every runner started from the same place, so a test
 * that creates a fresh table each run recovers a previous run's records onto
 * it. That is a test's problem, not an operator's.
 *
 * @param opts  Receives the defaults.
 */
void tdb_runner_opts_default(tdb_runner_opts_t *opts);

/**
 * Overlays db_ directives from rc_path onto *opts, ignoring other namespaces.
 *
 * The update is transactional: *opts is unchanged on failure. db_timeout is
 * validated even though the launcher does not consume it, because this module
 * owns the complete db_ namespace and bin/tetrisdb is its validator.
 *
 * @param rc_path  File to read.
 * @param opts     Seeded with defaults by the caller; overlaid here.
 * @returns The directive count, or -1 when the file is missing, a db_ value is
 *          invalid, or an unknown db_ key is present (message on stderr).
 */
int tdb_runner_opts_load(const char *rc_path, tdb_runner_opts_t *opts)
    __attribute__((warn_unused_result));

/**
 * Takes the directory lock that makes one runner per data directory.
 *
 * Called by a launcher before any other startup step, and held across
 * tdb_runner_spawn() so the JVM inherits it: the kernel then releases it
 * exactly when the runner dies, including after SIGKILL, with no cleanup path
 * to get wrong.
 *
 * The returned descriptor is guaranteed to satisfy tdb_runner_spawn()'s two
 * requirements - above fd 2, and not FD_CLOEXEC. Those used to be prose in
 * this header and an F_DUPFD dance in the caller; they are enforced here
 * because this is the module that imposes them.
 *
 * A socket probe is not a substitute: it races, and two runners reached
 * through two socket paths corrupt one directory just as thoroughly. ADR 0002.
 *
 * @param db_dir  Data directory; created if absent.
 * @param path    Receives the lockfile's path, for the caller's messages.
 * @param cap     Capacity of path.
 * @returns The locked descriptor, or -1 if the directory is unusable or a
 *          runner already owns it (message on stderr). Close it to release.
 */
int tdb_runner_lock(const char *db_dir, char *path, size_t cap)
    __attribute__((warn_unused_result));

/**
 * Forks, detaches and execs the runner.
 *
 * Returning rather than exec'ing in place is what lets the caller hold a lock
 * across the start: A FILE LOCK HELD BY THE CALLER IS INHERITED BY THE JVM, so
 * the lock lives exactly as long as the runner does, with no cleanup path to
 * get wrong. That is ADR 0002's central trick, and tdb_runner_lock() is what
 * produces a descriptor fit for it.
 *
 * Refuses before forking if the jar is unreadable, java does not run, or the
 * catalog does not exist. The last is not a courtesy: a runner reads
 * catalog.txt once at startup and never again, so a table created afterwards
 * is invisible until the next restart.
 *
 * The child is NOT reaped here and is not a zombie the caller must care about:
 * it is setsid'd and outlives the caller, which exits.
 *
 * @param opts    Where the runner and its data are.
 * @param err_fd  If >= 0, becomes the child's stderr, where the runner reports
 *                "listening on ..." and any operator message; the caller
 *                decides whether that is a log file or the terminal. stdin and
 *                stdout go to /dev/null, because SocketRunner writes nothing to
 *                stdout and a JVM holding the launcher's terminal open would
 *                outlive the command that started it.
 * @returns The child's pid, or -1 with no child left behind (message on
 *          stderr).
 */
pid_t tdb_runner_spawn(const tdb_runner_opts_t *opts, int err_fd);

/**
 * Waits until the runner answers on ipc.
 *
 * A bare connect, not a greeting: the greeting only arrives when a session
 * slot is free, so waiting for one would report a busy runner as a dead one.
 * The connect costs the runner one accept and an immediate EOF.
 *
 * @param ipc         Socket to connect to.
 * @param pid         The child to watch, or -1 when the caller has none - a
 *                    `check` verb asking whether SOMEBODY's runner is serving.
 *                    A real pid whose child has died is reaped here and is no
 *                    longer the caller's to wait on; otherwise it is
 *                    untouched.
 * @param timeout_ms  <= 0 means the 10 s default, which is a cold JVM plus
 *                    recovery with room to spare.
 * @returns 0 when a connect succeeds, -1 on timeout or if the child died first
 *          (message on stderr, naming the JDK-mismatch trap when the child
 *          exited immediately).
 */
int tdb_runner_wait(const char *ipc, pid_t pid, int timeout_ms);

#endif /* LIBTETRISDB_SOCKET_RUNNER_H */
