#ifndef LIBTETRISDB_WIRE_H
#define LIBTETRISDB_WIRE_H

/**
 * @file wire.h
 * @brief The line protocol, over a plain fd, shared by both runners.
 *
 * Included by pipe/proc.c and socket/socket.c. The protocol is identical for
 * PipeRunner and SocketRunner (db/docs/c-daemon-integration.md section 4), so
 * the line buffer, the <<END ...>> scan and the marker classification are
 * written once here. Everything above it - a child and a lossy queue on one
 * side, a connection and a deadline on the other - differs and deliberately
 * does not meet.
 *
 * THIS HEADER IS WHAT THE TWO PATHS SHARE, AND ALL OF IT. It pulls in
 * libtetrisdb/status.h and nothing else, so a file including only this one can
 * reach neither transport. That separation is checkable by the compiler
 * instead of being a rule someone has to keep.
 *
 * tdb_status_t is the shared public enum rather than a private twin because
 * the markers ARE the protocol, and a mapping table can drift.
 */

#include "libtetrisdb/status.h"

#include <stddef.h>

/** Longest response body kept from a statement. PipeRunner's failures are a
 * line or two and the query path's one row is shorter, so a short cap costs
 * nothing and bounds a runaway runner's output. */
#define TDB_BODY_MAX 1024

/** Wait indefinitely. Deadlines are absolute milliseconds on a monotonic
 * clock, so only differences are meaningful and a wall-clock change cannot
 * shorten or extend one. */
#define TDB_NO_DEADLINE (-1)

/** Now, on that monotonic clock. Called wherever a deadline is built. */
long long tdb_now_ms(void);

/** Line-buffered reader over one fd. The protocol is line-oriented and a
 * response terminator is only recognisable on a line boundary, so reads are
 * buffered here rather than by each caller. */
typedef struct {
    int    fd;
    char   buf[4096];
    size_t len; /**< Bytes held in buf. */
    size_t pos; /**< Next unread byte in buf. */
} tdb_wire_t;

/** What a read attempt did. Deliberately distinct from tdb_status_t: these are
 * facts about the fd, that one is a fact about a statement. */
enum {
    TDB_WIRE_LINE = 1, /**< A line was read. */
    TDB_WIRE_EOF = 0,  /**< The peer closed cleanly. */
    TDB_WIRE_IO = -1,  /**< The fd broke. */
    TDB_WIRE_LATE = -2 /**< The deadline passed first. */
};

/**
 * Reads one '\n'-terminated line.
 *
 * @param w         The reader.
 * @param out       Receives the line, NUL-terminated, newline stripped. A line
 *                  longer than cap is split: the caller sees the first cap-1
 *                  bytes as a line and the rest as the next one, which cannot
 *                  corrupt marker detection because markers are short and
 *                  anchored to the start of a line.
 * @param cap       Capacity of out.
 * @param deadline  Absolute monotonic ms, or TDB_NO_DEADLINE.
 * @returns One of the TDB_WIRE_* values.
 */
int tdb_wire_line(tdb_wire_t *w, char *out, size_t cap, long long deadline);

/** Writes every byte, retrying short writes and waiting for room until
 * deadline. Returns 0, TDB_WIRE_IO if the fd broke, or TDB_WIRE_LATE. */
int tdb_wire_write(int fd, const char *data, size_t len, long long deadline);

/**
 * Reads one statement's whole response, up to and including its "<<END ...>>"
 * marker.
 *
 * There is no framing beyond the marker, so a caller that stops reading early
 * leaves the next statement's response misaligned - which is why this always
 * reads to the marker, and why an abandoned exchange must end the connection
 * rather than continue on it.
 *
 * @param w         The reader.
 * @param body      Receives the lines above the marker, NUL-terminated and
 *                  truncated to cap; may be NULL.
 * @param cap       Capacity of body.
 * @param deadline  Absolute monotonic ms, or TDB_NO_DEADLINE.
 * @returns The marker's meaning, or TDB_IO / TDB_TIMEOUT if the response never
 *          arrived.
 */
tdb_status_t tdb_wire_response(tdb_wire_t *w, char *body, size_t cap,
                               long long deadline);

#endif /* LIBTETRISDB_WIRE_H */
