/**
 * @file pipe/proc.c
 * @brief The PipeRunner child process, and starting and stopping it.
 *
 * The protocol (db/docs/c-daemon-integration.md, section 3) is strictly
 * half-duplex: one statement in, one response block out, in lock-step, with
 * no request ids. That makes it a hard error for two threads to be in here at
 * once, and pipe/queue.c is what guarantees they are not.
 *
 * The protocol itself is in wire.c, shared with socket/socket.c, because SocketRunner
 * speaks exactly the same one. What is left here is everything that is about a
 * CHILD PROCESS rather than about the protocol: fork, exec, the startup
 * handshake, and the two ways to end it.
 *
 * Nothing here passes a deadline. That is this transport's contract, not an
 * omission: the child is ours, its lifetime is the daemon's, and a wedged one
 * is handled by pipe/queue.c respawning it rather than by a clock.
 */

#include "proc.h"

#include "../jvm.h"
#include "libtetrisdb/schema.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TDB_DEFAULT_JAR "db/dist/simpledb.jar"
#define TDB_DEFAULT_JAVA "java"
#define TDB_DEFAULT_QCAP 256

void tdb_opts_default(tdb_opts_t *opts) {
  opts->dir[0] = '\0';
  snprintf(opts->jar, sizeof(opts->jar), "%s", TDB_DEFAULT_JAR);
  snprintf(opts->java, sizeof(opts->java), "%s", TDB_DEFAULT_JAVA);
  opts->queue_cap = TDB_DEFAULT_QCAP;
}

/* The startup handshake, exactly as PipeRunner writes it. */
#define TDB_READY "<<READY>>"

/* Lines consumed while waiting for the handshake before giving up. Loading a
 * catalog prints a handful; anything past this means we are not talking to a
 * PipeRunner at all (a wrong jar, a JVM error) and would otherwise hang. */
#define TDB_HANDSHAKE_MAX 256

int tdb_proc_spawn(tdb_proc_t *p, const tdb_opts_t *opts) {
  int to_child[2], from_child[2];
  char catalog[PATH_MAX + 16];

  memset(p, 0, sizeof(*p));
  p->pid = -1;
  p->in_fd = p->out.fd = -1;

  if (opts->dir[0] == '\0') {
    fprintf(stderr, "tetrisdb: no data directory set (see tdb_opts_t.dir)\n");
    return -1;
  }

  /* Ask before forking. Without this the same two failures - no jar, no java -
   * arrive as "never reported ready" after a fork and a handshake wait, which
   * names neither. Shared with the SocketRunner's launcher (ADR 0001), so the
   * message is identical wherever a runner fails to start. */
  if (tdb_jvm_check(opts->java, opts->jar) < 0)
    return -1;

  tdb_catalog_path(catalog, sizeof(catalog), opts->dir);

  if (pipe(to_child) < 0) {
    fprintf(stderr, "tetrisdb: pipe: %s\n", strerror(errno));
    return -1;
  }
  if (pipe(from_child) < 0) {
    fprintf(stderr, "tetrisdb: pipe: %s\n", strerror(errno));
    close(to_child[0]);
    close(to_child[1]);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "tetrisdb: fork: %s\n", strerror(errno));
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return -1;
  }

  if (pid == 0) {
    /* Child. Only async-signal-safe calls until execvp: this fork can
     * happen while the daemon's other threads are running. */
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);

    /* PipeRunner folds its own stderr into each response body, so the
     * only thing left on fd 2 is a pre-startup usage error. Let it reach
     * the daemon's stderr rather than routing it nowhere. */
    char cp[PATH_MAX + 16];
    snprintf(cp, sizeof(cp), "%s", opts->jar);
    execlp(opts->java, opts->java, "-cp", cp, "simpledb.PipeRunner", catalog,
           (char *)NULL);
    _exit(127); /* exec failed - the parent sees this as a failed ready */
  }

  close(to_child[0]);
  close(from_child[1]);
  (void)fcntl(to_child[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(from_child[0], F_SETFD, FD_CLOEXEC);

  p->pid = pid;
  p->in_fd = to_child[1];
  p->out.fd = from_child[0];

  /* Wait for the handshake. Everything the catalog loader prints on the way
   * there is startup noise, not a response, so it is skipped. */
  char line[512];
  for (int i = 0; i < TDB_HANDSHAKE_MAX; i++) {
    int r = tdb_wire_line(&p->out, line, sizeof(line), TDB_NO_DEADLINE);
    if (r != TDB_WIRE_LINE)
      break; /* child exited (bad jar, exec failure) or the pipe broke */
    if (strncmp(line, TDB_READY, sizeof(TDB_READY) - 1) == 0)
      return 0;
  }

  fprintf(stderr, "tetrisdb: %s never reported ready (jar=%s catalog=%s)\n",
          opts->java, opts->jar, catalog);
  tdb_proc_kill(p);
  return -1;
}

int tdb_proc_exec(tdb_proc_t *p, const char *sql, char *body, size_t body_cap) {
  if (body != NULL && body_cap > 0)
    body[0] = '\0';

  if (tdb_wire_write(p->in_fd, sql, strlen(sql), TDB_NO_DEADLINE) != 0 ||
      tdb_wire_write(p->in_fd, "\n", 1, TDB_NO_DEADLINE) != 0)
    return -1;

  /* Three outcomes here, five on the wire. <<END retry>> collapses into "the
   * statement failed" on purpose: this transport's caller is the lossy
   * logging queue, which has no transaction to re-run and nothing a retry
   * would repair. The read path that does care keeps them apart - see
   * tdb_status_t in libtetrisdb/status.h. */
  switch (tdb_wire_response(&p->out, body, body_cap, TDB_NO_DEADLINE)) {
  case TDB_OK:
    return 1;
  case TDB_IO:
  case TDB_TIMEOUT: /* unreachable without a deadline; the child died */
    return -1;
  default:
    return 0;
  }
}

/* Reap pid, tolerating an interrupted wait. */
static void reap(pid_t pid) {
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
}

void tdb_proc_close(tdb_proc_t *p) {
  if (p->pid < 0)
    return;

  /* Closing stdin is the documented clean shutdown: PipeRunner flushes
   * every dirty page and exits 0. Signalling it instead would skip that. */
  if (p->in_fd >= 0) {
    close(p->in_fd);
    p->in_fd = -1;
  }
  reap(p->pid);
  if (p->out.fd >= 0) {
    close(p->out.fd);
    p->out.fd = -1;
  }
  p->pid = -1;
}

void tdb_proc_kill(tdb_proc_t *p) {
  if (p->pid < 0)
    return;

  kill(p->pid, SIGKILL);
  reap(p->pid);
  if (p->in_fd >= 0)
    close(p->in_fd);
  if (p->out.fd >= 0)
    close(p->out.fd);
  p->pid = -1;
  p->in_fd = p->out.fd = -1;
}
