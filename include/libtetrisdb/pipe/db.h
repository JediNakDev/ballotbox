#ifndef LIBTETRISDB_PIPE_DB_H
#define LIBTETRISDB_PIPE_DB_H

/**
 * @file pipe/db.h
 * @brief A SimpleDB table a daemon owns outright, written best-effort.
 *
 * Called by tetrislogd, its only consumer. SimpleDB (db/) is a Java library
 * with no server, so reaching it from C means running a runner process and
 * speaking a line-oriented protocol to it. This module runs simpledb.PipeRunner
 * as a CHILD over its stdin and stdout, and is that child, its pipes and the
 * single thread allowed to touch them, behind an interface whose whole surface
 * is "hand me a statement".
 *
 * IT NEVER BLOCKS, NEVER FAILS LOUDLY, AND CANNOT READ. If you need an answer
 * rather than a best-effort write, libtetrisdb/socket/db.h is the other
 * contract: one connection to the shared SocketRunner, blocking, with a
 * deadline and a readable result. The two share the wire and nothing else.
 *
 * Two constraints from db/docs/c-daemon-integration.md are correctness
 * requirements rather than style, and both are met here rather than by every
 * caller remembering them:
 *
 *   1. Exactly one thread may drive the pipe. The protocol has no request ids,
 *      so two threads would interleave on the wire, and SimpleDB has no
 *      locking layer making concurrent execution safe either. tdb_submit() is
 *      therefore a queue push, callable from any thread, and a private worker
 *      is the only thing that ever sees the pipe.
 *   2. A table is owned by exactly one daemon process. Each PipeRunner caches
 *      pages in its own BufferPool with no cross-process invalidation, so a
 *      table written here must never be touched by another process while this
 *      handle is alive. tetrislogd owns "log" on that basis.
 */

#include <limits.h>
#include <stddef.h>

/** One connection: a PipeRunner child, its pipes, the queue and the worker
 * thread. Opaque - every field is owned by the worker or the queue lock. */
typedef struct tdb tdb_t;

/** How to reach SimpleDB. Fixed buffers so an rc overlay can rewrite a field
 * in place, matching logd_opts_t. */
typedef struct {
    char   dir[PATH_MAX];  /**< Directory holding catalog.txt and *.dat. */
    char   jar[PATH_MAX];  /**< simpledb.jar for the classpath. */
    char   java[PATH_MAX]; /**< java binary, resolved through PATH. */
    size_t queue_cap;      /**< Pending statements before dropping; 0 = default. */
} tdb_opts_t;

/**
 * Seeds *opts with the built-in defaults.
 *
 * Call before overlaying rc values, so a field the file omits still has a
 * usable one.
 *
 * @param opts  Receives the defaults. dir is deliberately left empty: two
 *              daemons accepting one built-in directory would share a catalog
 *              and eventually a .dat, which is the corruption invariant 2
 *              exists to prevent, so tdb_start() fails loudly on an unset one.
 */
void tdb_opts_default(tdb_opts_t *opts);

/**
 * Spawns the PipeRunner child, waits for its startup handshake, and starts the
 * worker thread.
 *
 * Called once during daemon startup, never per statement: it blocks for a JVM
 * startup.
 *
 * @param opts      Where SimpleDB is.
 * @param probe     If non-NULL, one statement run synchronously between the
 *                  handshake and the worker's first breath. It is the only way
 *                  to see a statement's output, and it is safe precisely
 *                  because no other thread can be using the pipe yet - a
 *                  daemon that needs to read the table it is about to write
 *                  does it here, once, and never races the worker. Reading
 *                  output after startup is not supported by design.
 * @param body      Receives the probe's response text.
 * @param body_cap  Capacity of body.
 * @returns The handle, or NULL on failure with no child left behind (message
 *          on stderr).
 */
tdb_t *tdb_start(const tdb_opts_t *opts, const char *probe, char *body,
                 size_t body_cap);

/**
 * Queues one SQL statement for execution.
 *
 * Callable from any thread; the statement is copied, so sql may be a stack
 * buffer. Callers are expected to ignore the result for best-effort writes and
 * check tdb_dropped() at exit - a daemon must not stop serving because its
 * database is slow.
 *
 * @param db   The handle.
 * @param sql  One statement, ending in ';'.
 * @returns 0 if queued, -1 if dropped because the queue was full or the
 *          connection is shutting down.
 */
int tdb_submit(tdb_t *db, const char *sql);

/** Statements dropped since tdb_start(): queue full, or submitted during
 * shutdown. Non-zero means the table is missing records. */
unsigned long tdb_dropped(tdb_t *db);

/** Statements the child reported as failed since tdb_start(). Non-zero means
 * SQL was rejected - a schema mismatch or a malformed statement - and is worth
 * reporting even though nothing blocked on it. */
unsigned long tdb_errors(tdb_t *db);

/**
 * Drains the queue, shuts the child down cleanly, reaps it and frees the
 * handle.
 *
 * Called from the daemon's exit path, not a signal handler: it blocks for as
 * long as the drain takes.
 *
 * Closing stdin is the only shutdown THIS CHILD offers that flushes dirty
 * pages - PipeRunner flushes and exits 0 on EOF and installs no shutdown hook,
 * so signalling it instead loses whatever is unwritten. That is a fact about
 * this transport and NOT about SimpleDB: SocketRunner does flush on SIGTERM,
 * from a shutdown hook. ADR 0001 and ADR 0002 both recorded the unscoped
 * version of this claim as one to correct here.
 *
 * @param db       The handle; NULL is safe.
 * @param dropped  Receives the final drop count; may be NULL.
 * @param errors   Receives the final error count; may be NULL. Both are
 *                 out-parameters because the handle is gone by the time this
 *                 returns and the drain itself can add to either.
 */
void tdb_stop(tdb_t *db, unsigned long *dropped, unsigned long *errors);

#endif /* LIBTETRISDB_PIPE_DB_H */
