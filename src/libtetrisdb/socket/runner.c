/**
 * @file socket/runner.c
 * @brief Putting a SocketRunner where socket.c expects to find one.
 *
 * The contract, and the mechanics/policy line it draws, are in
 * include/libtetrisdb/socket/runner.h.
 *
 * It lives beside socket.c rather than at the top level because it is entirely
 * about the socket transport: the pipe path's runner is a child this library
 * owns for a daemon's lifetime (pipe/proc.c), while this one is a process that
 * outlives whoever started it and is reached by address afterwards. What the
 * two genuinely share - is there a jar, does java run - is in jvm.c, one level
 * up, which is the sharing ADR 0001 asked for.
 */

#include "../jvm.h"
#include "libcommon/limits.h"
#include "libcommon/rc.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/runner.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "libtetrisdb/socket/conf.h"

#include <stddef.h>
#include <sys/file.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The db_ namespace's values and bounds are libtetrisdb/socket/conf.h's,
 * shared with socket/socket.c and with libtetrisauth's login path. Only the
 * readiness wait is this file's alone. */
#define TDB_RUNNER_DEFAULT_WAIT_MS 10000

const char *const tdb_rc_keys[] = {
    "db_dir", "db_ipc", "db_sessions", "db_jar", "db_java", "db_timeout",
    NULL,
};

/*
 * The complete db_ namespace, as a table.
 *
 * db_timeout is check_only here: bin/tetrisdb owns the namespace and refuses a
 * bad value while an operator is watching, but the key is consumed by
 * bin/session's login path (libtetrisauth's authconf.c), not by the launcher.
 * It used to be validated into a local called `ignored`, which read like dead
 * code and was the opposite.
 */
static const rc_key_t DB_KEYS[] = {
    {.key = "db_dir",
     .type = RC_STR,
     .off = offsetof(tdb_runner_opts_t, dir),
     .cap = sizeof(((tdb_runner_opts_t *)0)->dir)},
    {.key = "db_ipc",
     .type = RC_STR,
     .off = offsetof(tdb_runner_opts_t, ipc),
     .cap = sizeof(((tdb_runner_opts_t *)0)->ipc),
     .max_len = TDB_IPC_MAX},
    {.key = "db_sessions",
     .type = RC_INT,
     .off = offsetof(tdb_runner_opts_t, sessions),
     .lo = 1,
     .hi = MAX_SESSIONS},
    {.key = "db_jar",
     .type = RC_STR,
     .off = offsetof(tdb_runner_opts_t, jar),
     .cap = sizeof(((tdb_runner_opts_t *)0)->jar)},
    {.key = "db_java",
     .type = RC_STR,
     .off = offsetof(tdb_runner_opts_t, java),
     .cap = sizeof(((tdb_runner_opts_t *)0)->java)},
    {.key = "db_timeout",
     .type = RC_INT,
     .lo = TDB_TIMEOUT_MIN_MS,
     .hi = TDB_TIMEOUT_MAX_MS,
     .check_only = true},
};

void tdb_runner_opts_default(tdb_runner_opts_t *opts) {
  snprintf(opts->dir, sizeof(opts->dir), "%s", TDB_DEFAULT_DIR);
  snprintf(opts->jar, sizeof(opts->jar), "%s", TDB_DEFAULT_JAR);
  snprintf(opts->java, sizeof(opts->java), "%s", TDB_DEFAULT_JAVA);
  snprintf(opts->ipc, sizeof(opts->ipc), "%s", TDB_DEFAULT_IPC);
  opts->sessions = TDB_DEFAULT_SESSIONS;
  opts->recover = 1;
}

int tdb_runner_opts_load(const char *rc_path, tdb_runner_opts_t *opts) {
  if (rc_path == NULL || opts == NULL)
    return -1;

  tdb_runner_opts_t scratch = *opts;
  rc_defect_t defect;

  int applied = rc_bind(rc_path, DB_KEYS, sizeof DB_KEYS / sizeof DB_KEYS[0],
                        &scratch, "db_", &defect);
  if (applied == RC_E_OPEN) {
    fprintf(stderr, "tetrisdb: cannot read %s: %s\n", rc_path,
            strerror(errno));
    return -1;
  }
  if (applied < 0) {
    fprintf(stderr, "tetrisdb: %s: invalid directive (%s = %s)\n", rc_path,
            defect.key, defect.value);
    return -1;
  }

  *opts = scratch;
  return applied;
}

/** The lockfile inside a data directory. Called by tdb_runner_lock(). */
static int lock_path(const char *db_dir, char *path, size_t cap) {
  int n = snprintf(path, cap, "%s/.runner.lock", db_dir);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int tdb_runner_lock(const char *db_dir, char *path, size_t cap) {
  if (db_dir == NULL || db_dir[0] == '\0' || path == NULL) {
    fprintf(stderr, "tetrisdb: no data directory set\n");
    return -1;
  }
  if (tdb_mkdir_p(db_dir) != 0)
    return -1;
  if (lock_path(db_dir, path, cap) != 0) {
    fprintf(stderr, "tetrisdb: lock path too long for %s\n", db_dir);
    return -1;
  }

  int opened = open(path, O_RDWR | O_CREAT, 0640);
  if (opened < 0) {
    fprintf(stderr, "tetrisdb: open %s: %s\n", path, strerror(errno));
    return -1;
  }

  /* tdb_runner_spawn() replaces fd 0, 1 and 2 in the child, so a low
   * descriptor here would become one of them. F_DUPFD does not set
   * FD_CLOEXEC, which is equally load-bearing: the JVM must inherit this. */
  int fd = opened;
  if (fd <= STDERR_FILENO) {
    fd = fcntl(opened, F_DUPFD, STDERR_FILENO + 1);
    close(opened);
    if (fd < 0) {
      fprintf(stderr, "tetrisdb: duplicate %s: %s\n", path, strerror(errno));
      return -1;
    }
  }

  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    fprintf(stderr, "tetrisdb: %s is locked - a runner already owns %s\n",
            path, db_dir);
    close(fd);
    return -1;
  }
  return fd;
}

pid_t tdb_runner_spawn(const tdb_runner_opts_t *opts, int err_fd) {
  char catalog[PATH_MAX + 16];
  char sessions[32];

  if (opts == NULL || opts->dir[0] == '\0') {
    fprintf(stderr, "tetrisdb: no data directory set (see "
                    "tdb_runner_opts_t.dir)\n");
    return -1;
  }
  if (opts->ipc[0] == '\0') {
    fprintf(stderr, "tetrisdb: no socket path set (see "
                    "tdb_runner_opts_t.ipc)\n");
    return -1;
  }
  if (strlen(opts->ipc) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    fprintf(stderr, "tetrisdb: socket path too long: %s\n", opts->ipc);
    return -1;
  }
  if (tdb_jvm_check(opts->java, opts->jar) < 0)
    return -1;

  tdb_catalog_path(catalog, sizeof(catalog), opts->dir);
  if (access(catalog, R_OK) != 0) {
    fprintf(stderr, "tetrisdb: %s: %s - create the tables before starting the "
                    "runner, it reads the catalog once\n",
            catalog, strerror(errno));
    return -1;
  }

  snprintf(sessions, sizeof(sessions), "--sessions=%d",
           opts->sessions > 0 ? opts->sessions : TDB_DEFAULT_SESSIONS);

  /* Built here rather than in the child: after fork() only async-signal-safe
   * calls are allowed, and snprintf is not one of them. */
  char *argv[9];
  int n = 0;
  argv[n++] = (char *)opts->java;
  argv[n++] = (char *)"-cp";
  argv[n++] = (char *)opts->jar;
  argv[n++] = (char *)"simpledb.SocketRunner";
  argv[n++] = catalog;
  argv[n++] = (char *)opts->ipc;
  argv[n++] = sessions;
  if (!opts->recover)
    argv[n++] = (char *)"--no-recover";
  argv[n] = NULL;

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "tetrisdb: fork: %s\n", strerror(errno));
    return -1;
  }

  if (pid == 0) {
    /* Its own session, so the runner survives the terminal that started it
     * and never receives the launcher's job-control signals. */
    setsid();

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      if (err_fd < 0)
        dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO)
        close(null_fd);
    }
    if (err_fd >= 0)
      dup2(err_fd, STDERR_FILENO);

    /* Nothing else is closed: a lock the caller holds across this call must
     * reach the JVM, which is what makes the runner's lifetime and the lock's
     * lifetime the same thing. */
    execvp(opts->java, argv);
    _exit(127);
  }
  return pid;
}

/* Milliseconds on a monotonic clock. Local rather than wire.c's tdb_now_ms(),
 * so this file needs nothing from the protocol layer. */
static long long now_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    return 0;
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One connect attempt. Returns 1 if something is listening. */
static int reachable(const char *ipc) {
  struct sockaddr_un addr;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
}

int tdb_runner_wait(const char *ipc, pid_t pid, int timeout_ms) {
  long long deadline =
      now_ms() + (timeout_ms > 0 ? timeout_ms : TDB_RUNNER_DEFAULT_WAIT_MS);

  if (ipc == NULL || ipc[0] == '\0') {
    fprintf(stderr, "tetrisdb: no socket path to wait on\n");
    return -1;
  }

  for (;;) {
    if (reachable(ipc))
      return 0;

    /* Ask about the child before the clock: a JVM that refuses to start is
     * the common failure, and waiting out ten seconds to say so buries the
     * status that explains it. */
    if (pid > 0) {
      int status = 0;
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        if (WIFEXITED(status))
          /* Name causes as possibilities, not as the diagnosis. Several
           * unrelated failures land here with the same exit status, and a
           * confident wrong answer sends the reader further from the real one
           * than no answer would - the runner's own stderr has it. */
          fprintf(stderr, "tetrisdb: the runner exited (status %d) instead of "
                          "listening on %s - its stderr says why. A jar built "
                          "by a newer JDK than the java on PATH looks like "
                          "this; so does a socket path whose directory does "
                          "not exist.\n",
                  WEXITSTATUS(status), ipc);
        else
          fprintf(stderr, "tetrisdb: the runner was killed by signal %d "
                          "instead of listening on %s\n",
                  WTERMSIG(status), ipc);
        return -1;
      }
    }

    if (now_ms() >= deadline) {
      fprintf(stderr, "tetrisdb: nothing is listening on %s\n", ipc);
      return -1;
    }
    usleep(50000);
  }
}
