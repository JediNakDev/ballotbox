#ifndef LIBTETRISDB_SOCKET_DB_H
#define LIBTETRISDB_SOCKET_DB_H

/**
 * @file socket/db.h
 * @brief One connection to the shared SocketRunner, for a caller that needs an
 *        answer.
 *
 * Called by libtetrisauth's login path, its only consumer. This is the
 * opposite contract to pipe/db.h's, in the same library and deliberately not
 * behind the same type: tdb_t is a lossy queue that never blocks and drops
 * rather than stalls, which is correct for logging and wrong for a login,
 * because a dropped login has no rendering for a user sitting in a modal
 * screen. tdb_socket_t is one connection, opened when a statement is ready and
 * closed when the exchange resolves. It blocks, it fails loudly, and it
 * answers within a deadline.
 *
 * Rejected: one handle with a mode flag, which would make every sentence in
 * pipe.h about blocking, failure and threading conditional on how the handle
 * was built. Also rejected: a separate library, which would duplicate the
 * marker reader - the two share wire.c internally and nothing publicly.
 *
 * IT BLOCKS, AND THE DEADLINE IS WHY THAT IS SAFE. A connection queued behind
 * a full --sessions pool waits indefinitely and silently for <<READY>>: no
 * error, no greeting, nothing to poll for. The deadline turns that invisible
 * hang into an ordinary error return. It starts at tdb_socket_open() and
 * covers the connect, the greeting and every later statement, so a caller's
 * retry loop cannot outrun it.
 *
 * Rejected: a pollable fd plus a pump, and a worker thread with a self-pipe.
 * The caller's caller is a session loop that would gain a third fd and a state
 * machine, or a thread in a strictly single-threaded process that cannot be
 * cleanly reclaimed when parked in read() on a dead runner.
 *
 * ONE STATEMENT PER CALL, AND RETRY IS THE CALLER'S. A transaction is four
 * ordinary statements in order on one connection and needs nothing from this
 * interface, because a connection is owned by one operation in one process.
 * The backoff loop lives in the caller because the sequence that needs it
 * branches on a row count only the caller can read.
 *
 * Full reasoning: #44.
 */

#include "libtetrisdb/status.h"

#include <limits.h>
#include <stddef.h>

/** One connection: the socket, its line buffer and the deadline. Opaque, and
 * owned by ONE thread in ONE process for its whole life - the protocol has no
 * request ids, so two writers would interleave on the wire. */
typedef struct tdb_socket tdb_socket_t;

/** How to reach the runner. Fixed buffer so an rc overlay can rewrite the path
 * in place, matching tdb_opts_t. */
typedef struct {
    char sock[PATH_MAX]; /**< SocketRunner's unix socket; the db_ipc rc key. */
    int  timeout_ms;     /**< Whole-exchange deadline; 0 means the default. */
} tdb_socket_opts_t;

/**
 * Seeds *opts with libtetrisdb/socket/conf.h's defaults.
 *
 * Call before overlaying rc values, so a key the file omits still has a usable
 * one. The path is relative and stays that way: it resolves against the
 * caller's working directory (ADR 0003).
 *
 * @param opts  Receives the defaults.
 */
void tdb_socket_opts_default(tdb_socket_opts_t *opts);

/*
 * Connect to the runner and consume its "<<READY>>" greeting, or fail.
 *
 * Returns NULL on any failure (message on stderr), leaving no fd behind. The
 * caller is not told which failure it was, on purpose: a missing socket, a
 * refused connect and an expired deadline are one thing to the only caller
 * ("the account service is unreachable"), and distinguishing them would put a
 * second error vocabulary next to tdb_status_t for no decision it can make.
 *
 * THIS IS WHERE THE CLOCK STARTS. opts->timeout_ms bounds everything from here
 * to tdb_socket_close(), not each statement separately.
 *
 * PER-OPERATION LIFETIME. Open when a statement is ready, close when the
 * exchange resolves. Never hold one across a wait for client input and never
 * for the rest of a round: a connection is one of the runner's --sessions
 * slots, and holding it idle queues somebody else's login behind it.
 */
/**
 * Connects to the runner and consumes its "<<READY>>" greeting.
 *
 * THIS IS WHERE THE CLOCK STARTS: opts->timeout_ms bounds everything from here
 * to tdb_socket_close(), not each statement separately.
 *
 * PER-OPERATION LIFETIME. Open when a statement is ready, close when the
 * exchange resolves. Never hold one across a wait for client input and never
 * for the rest of a round: a connection is one of the runner's --sessions
 * slots, and holding it idle queues somebody else's login behind it.
 *
 * @param opts  Where the runner is, and the deadline.
 * @returns The connection, or NULL on any failure, leaving no fd behind
 *          (message on stderr). Which failure is deliberately not reported: a
 *          missing socket, a refused connect and an expired deadline are one
 *          thing to the only caller, and distinguishing them would put a
 *          second error vocabulary beside tdb_status_t for no decision it can
 *          make.
 */
tdb_socket_t *tdb_socket_open(const tdb_socket_opts_t *opts);

/**
 * Sends one statement and reads its whole response.
 *
 * A statement containing a newline is refused as TDB_ERROR without being sent:
 * the wire is one statement per line, so a newline would submit a second
 * statement whose response nobody reads, leaving every later reply off by one.
 * Quoting is tdb_quote()'s job; this is the backstop.
 *
 * @param c     The connection.
 * @param sql   One statement, ending in ';'.
 * @param body  Receives everything the statement printed above its marker
 *              line, NUL-terminated and truncated to cap; may be NULL if only
 *              the outcome matters. Lines are kept as lines, because a select
 *              reply's row structure is what tdb_row_fields() reads. On a
 *              failure it carries the runner's error text, whose format is
 *              explicitly not a stable contract - branch on the return value,
 *              log the body.
 * @param cap   Capacity of body.
 * @returns The statement's outcome.
 */
tdb_status_t tdb_socket_exec(tdb_socket_t *c, const char *sql, char *body,
                             size_t cap);

/**
 * Closes the connection and frees the handle.
 *
 * The documented way to end a session: the runner rolls back any transaction
 * left open and releases its page locks, so a caller that gives up
 * mid-transaction cannot wedge anyone else. There is deliberately no "rollback
 * and reuse" - the connection is the transaction's scope.
 *
 * @param c  The connection; NULL and already-failed are both safe.
 */
void tdb_socket_close(tdb_socket_t *c);

/* --- reading a select reply -------------------------------------------- *
 *
 * A select prints a tab-separated header, a line of dashes, one line per row,
 * a blank line, then " N rows."
 * (db/src/java/simpledb/execution/Query.java:102-123), wrapped in the parser's
 * own narration, which these read past. Strings print raw - unpadded,
 * unquoted - so a TAB inside a stored value shifts every field after it, which
 * is why the username charset excludes TAB and the digest is hex.
 *
 * This is not a result-set API: no allocation, no types, no cursor. The only
 * read in the system is one row of one table. It lives here because the layout
 * is a property of the wire.
 */

/**
 * How many rows a reply body carries.
 *
 * An insert's reply also carries a table, so a non-negative return means "this
 * body has a table in it", not "this was a select".
 *
 * @param body  A reply body from tdb_socket_exec().
 * @returns The row count, or -1 if body carries no complete table - an error
 *          message, or a reply cut short by an undersized buffer. Zero and -1
 *          are different answers and must stay so: the first is "no such
 *          user", the second is "this is not an answer", and conflating them
 *          refuses a login for an account that exists.
 */
int tdb_row_count(const char *body);

/**
 * The fields of one row, as slices into body.
 *
 * Nothing is copied and nothing is NUL-terminated, so body must outlive the
 * use of f.
 *
 * @param body  A reply body from tdb_socket_exec().
 * @param row   0-based row index.
 * @param f     Receives field starts.
 * @param len   Receives field lengths.
 * @param max   Entries available in f and len.
 * @returns The number of fields the row actually has, having filled at most
 *          max, or -1 if body is not a select reply or row is out of range. A
 *          return larger than max is a schema disagreement rather than a
 *          truncation to tolerate: FIELDS ARE POSITIONAL, so a caller that
 *          ignores the count reads a salt as a digest with nothing reporting
 *          it.
 */
int tdb_row_fields(const char *body, int row, const char *f[], size_t len[],
                   int max);

#endif /* LIBTETRISDB_SOCKET_DB_H */
