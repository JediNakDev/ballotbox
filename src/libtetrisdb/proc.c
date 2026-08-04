/*
 * proc.c - the PipeRunner child process and the wire protocol to it.
 *
 * The protocol (db/docs/c-daemon-integration.md, section 3) is strictly
 * half-duplex: one statement in, one response block out, in lock-step, with
 * no request ids. That makes this file's whole job "write a line, read until
 * the end marker" - and makes it a hard error for two threads to be in here
 * at once. db.c is what guarantees they are not.
 */

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Handshake and response markers, exactly as PipeRunner writes them. */
#define TDB_READY "<<READY>>"
#define TDB_END "<<END "
#define TDB_OK "<<END ok>>"

/* Lines consumed while waiting for the handshake before giving up. Loading a
 * catalog prints a handful; anything past this means we are not talking to a
 * PipeRunner at all (a wrong jar, a JVM error) and would otherwise hang. */
#define TDB_HANDSHAKE_MAX 256

/*
 * Read one '\n'-terminated line into out (NUL-terminated, newline stripped).
 *
 * Returns 1 on a line, 0 on clean EOF, -1 on error. A line longer than cap is
 * split: the caller sees the first cap-1 bytes as a line and the rest as the
 * next one. That cannot corrupt marker detection, since markers are short and
 * anchored to the start of a line.
 */
static int read_line(tdb_proc_t *p, char *out, size_t cap) {
  size_t n = 0;

  for (;;) {
    if (p->pos == p->len) {
      ssize_t r = read(p->out_fd, p->buf, sizeof(p->buf));
      if (r < 0) {
        if (errno == EINTR)
          continue;
        return -1;
      }
      if (r == 0) {
        /* EOF: hand back a partial trailing line if we have one. */
        if (n > 0) {
          out[n] = '\0';
          return 1;
        }
        return 0;
      }
      p->len = (size_t)r;
      p->pos = 0;
    }

    while (p->pos < p->len) {
      char c = p->buf[p->pos++];
      if (c == '\n') {
        out[n] = '\0';
        return 1;
      }
      if (n + 1 < cap)
        out[n++] = c;
    }
  }
}

/* Write every byte of buf, retrying short writes. Returns 0, or -1 if the
 * pipe broke (the child died). */
static int write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, buf, len);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buf += (size_t)w;
    len -= (size_t)w;
  }
  return 0;
}

int tdb_proc_spawn(tdb_proc_t *p, const tdb_opts_t *opts) {
  int to_child[2], from_child[2];
  char catalog[PATH_MAX + 16];

  memset(p, 0, sizeof(*p));
  p->pid = -1;
  p->in_fd = p->out_fd = -1;

  if (opts->dir[0] == '\0') {
    fprintf(stderr, "tetrisdb: no data directory set (see tdb_opts_t.dir)\n");
    return -1;
  }

  snprintf(catalog, sizeof(catalog), "%s/catalog.txt", opts->dir);

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
  p->out_fd = from_child[0];

  /* Wait for the handshake. Everything the catalog loader prints on the way
   * there is startup noise, not a response, so it is skipped. */
  char line[512];
  for (int i = 0; i < TDB_HANDSHAKE_MAX; i++) {
    int r = read_line(p, line, sizeof(line));
    if (r <= 0)
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
  char line[TDB_BODY_MAX];
  size_t used = 0;

  if (body != NULL && body_cap > 0)
    body[0] = '\0';

  if (write_all(p->in_fd, sql, strlen(sql)) < 0 ||
      write_all(p->in_fd, "\n", 1) < 0)
    return -1;

  /* Accumulate until the marker line. There is no framing beyond it, so a
   * caller that stopped reading early would leave the next statement's
   * response misaligned - hence "always read to the marker". */
  for (;;) {
    int r = read_line(p, line, sizeof(line));
    if (r <= 0)
      return -1; /* child died mid-statement */

    if (strncmp(line, TDB_END, sizeof(TDB_END) - 1) == 0)
      return strcmp(line, TDB_OK) == 0 ? 1 : 0;

    /* Lines are kept as lines: a startup probe's caller has to pick a
     * value out of tabular output, and flattening would destroy the row
     * structure that makes it findable. */
    if (body != NULL && used + 1 < body_cap) {
      size_t room = body_cap - used;
      int n = snprintf(body + used, room, "%s%s", used > 0 ? "\n" : "", line);
      used += (n < 0 || (size_t)n >= room) ? room - 1 : (size_t)n;
    }
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
  if (p->out_fd >= 0) {
    close(p->out_fd);
    p->out_fd = -1;
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
  if (p->out_fd >= 0)
    close(p->out_fd);
  p->pid = -1;
  p->in_fd = p->out_fd = -1;
}
