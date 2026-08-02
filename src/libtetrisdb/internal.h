#ifndef LIBTETRISDB_INTERNAL_H
#define LIBTETRISDB_INTERNAL_H

/*
 * internal.h - the pieces of libtetrisdb that are not part of its interface.
 *
 * Split out so the child-process protocol (proc.c) can be reasoned about and
 * tested without the queue and worker thread (db.c) around it. Nothing here
 * is visible to callers.
 */

#include "libtetrisdb/tetrisdb.h"

#include <stdio.h>
#include <sys/types.h>

/* Longest response body kept from a statement. Only ever shown when a
 * statement fails, and PipeRunner's failures are a line or two, so a short
 * cap costs nothing and bounds a runaway child's output. */
#define TDB_BODY_MAX 1024

/* One PipeRunner child and the two pipes to it. */
typedef struct {
    pid_t pid;      /* child, or -1 when not running */
    int   in_fd;    /* we write statements here -> child stdin */
    int   out_fd;   /* we read responses here <- child stdout */

    /* Line-buffered reader over out_fd. The protocol is line-oriented and
     * the response terminator can only be recognised on a line boundary, so
     * reads must be buffered here rather than by each caller. */
    char   buf[4096];
    size_t len;     /* bytes held in buf */
    size_t pos;     /* next unread byte in buf */
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

#endif /* LIBTETRISDB_INTERNAL_H */
