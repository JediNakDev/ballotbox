# Integrating a C daemon with SimpleDB via `PipeRunner`

This document specifies how a C daemon should talk to SimpleDB.
It assumes the daemon owns a disjoint set of tables it is allowed to write, and that a sibling daemon (possibly written by someone else) owns the rest.
It is written so an agent implementing the C side does not need any other context.

## 1. Architecture overview

```
  producer thread(s)          worker thread              child process
  (accept requests,     ->    (owns the pipe,     ->     java -cp simpledb.jar
   build SQL text)             the only thread             simpledb.PipeRunner
                                touching the DB)            <catalog file>
```

Each daemon process (`serverd`, `loggerd`, ...) spawns its own `PipeRunner` child process and owns exactly one long-lived pair of pipes to it.
There is no shared JVM, no shared `BufferPool`, and no network server between daemons.
Two daemons run as two independent OS processes, each with its own JVM, each reading the same `catalog.txt` but touching different `.dat` files on disk.

This works *only* because table ownership is disjoint (see Section 2).
If that ever stops being true, this whole design breaks and needs to change (see Section 6).

## 2. Hard invariants

These are correctness requirements, not style preferences. Violating them causes silent data corruption, not a crash.

1. **Table ownership must be strictly disjoint across daemons.**
   Every table in `catalog.txt` must be written by exactly one daemon process, and read only by that same daemon.
   SimpleDB has no cross-process locking or cache invalidation. Each `PipeRunner` process caches pages of the tables it touches in its own private, in-process `BufferPool`. If daemon A writes a row and daemon B has ever read that same table, B's cached copy has no way of knowing A's write happened — B will serve or overwrite stale data. This is true even though writes are flushed to disk on commit (SimpleDB uses a FORCE policy): FORCE guarantees the *writer's* durability, not visibility to a second process with its own cache.
   Concretely: `serverd` must never issue `SELECT`/`INSERT`/`DELETE` against the log table, and `loggerd` must never touch any table `serverd` owns. If a future feature needs both daemons to touch the same table, this architecture no longer applies — see Section 6.

2. **Exactly one worker thread per daemon may talk to its `PipeRunner` process.**
   The pipe is a strictly ordered, half-duplex request/response channel: one line in, one response block out, in lock-step. There is no request ID or multiplexing in this protocol. If two threads write to the pipe concurrently, their SQL statements interleave on the wire and responses become unattributable. `PipeRunner` also has no internal locking — SimpleDB's lab3 transaction/locking layer is not implemented in this codebase, so even if you avoided wire interleaving, concurrent statements executing inside the same JVM would race on `BufferPool` with no isolation guarantees at all.
   Practical consequence: architect the daemon as many producer threads (handling client connections, log events, whatever) pushing SQL onto a bounded queue, and exactly one consumer/worker thread draining that queue and driving the pipe. Never spawn a thread per SQL statement, and never let two threads hold the pipe file descriptors at once.

3. **Don't fan out multiple `PipeRunner` processes for one daemon's own tables.**
   If `serverd` owns tables `foo` and `bar`, run **one** `PipeRunner` process for both, not one per table. Splitting further doesn't help (they'd share nothing) and just multiplies JVM startup cost.

## 3. Wire protocol

`PipeRunner` (`src/java/simpledb/PipeRunner.java`) is a non-interactive driver: it never touches jline/`ConsoleReader`, so it behaves identically whether stdin is a TTY or a pipe. Launch it as:

```
java -cp dist/simpledb.jar simpledb.PipeRunner <catalogFile>
```

- **Startup**: once the catalog is loaded, `PipeRunner` prints one line, `<<READY>>`, then flushes. The daemon must wait for this line before sending the first statement.
- **Request**: exactly one SQL statement per line on stdin, ending in `;`, UTF-8, newline-terminated (`\n`). Blank lines are ignored (not echoed with a response).
- **Response**: whatever text `Parser` prints for that statement (query results, "Transaction N committed.", etc.), followed by exactly one marker line:
  - `<<END ok>>` — statement succeeded.
  - `<<END error>>` — statement failed (parse error, aborted transaction, unsupported statement). The error text appears in the body above the marker but its exact format is not a stable contract — check the marker, not the body, to decide success/failure programmatically.
- **stderr**: anything the parser prints to stderr during a statement is captured and folded into that statement's stdout response body, so the daemon does not need to drain the child's stderr pipe (it stays quiet except for pre-startup usage errors).
- **Shutdown**: closing stdin (EOF) causes `PipeRunner` to flush all dirty pages to disk and exit(0). This is the only supported clean shutdown path — do not `SIGKILL` the child if you can help it, since NO-STEAL/FORCE durability depends on commit-time flushing, but an in-flight uncommitted transaction has nothing to lose either way. Prefer closing the pipe over signaling.

There is no framing beyond the `<<END ...>>` line, so the daemon's read loop must buffer until it sees a line that starts with `<<END ` (do this on line boundaries, not by pattern-matching inside partial reads).

## 4. C skeleton (illustrative, not production code)

This sketch shows the shape of the wiring: fork/exec with two pipes, a worker thread owning the child, and a bounded-queue producer API. Error handling, backpressure policy, and retry/restart logic are intentionally left out — fill those in for your actual daemon.

```c
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int to_child[2];    /* parent writes[1] -> child stdin[0] */
    int from_child[2];  /* child stdout[1] -> parent reads[0] */
    pid_t pid;
} db_proc_t;

/* --- spawn ------------------------------------------------------------ */

int db_spawn(db_proc_t *p, const char *catalog_path) {
    pipe(p->to_child);
    pipe(p->from_child);

    p->pid = fork();
    if (p->pid == 0) {
        dup2(p->to_child[0], STDIN_FILENO);
        dup2(p->from_child[1], STDOUT_FILENO);
        close(p->to_child[0]); close(p->to_child[1]);
        close(p->from_child[0]); close(p->from_child[1]);
        execlp("java", "java", "-cp", "dist/simpledb.jar",
               "simpledb.PipeRunner", catalog_path, (char *)NULL);
        _exit(127); /* exec failed */
    }
    close(p->to_child[0]);
    close(p->from_child[1]);

    /* block here for the "<<READY>>" line before returning */
    char buf[64];
    read_line(p->from_child[0], buf, sizeof buf);
    return strncmp(buf, "<<READY>>", 9) == 0 ? 0 : -1;
}

/* --- single in-flight request/response, called only from the worker --- */

typedef struct { int ok; char *body; } db_result_t;

db_result_t db_exec_one(db_proc_t *p, const char *sql /* ends in ';' */) {
    dprintf(p->to_child[1], "%s\n", sql);

    /* accumulate lines into a growable buffer until one starts with
       "<<END " -- real code needs a real buffered line reader here */
    char *body = read_until_end_marker(p->from_child[0]);
    int ok = strstr(body, "<<END ok>>") != NULL;
    db_result_t r = { ok, body };
    return r;
}

/* --- bounded queue: many producers, one consumer ----------------------- */

#define QCAP 256
typedef struct {
    char *items[QCAP];
    int head, tail, count;
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
} db_queue_t;

void db_queue_push(db_queue_t *q, char *sql /* takes ownership */) {
    pthread_mutex_lock(&q->m);
    while (q->count == QCAP) pthread_cond_wait(&q->not_full, &q->m);
    q->items[q->tail] = sql;
    q->tail = (q->tail + 1) % QCAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
}

char *db_queue_pop(db_queue_t *q) {
    pthread_mutex_lock(&q->m);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->m);
    char *sql = q->items[q->head];
    q->head = (q->head + 1) % QCAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return sql;
}

/* --- the ONE worker thread that owns the pipe --------------------------- */

void *db_worker(void *arg) {
    struct { db_proc_t *proc; db_queue_t *q; } *ctx = arg;
    for (;;) {
        char *sql = db_queue_pop(ctx->q);         /* blocks until work exists */
        db_result_t r = db_exec_one(ctx->proc, sql);
        /* deliver r to whoever submitted sql, e.g. via a per-request
           condvar/future stashed alongside the queue entry */
        free(sql);
    }
    return NULL;
}

/* db_submit(sql) called from any producer thread just does db_queue_push */
```

Two independent instances of this whole file (two `db_proc_t`, two queues, two worker threads) run inside `serverd`; a separate, unrelated instance runs inside `loggerd`. They never share a queue, a worker thread, or a pipe.

## 5. Operational notes

- **Build**: `ant dist` from the project root produces `dist/simpledb.jar`, which already includes `PipeRunner` (no separate build step needed).
- **Adding a table**: convert source data with `java -jar dist/simpledb.jar convert file.txt <numFields>` to produce `file.dat`, then add a line to `catalog.txt` of the form `tablename (field1 type1, field2 type2, ...)` — the `.dat` file is resolved as `<tablename>.dat` in the same directory as `catalog.txt` (see `Catalog.loadSchema`, `src/java/simpledb/common/Catalog.java:160`). There is no `CREATE TABLE` statement support in the SQL parser.
- **Restart policy**: if a `PipeRunner` child dies or is killed, all data committed before the crash is safely on disk (FORCE-on-commit). It is safe for the daemon to respawn a fresh `PipeRunner` process and resume submitting statements; nothing needs replaying.
- **Known limitations**: one statement per line, no multi-statement transactions from the daemon's side (each line auto-commits as its own transaction — see `Parser.processNextStatement`), and no concurrent access support within a single `PipeRunner` process (Section 2, invariant 2) since transaction/locking (lab3) is not implemented in this codebase.

## 6. When this architecture stops applying

If a future requirement needs two daemons to read or write the *same* table (shared counters, foreign keys spanning both daemons' data, etc.), invariant 1 in Section 2 is violated and this design is no longer correct — do not attempt to patch around it with an external file lock or a shared mutex file. At that point, either merge the two daemons' DB access into one process (one `PipeRunner`, one worker thread, all callers funnel through it), or replace SimpleDB's per-process `BufferPool` with something that actually supports cross-process concurrency control. This document does not cover that case.
