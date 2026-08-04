#ifndef LIBTETRISDB_H
#define LIBTETRISDB_H

/*
 * tetrisdb.h - SimpleDB access for a tetriSH daemon.
 *
 * SimpleDB (db/) is a Java library with no server: the only way to reach it
 * from C is to run simpledb.PipeRunner as a child process and speak a
 * line-oriented request/response protocol over its stdin/stdout. This module
 * is that child, its pipes, and the single thread allowed to touch them,
 * behind an interface whose whole surface is "hand me a statement".
 *
 * Two constraints from db/docs/c-daemon-integration.md are not style
 * preferences but correctness requirements, and both are met here rather than
 * by every caller remembering them:
 *
 *   1. Exactly one thread may drive the pipe. The protocol has no request ids,
 *      so two threads writing statements would interleave on the wire, and
 *      SimpleDB has no locking layer to make concurrent execution safe either.
 *      tdb_submit() is therefore a queue push, callable from any thread, and a
 *      private worker thread is the only thing that ever sees the pipe.
 *
 *   2. A table is owned by exactly one daemon process. Each PipeRunner caches
 *      pages in its own private BufferPool with no cross-process invalidation,
 *      so a table written here must never be read or written by another
 *      process while this handle is alive. tetrislogd owns "log" on that
 *      basis; nothing else may touch it.
 *
 * The submit path inherits libcommon/logmsg.h's contract: it never blocks and
 * never fails loudly. A wedged child or a full queue costs dropped statements
 * (counted, see tdb_dropped()), never a stalled caller - a daemon must not
 * stop serving because its database is slow.
 */

#include <limits.h>
#include <stddef.h>

/* One connection: a PipeRunner child, its pipes, the queue, and the worker
 * thread. Opaque - every field is owned by the worker or the queue lock. */
typedef struct tdb tdb_t;

/* How to reach SimpleDB. Fixed buffers so an rc-file overlay can rewrite a
 * field in place, matching logd_opts_t. */
typedef struct {
    char   dir[PATH_MAX];      /* directory holding catalog.txt and *.dat */
    char   jar[PATH_MAX];      /* simpledb.jar to put on the classpath */
    char   java[PATH_MAX];     /* java binary, resolved through PATH */
    size_t queue_cap;          /* pending statements before dropping; 0 = default */
} tdb_opts_t;

/*
 * Seed *opts with the built-in defaults (db/dist/simpledb.jar, "java", a
 * 256-deep queue). Always call this before overlaying rc-file values, so a
 * field the file omits still has a usable value.
 *
 * dir is deliberately left empty: two daemons accepting one built-in
 * directory would share a catalog and eventually a .dat, which is the
 * corruption invariant 2 exists to prevent. The caller must name its own
 * directory, and tdb_ensure_table()/tdb_start() fail loudly if it does not.
 */
void tdb_opts_default(tdb_opts_t *opts);

/*
 * Ensure a table exists, creating it if it does not.
 *
 * SimpleDB's SQL parser has no CREATE TABLE: a table is a line in catalog.txt
 * naming its columns plus a <name>.dat heap file beside it. Creating one is
 * therefore a filesystem operation, and it must happen before tdb_start(),
 * because PipeRunner reads the catalog once at startup and never again.
 *
 * schema is the column list as it appears inside the catalog's parentheses,
 * e.g. "id int, pid int, ts int, status string, msg string". If the catalog
 * already carries a line for name, this call leaves it completely alone - it
 * does not compare or migrate the schema, so changing an existing table's
 * columns is a manual operation (edit catalog.txt, delete the .dat).
 *
 * A newly created .dat gets one empty page, not zero bytes: a zero-byte heap
 * file can be inserted into but not scanned (a read of it aborts with
 * "Failed to read page 0"), and a table nobody has written to yet should
 * still answer a query with no rows.
 *
 * Returns 0 if the table exists on return, -1 on failure (message on stderr).
 */
int tdb_ensure_table(const char *dir, const char *name, const char *schema);

/*
 * Spawn the PipeRunner child, wait for its startup handshake, and start the
 * worker thread. Returns NULL on failure (message on stderr), in which case
 * no child is left behind.
 *
 * Blocks until the child reports readiness, which costs a JVM startup - do
 * this once, during daemon startup, not per statement.
 *
 * probe, if non-NULL, is one statement run synchronously between the
 * handshake and the worker's first breath, with its response text copied into
 * body. It is the only way to see a statement's output, and it is safe
 * precisely because no other thread can be using the pipe yet: a daemon that
 * needs to read the table it is about to write (a sequence counter, a
 * high-water mark) does it here, once, and never races the worker. Passing a
 * statement whose output matters after startup is not supported by design.
 */
tdb_t *tdb_start(const tdb_opts_t *opts, const char *probe, char *body,
                 size_t body_cap);

/*
 * Queue one SQL statement (ending in ';') for execution. Callable from any
 * thread; the statement is copied, so sql may be a stack buffer.
 *
 * Returns 0 if the statement was queued, -1 if it was dropped because the
 * queue was full or the connection is shutting down. Callers are expected to
 * ignore the result for best-effort writes and check tdb_dropped() at exit.
 */
int tdb_submit(tdb_t *db, const char *sql);

/*
 * Quote a string for use as a SQL literal: copies src into dst wrapped in
 * single quotes, doubling any quote inside it, and truncating so the result
 * fits dst (cap includes the terminating NUL).
 *
 * This is what keeps sender-supplied text from breaking out of its literal
 * and appending statements of its own. Note the doubling survives into
 * storage - SimpleDB's parser accepts '' as an escape but does not collapse
 * it back to one quote on read, so a stored message shows both.
 *
 * Returns dst.
 */
char *tdb_quote(char *dst, size_t cap, const char *src);

/* Statements dropped since tdb_start(): queue full, or submitted during
 * shutdown. Non-zero means the table is missing records. */
unsigned long tdb_dropped(tdb_t *db);

/* Statements the child reported as failed (<<END error>>) since tdb_start().
 * Non-zero means SQL was rejected - a schema mismatch or a malformed
 * statement - and is worth reporting even though nothing blocked on it. */
unsigned long tdb_errors(tdb_t *db);

/*
 * Drain whatever is already queued, shut the child down cleanly by closing
 * its stdin (the only shutdown path that flushes dirty pages), reap it, and
 * free the handle. Safe to call with NULL.
 *
 * The final counters are written to dropped and errors (either may be NULL),
 * because the handle is gone by the time this returns and the drain itself
 * can add to both.
 *
 * Blocks for as long as the drain takes, so call it from the daemon's exit
 * path, not from a signal handler.
 */
void tdb_stop(tdb_t *db, unsigned long *dropped, unsigned long *errors);

#endif /* LIBTETRISDB_H */
