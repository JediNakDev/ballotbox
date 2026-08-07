/**
 * @file socket/socket.c
 * @brief One connection to the shared SocketRunner, under a * deadline.
 *
 * The contract and the reasoning are in include/libtetrisdb/socket/db.h. What
 * is here is the mechanics: connect, greeting, one statement at a time, close.
 *
 * The socket is non-blocking for its whole life and every wait goes through
 * wire.c's poll(). That is not an async design - every call here still blocks
 * the caller until it has an answer - it is what lets ONE deadline cover the
 * connect, the greeting and every statement, including the case the deadline
 * exists for: a connection queued behind a full --sessions pool, where the
 * runner is alive, the socket is connected, and nothing ever arrives.
 */

/* wire.h, not pipe/db.h: this path has no child process, no queue and no
 * tdb_t, and taking only the shared header is what keeps it that way. */
#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/db.h"

#include "../wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The default socket path and deadline are libtetrisdb/socket/conf.h's,
 * shared with the launcher and with libtetrisauth's login path. */

/* The per-connection greeting, exactly as SocketRunner writes it. Unlike
 * PipeRunner's, this one is not preceded by catalog-loading noise: that goes
 * to the runner's own stderr, and the connection carries nothing but the
 * greeting and responses. So this is read as exactly one line, not scanned
 * for - anything else on the wire means we are not talking to a runner. */
#define TDB_READY "<<READY>>"

struct tdb_socket {
  tdb_wire_t wire;    /* the socket, and its line buffer */
  long long deadline; /* absolute monotonic ms; set at open */
  tdb_status_t
      failed; /* TDB_OK, or the terminal status this conn is stuck on */
};

void tdb_socket_opts_default(tdb_socket_opts_t *opts) {
  snprintf(opts->sock, sizeof(opts->sock), "%s", TDB_DEFAULT_IPC);
  opts->timeout_ms = TDB_DEFAULT_TIMEOUT_MS;
}

/** Connect to path, non-blocking, bounded by deadline. Returns the fd or -1
 * (message on stderr).
 * A unix-socket connect() usually completes immediately, but it can return
 * EINPROGRESS when the listener's backlog is full - which is exactly the
 * overloaded runner this deadline is for, so it is waited on rather than
 * treated as an error. */
static int connect_deadline(const char *path, long long deadline) {
  struct sockaddr_un addr;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    fprintf(stderr, "tetrisdb: socket path too long (%zu >= %zu): %s\n",
            strlen(path), sizeof(addr.sun_path), path);
    return -1;
  }
  memcpy(addr.sun_path, path, strlen(path));

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "tetrisdb: socket: %s\n", strerror(errno));
    return -1;
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  (void)fcntl(fd, F_SETFL, O_NONBLOCK);

#ifdef SO_NOSIGPIPE
  /* A runner that died between the connect and the write would otherwise
   * kill this process with SIGPIPE. The session process does ignore SIGPIPE,
   * but a library that hands its caller a dead process instead of an error
   * return is relying on the caller's signal disposition, which is not part
   * of any contract here. */
  int on = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    return fd;

  if (errno != EINPROGRESS) {
    fprintf(stderr, "tetrisdb: connect %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }

  struct pollfd p;
  int err = 0;
  socklen_t errlen = sizeof(err);

  for (;;) {
    long long left = deadline - tdb_now_ms();
    if (left <= 0) {
      fprintf(stderr, "tetrisdb: connect %s: deadline passed\n", path);
      close(fd);
      return -1;
    }
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    int r = poll(&p, 1, (int)left);
    if (r > 0)
      break;
    if (r == 0) {
      fprintf(stderr, "tetrisdb: connect %s: deadline passed\n", path);
      close(fd);
      return -1;
    }
    if (errno == EINTR)
      continue;
    fprintf(stderr, "tetrisdb: poll: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  /* A ready-for-write socket is not a connected one: the result of the
   * connect is in SO_ERROR, and reading it is the only way to see a refusal
   * that arrived asynchronously. */
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
    fprintf(stderr, "tetrisdb: connect %s: %s\n", path,
            strerror(err != 0 ? err : errno));
    close(fd);
    return -1;
  }
  return fd;
}

tdb_socket_t *tdb_socket_open(const tdb_socket_opts_t *opts) {
  if (opts == NULL || opts->sock[0] == '\0') {
    fprintf(stderr,
            "tetrisdb: no socket path set (see tdb_socket_opts_t.sock)\n");
    return NULL;
  }

  tdb_socket_t *c = calloc(1, sizeof(*c));
  if (c == NULL) {
    fprintf(stderr, "tetrisdb: out of memory\n");
    return NULL;
  }

  /* The clock starts before the connect, because the connect is one of the
   * things that can hang. */
  int ms = opts->timeout_ms > 0 ? opts->timeout_ms : TDB_DEFAULT_TIMEOUT_MS;
  c->deadline = tdb_now_ms() + ms;
  c->failed = TDB_OK;

  c->wire.fd = connect_deadline(opts->sock, c->deadline);
  if (c->wire.fd < 0) {
    free(c);
    return NULL;
  }

  /* The greeting. A queued connection sits here, silently, until a session
   * slot frees up - which is the invisible hang the deadline exists for. */
  char line[64];
  int r = tdb_wire_line(&c->wire, line, sizeof(line), c->deadline);
  if (r != TDB_WIRE_LINE || strcmp(line, TDB_READY) != 0) {
    fprintf(stderr, "tetrisdb: %s never sent %s (%s)\n", opts->sock, TDB_READY,
            r == TDB_WIRE_LATE  ? "deadline passed"
            : r == TDB_WIRE_EOF ? "connection closed"
            : r == TDB_WIRE_IO  ? strerror(errno)
                                : "unexpected greeting");
    close(c->wire.fd);
    free(c);
    return NULL;
  }
  return c;
}

tdb_status_t tdb_socket_exec(tdb_socket_t *c, const char *sql, char *body,
                             size_t cap) {
  if (body != NULL && cap > 0)
    body[0] = '\0';

  if (c == NULL)
    return TDB_IO;

  /* Terminal statuses are sticky: after an abandoned exchange the socket
   * still holds an unread response, so the next statement's reply cannot be
   * told from this one's leftovers. This is also what stops a caller's retry
   * loop from outrunning the deadline. */
  if (c->failed != TDB_OK)
    return c->failed;

  if (strpbrk(sql, "\n\r") != NULL) {
    fprintf(stderr, "tetrisdb: refusing a statement containing a newline\n");
    return TDB_ERROR;
  }

  int w = tdb_wire_write(c->wire.fd, sql, strlen(sql), c->deadline);
  if (w == 0)
    w = tdb_wire_write(c->wire.fd, "\n", 1, c->deadline);
  if (w != 0)
    return c->failed = (w == TDB_WIRE_LATE) ? TDB_TIMEOUT : TDB_IO;

  tdb_status_t st = tdb_wire_response(&c->wire, body, cap, c->deadline);
  if (st == TDB_TIMEOUT || st == TDB_IO)
    c->failed = st;
  return st;
}

void tdb_socket_close(tdb_socket_t *c) {
  if (c == NULL)
    return;

  /* Just close. The runner rolls back whatever transaction is open and
   * releases its page locks when the connection ends, so sending a rollback
   * first would only add a statement that can itself time out. */
  if (c->wire.fd >= 0)
    close(c->wire.fd);
  free(c);
}
