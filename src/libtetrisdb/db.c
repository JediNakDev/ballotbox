/*
 * db.c - the bounded queue, the one worker thread, and the public handle.
 *
 * This file exists to enforce one rule: exactly one thread ever touches the
 * pipe. Producers hand over a string and move on; the worker is alone with
 * the child for the whole request/response exchange. That is what makes
 * tdb_submit() callable from anywhere without the caller thinking about it.
 *
 * The queue is bounded and lossy on purpose. Blocking a producer would let a
 * slow JVM back-pressure into the daemon's receive loop, which is exactly the
 * failure logging is supposed to avoid: a record is worth dropping, a
 * stalled daemon is not.
 */

#include "internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Consecutive respawns before giving up on the child for good. A PipeRunner
 * that dies repeatedly is misconfigured, not unlucky, and refetching a JVM in
 * a tight loop would cost more than the records it saves. */
#define TDB_RESPAWN_MAX 3

struct tdb {
  tdb_proc_t proc;
  tdb_opts_t opts;

  char **items; /* ring of owned strings, queue_cap entries */
  size_t cap, head, tail, count;

  int stopping; /* set by tdb_stop: drain, then exit the worker */
  int dead;     /* child unusable and out of respawns */

  unsigned long dropped;
  unsigned long errors;

  pthread_mutex_t m;
  pthread_cond_t not_empty;
  pthread_t worker;
  int worker_live;
};

/* Take the next statement, or NULL when the worker should exit. Blocks while
 * the queue is empty and we are still running. Caller owns the string. */
static char *dequeue(tdb_t *db) {
  pthread_mutex_lock(&db->m);
  while (db->count == 0 && !db->stopping)
    pthread_cond_wait(&db->not_empty, &db->m);

  char *sql = NULL;
  if (db->count > 0) {
    sql = db->items[db->head];
    db->head = (db->head + 1) % db->cap;
    db->count--;
  }
  pthread_mutex_unlock(&db->m);
  return sql; /* NULL only when stopping with an empty queue */
}

/*
 * Run one statement, respawning the child if it died mid-flight.
 *
 * A restart is safe without replay: SimpleDB commits with FORCE, so every
 * statement that reported success is already on disk, and the one in flight
 * had not committed. It is retried once against the fresh child.
 */
static void execute(tdb_t *db, const char *sql) {
  char body[TDB_BODY_MAX];

  for (int attempt = 0; attempt <= TDB_RESPAWN_MAX; attempt++) {
    if (db->dead) {
      /* Queued before the child was written off: it never reached the
       * table, so it is a drop like any other. */
      pthread_mutex_lock(&db->m);
      db->dropped++;
      pthread_mutex_unlock(&db->m);
      return;
    }

    int r = tdb_proc_exec(&db->proc, sql, body, sizeof(body));
    if (r >= 0) {
      if (r == 0) {
        /* SQL the child rejected. Retrying cannot help - the
         * statement itself is wrong - so count it and move on. */
        pthread_mutex_lock(&db->m);
        db->errors++;
        pthread_mutex_unlock(&db->m);
        fprintf(stderr, "tetrisdb: statement failed: %s\n", body);
      }
      return;
    }

    /* The child is gone. Reap it and try to bring up a replacement. */
    tdb_proc_kill(&db->proc);
    if (attempt == TDB_RESPAWN_MAX ||
        tdb_proc_spawn(&db->proc, &db->opts) < 0) {
      fprintf(stderr, "tetrisdb: giving up on PipeRunner; "
                      "further statements will be dropped\n");
      pthread_mutex_lock(&db->m);
      db->dead = 1;
      pthread_mutex_unlock(&db->m);
      return;
    }
    fprintf(stderr, "tetrisdb: PipeRunner restarted\n");
  }
}

static void *worker_main(void *arg) {
  tdb_t *db = arg;
  char *sql;

  while ((sql = dequeue(db)) != NULL) {
    execute(db, sql);
    free(sql);
  }
  return NULL;
}

tdb_t *tdb_start(const tdb_opts_t *opts, const char *probe, char *body,
                 size_t body_cap) {
  tdb_t *db = calloc(1, sizeof(*db));
  if (db == NULL) {
    fprintf(stderr, "tetrisdb: out of memory\n");
    return NULL;
  }

  db->opts = *opts;
  db->cap = opts->queue_cap > 0 ? opts->queue_cap : 256;
  db->items = calloc(db->cap, sizeof(*db->items));
  if (db->items == NULL) {
    fprintf(stderr, "tetrisdb: out of memory\n");
    free(db);
    return NULL;
  }

  pthread_mutex_init(&db->m, NULL);
  pthread_cond_init(&db->not_empty, NULL);

  /* Spawn before the worker exists, so the JVM's startup cost is paid on
   * the caller's thread and a failure is reported synchronously. */
  if (tdb_proc_spawn(&db->proc, &db->opts) < 0)
    goto fail;

  /* The one synchronous statement, run while this thread is still the only
   * one that knows the pipe exists. A failure is reported but not fatal:
   * the probe reads, and a daemon that cannot read can still write. */
  if (probe != NULL && tdb_proc_exec(&db->proc, probe, body, body_cap) < 0) {
    fprintf(stderr, "tetrisdb: PipeRunner died during startup probe\n");
    tdb_proc_kill(&db->proc);
    goto fail;
  }

  if (pthread_create(&db->worker, NULL, worker_main, db) != 0) {
    fprintf(stderr, "tetrisdb: cannot start worker thread\n");
    tdb_proc_close(&db->proc);
    goto fail;
  }
  db->worker_live = 1;
  return db;

fail:
  pthread_cond_destroy(&db->not_empty);
  pthread_mutex_destroy(&db->m);
  free(db->items);
  free(db);
  return NULL;
}

int tdb_submit(tdb_t *db, const char *sql) {
  if (db == NULL)
    return -1;

  char *copy = strdup(sql);
  if (copy == NULL) {
    pthread_mutex_lock(&db->m);
    db->dropped++;
    pthread_mutex_unlock(&db->m);
    return -1;
  }

  pthread_mutex_lock(&db->m);
  if (db->count == db->cap || db->stopping || db->dead) {
    /* Full, shutting down, or no child left: drop. Never block - see the
     * file comment. */
    db->dropped++;
    pthread_mutex_unlock(&db->m);
    free(copy);
    return -1;
  }
  db->items[db->tail] = copy;
  db->tail = (db->tail + 1) % db->cap;
  db->count++;
  pthread_cond_signal(&db->not_empty);
  pthread_mutex_unlock(&db->m);
  return 0;
}

unsigned long tdb_dropped(tdb_t *db) {
  if (db == NULL)
    return 0;
  pthread_mutex_lock(&db->m);
  unsigned long n = db->dropped;
  pthread_mutex_unlock(&db->m);
  return n;
}

unsigned long tdb_errors(tdb_t *db) {
  if (db == NULL)
    return 0;
  pthread_mutex_lock(&db->m);
  unsigned long n = db->errors;
  pthread_mutex_unlock(&db->m);
  return n;
}

void tdb_stop(tdb_t *db, unsigned long *dropped, unsigned long *errors) {
  if (db == NULL) {
    if (dropped != NULL)
      *dropped = 0;
    if (errors != NULL)
      *errors = 0;
    return;
  }

  /* Ask the worker to finish what is queued and exit. Statements already
   * accepted were "logged" from the producer's point of view, so they are
   * drained, not discarded; new ones are refused from here on. */
  pthread_mutex_lock(&db->m);
  db->stopping = 1;
  pthread_cond_broadcast(&db->not_empty);
  pthread_mutex_unlock(&db->m);

  if (db->worker_live)
    pthread_join(db->worker, NULL);

  tdb_proc_close(&db->proc);

  /* Anything still queued when the worker gave up (it only does so on a
   * dead child) never reached the table. */
  while (db->count > 0) {
    free(db->items[db->head]);
    db->head = (db->head + 1) % db->cap;
    db->count--;
    db->dropped++;
  }

  if (dropped != NULL)
    *dropped = db->dropped;
  if (errors != NULL)
    *errors = db->errors;

  pthread_cond_destroy(&db->not_empty);
  pthread_mutex_destroy(&db->m);
  free(db->items);
  free(db);
}
