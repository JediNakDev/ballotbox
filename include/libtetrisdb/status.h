#ifndef LIBTETRISDB_STATUS_H
#define LIBTETRISDB_STATUS_H

/**
 * @file status.h
 * @brief What one statement did, for both runners.
 *
 * Included by libtetrisdb/socket/db.h and the private wire.h, which is how
 * the pipe transport reaches it - pipe/db.h does not name it, because a
 * best-effort write has no outcome to report. The markers ARE the protocol
 * (db/docs/c-daemon-integration.md section 4), so the enum belongs to the
 * wire rather than to either transport, and neither transport's header may
 * include the other's.
 */

/**
 * The outcome of one statement.
 *
 * TDB_TIMEOUT and TDB_IO are terminal for the connection: the exchange was
 * abandoned mid-response, so the next statement's reply cannot be told from
 * this one's leftovers. Once either is returned, every later tdb_socket_exec()
 * returns the same value without touching the socket, and the only thing left
 * to do is tdb_socket_close().
 */
typedef enum {
    TDB_OK = 0,  /**< <<END ok>>: the statement succeeded. */
    TDB_RETRY,   /**< <<END retry>>: deadlock victim, resubmit it. */
    TDB_ERROR,   /**< <<END error>>: the statement is wrong; do not retry. */
    TDB_TIMEOUT, /**< The deadline passed with the exchange unfinished. */
    TDB_IO       /**< The runner died, or the socket broke. */
} tdb_status_t;

#endif /* LIBTETRISDB_STATUS_H */
