#ifndef LIBTETRISDB_PIPE_PROC_H
#define LIBTETRISDB_PIPE_PROC_H

/**
 * @file proc.h
 * @brief The PipeRunner child, private to the fire-and-forget path.
 *
 * Included by pipe/proc.c and pipe/queue.c and nothing else. Split out so the
 * child-process handling can be reasoned about without the queue and worker
 * thread around it.
 *
 * Nothing on the query path includes this file: socket/socket.c takes wire.h
 * alone, so "the login path does not depend on the logging path" is a fact the
 * compiler holds rather than a rule someone has to keep.
 */

#include "libtetrisdb/pipe/db.h"
#include "../wire.h"

#include <stdio.h>
#include <sys/types.h>

/** One PipeRunner child and the two pipes to it. */
typedef struct {
    pid_t      pid;   /**< Child, or -1 when not running. */
    int        in_fd; /**< Statements are written here, to the child's stdin. */
    tdb_wire_t out;   /**< Responses are read here, from the child's stdout. */
} tdb_proc_t;

/*
 * Fork and exec "java -cp <jar> simpledb.PipeRunner <dir>/catalog.txt", then
 * read the child's startup output until its "<<READY>>" line.
 *
 * Loading the catalog prints a line per table before the handshake, so the
 * ready line is found by scanning, not by reading exactly one line.
 *
 * Returns 0 with *p populated, or -1 (message on stderr) with no child left
 * running.
 */
int tdb_proc_spawn(tdb_proc_t *p, const tdb_opts_t *opts);

/*
 * Send one statement and consume its whole response, up to and including the
 * "<<END ...>>" marker line.
 *
 * Returns 1 if the statement succeeded, 0 if the child reported an error (its
 * body is copied into body, capped at TDB_BODY_MAX), or -1 if the child died
 * or the pipe broke - in which case the caller must stop using *p until it
 * has been respawned.
 */
int tdb_proc_exec(tdb_proc_t *p, const char *sql, char *body, size_t body_cap);

/* Close the child's stdin so it flushes and exits, then reap it. Waits for
 * the child; safe to call on an already-closed proc. */
void tdb_proc_close(tdb_proc_t *p);

/* Kill and reap the child without the clean flush. Only for a child that is
 * already unusable (failed handshake), never for normal shutdown. */
void tdb_proc_kill(tdb_proc_t *p);

#endif /* LIBTETRISDB_PIPE_PROC_H */
