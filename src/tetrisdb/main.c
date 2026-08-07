/*
 * tetrisdb - launch and provision the shared SocketRunner.
 *
 * Phase 4 (#52, amended by #58 and #60). The decision is recorded in
 * docs/adr/0002-starting-the-shared-socketrunner.md; the ordering and the full
 * step list are in docs/libtetrisauth.md.
 *
 * One-shot, three verbs, not a daemon and not run under dspawn:
 *
 *   tetrisdb start   preflight, spawn the JVM, wait for readiness, report, exit
 *   tetrisdb check   is a runner serving var/db right now?  exit 0/1, a short
 *                    report of the configuration, the secret and the runner
 *   tetrisdb stop    SIGTERM the runner named by the lockfile
 *
 * The two things most easily got wrong, both recorded in ADR 0002:
 *
 *   - The flock on <db_dir>/.runner.lock is HELD ACROSS THE FORK AND EXEC, so
 *     the JVM owns it for exactly its own lifetime with no cleanup path to get
 *     wrong. Without it, running start twice reaches the two-JVM corruption
 *     case with no error anywhere, because SocketRunner.bind() unlinks a live
 *     runner's socket. A connect() probe is not a substitute: it races, and it
 *     guards the socket rather than the directory.
 *   - Step 8's readiness poll gates NOTHING. It decides what is logged and
 *     what is returned. Nothing in the system may cache runner health.
 *
 * The name is already slightly a lie: steps 3, 4 and 5 provision libtetrisauth's
 * resources, not the database's. Not renamed, deliberately - churn across the
 * ADR, the rc line and #60's reader table for zero behaviour change.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "libcommon/logmsg.h"
#include "libcommon/rc.h"
#include "libtetrisauth/config.h"
#include "libtetrisauth/provision.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/runner.h"

#define REG_SEM_NAME "/tetrish_register"
#define RUNNER_ERR_PATH "var/log/tetrisdb.err"
#define LOG_SOCKET_PATH "var/run/tetrislogd.sock"
#define STOP_WAIT_MS 10000

static void report(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void report(log_level_t level, const char *fmt, ...) {
  va_list ap;
  va_list log_ap;

  va_start(ap, fmt);
  va_copy(log_ap, ap);
  fprintf(stderr, "tetrisdb: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  (void)log_vsend(level, fmt, log_ap);
  va_end(log_ap);
  va_end(ap);
}

/* The lock has to exist before tdb_ensure_table() creates the table, so its
 * directory is the one small piece of step 1 that may need creating. */
static int mkdir_parent(const char *path) {
  char buf[PATH_MAX];

  if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) {
    report(LOG_ERROR, "path is too long: %s", path);
    return -1;
  }

  char *slash = strrchr(buf, '/');
  if (slash == NULL || slash == buf)
    return 0; /* the working directory, or "/" - both already exist */
  *slash = '\0';
  return tdb_mkdir_p(buf);
}

static int runner_lock_path(const char *db_dir, char *path, size_t pathlen) {
  if (snprintf(path, pathlen, "%s/.runner.lock", db_dir) >= (int)pathlen) {
    report(LOG_ERROR, "runner lock path is too long for %s", db_dir);
    return -1;
  }
  return 0;
}

/**
 * Takes the runner lock and clears any stale pid from it.
 *
 * The fd hygiene the JVM inheritance depends on is tdb_runner_lock()'s, so it
 * is enforced by the module that imposes it rather than restated here.
 */
static int acquire_lock(const char *db_dir, char *path, size_t pathlen) {
  int fd = tdb_runner_lock(db_dir, path, pathlen);
  if (fd < 0)
    return -1;

  /* Once the lock is ours, any old pid is necessarily stale. Clear it before
   * preflight so the file names an owner exactly when one exists. A duplicate
   * start never reaches this line and therefore cannot erase the live pid. */
  if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 || fsync(fd) != 0) {
    report(LOG_ERROR, "clear %s: %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

/* Catalog lines are `name (columns...)`. Match the table token exactly, so a
 * harmless table named `logmeta` is not mistaken for tetrislogd's `log`. */
static int catalog_declares_log(const char *db_dir) {
  char path[PATH_MAX];
  tdb_catalog_path(path, sizeof path, db_dir);

  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (errno == ENOENT)
      return 0;
    report(LOG_ERROR, "open %s: %s", path, strerror(errno));
    return -1;
  }

  char line[1024];
  int found = 0;
  while (!found && fgets(line, sizeof line, file) != NULL) {
    char *start = line;
    while (*start == ' ' || *start == '\t')
      start++;
    char *paren = strchr(start, '(');
    if (paren == NULL)
      continue;
    char *end = paren;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
      end--;
    found = end - start == 3 && memcmp(start, "log", 3) == 0;
  }
  if (ferror(file)) {
    report(LOG_ERROR, "read %s: %s", path, strerror(errno));
    fclose(file);
    return -1;
  }
  fclose(file);
  return found;
}

static int recreate_registration_semaphore(void) {
  if (sem_unlink(REG_SEM_NAME) != 0 && errno != ENOENT) {
    report(LOG_ERROR, "sem_unlink(%s): %s", REG_SEM_NAME, strerror(errno));
    return -1;
  }

  sem_t *sem = sem_open(REG_SEM_NAME, O_CREAT | O_EXCL, 0600, 1);
  if (sem == SEM_FAILED) {
    report(LOG_ERROR, "sem_open(%s): %s", REG_SEM_NAME, strerror(errno));
    return -1;
  }
  sem_close(sem);
  return 0;
}

static int record_pid(int lock_fd, const char *lock_path, pid_t pid) {
  char text[64];
  int len = snprintf(text, sizeof text, "%ld\n", (long)pid);

  if (ftruncate(lock_fd, 0) != 0 || lseek(lock_fd, 0, SEEK_SET) < 0) {
    report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
    return -1;
  }
  size_t written = 0;
  while (written < (size_t)len) {
    ssize_t n = write(lock_fd, text + written, (size_t)len - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
      return -1;
    }
    if (n == 0) {
      report(LOG_ERROR, "write %s: wrote zero bytes", lock_path);
      return -1;
    }
    written += (size_t)n;
  }
  if (fsync(lock_fd) != 0) {
    report(LOG_ERROR, "write %s: %s", lock_path, strerror(errno));
    return -1;
  }
  return 0;
}

static int clear_pid(int lock_fd, const char *lock_path) {
  if (ftruncate(lock_fd, 0) != 0 || lseek(lock_fd, 0, SEEK_SET) < 0 ||
      fsync(lock_fd) != 0) {
    report(LOG_ERROR, "clear %s: %s", lock_path, strerror(errno));
    return -1;
  }
  return 0;
}

static void stop_failed_child(pid_t pid) {
  if (pid <= 0)
    return;
  (void)kill(pid, SIGTERM);

  struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
  for (int i = 0; i < 100; i++) {
    pid_t done = waitpid(pid, NULL, WNOHANG);
    if (done == pid || (done < 0 && errno == ECHILD))
      return;
    nanosleep(&pause, NULL);
  }

  (void)kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
    ;
}

/*
 * Loads and validates the rc file. Called by start() and check().
 *
 * RC_PATH is relative, so it resolves against the working directory, which is
 * the checkout under the supported launcher (ADR 0003). A missing file is a
 * refusal rather than a silent run on defaults: rc_bind() reports RC_E_OPEN
 * and both verbs stop here, before the lock, the semaphore or the socket.
 */
static int load_config(tdb_runner_opts_t *opts, int validate_auth) {
  tdb_runner_opts_default(opts);

  int applied = tdb_runner_opts_load(RC_PATH, opts);
  if (applied < 0)
    return -1;
  if (validate_auth && tauth_rc_validate(RC_PATH) < 0)
    return -1;
  return applied;
}

static int start(void) {
  tdb_runner_opts_t opts;

  /* Configuration is the only work before the lock. A typo must not create a
   * lock file, reset a live semaphore, provision a secret or unlink a socket. */
  if (load_config(&opts, 1) < 0)
    return 1;

  (void)log_open(LOG_SOCKET_PATH);

  char lock_path[PATH_MAX];
  int lock_fd = acquire_lock(opts.dir, lock_path, sizeof lock_path);
  if (lock_fd < 0)
    goto fail;

  int has_log = catalog_declares_log(opts.dir);
  if (has_log != 0) {
    if (has_log > 0)
      report(LOG_ERROR,
             "%s/catalog.txt declares table 'log', which belongs to "
             "tetrislogd's separate runner",
             opts.dir);
    goto fail_locked;
  }

  if (recreate_registration_semaphore() != 0)
    goto fail_locked;

  if (tdb_ensure_table(opts.dir, TETRISAUTH_DB_TABLE,
                       TETRISAUTH_DB_SCHEMA) != 0) {
    report(LOG_ERROR, "could not provision table '%s' in %s",
           TETRISAUTH_DB_TABLE, opts.dir);
    goto fail_locked;
  }

  char secret_err[256];
  if (tauth_secret_provision(".", secret_err, sizeof secret_err) != 0) {
    report(LOG_ERROR, "cannot provision auth/jwt_secret: %s", secret_err);
    goto fail_locked;
  }

  if (mkdir_parent(opts.ipc) != 0)
    goto fail_locked;

  if (unlink(opts.ipc) != 0 && errno != ENOENT) {
    report(LOG_ERROR, "unlink stale socket %s: %s", opts.ipc, strerror(errno));
    goto fail_locked;
  }

  if (mkdir_parent(RUNNER_ERR_PATH) != 0)
    goto fail_locked;

  int err_fd = open(RUNNER_ERR_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);
  if (err_fd < 0) {
    report(LOG_ERROR, "open %s: %s", RUNNER_ERR_PATH, strerror(errno));
    goto fail_locked;
  }

  pid_t pid = tdb_runner_spawn(&opts, err_fd);
  close(err_fd);
  if (pid < 0) {
    report(LOG_ERROR, "runner did not start; see %s", RUNNER_ERR_PATH);
    goto fail_locked;
  }

  if (record_pid(lock_fd, lock_path, pid) != 0) {
    stop_failed_child(pid);
    (void)clear_pid(lock_fd, lock_path);
    goto fail_locked;
  }

  if (tdb_runner_wait(opts.ipc, pid, 0) != 0) {
    stop_failed_child(pid);
    (void)unlink(opts.ipc);
    (void)clear_pid(lock_fd, lock_path);
    report(LOG_ERROR, "runner failed readiness; see %s", RUNNER_ERR_PATH);
    goto fail_locked;
  }

  report(LOG_INFO, "started runner pid=%ld socket=%s sessions=%d",
         (long)pid, opts.ipc, opts.sessions);
  close(lock_fd); /* the JVM's inherited descriptor keeps the lock held */
  log_close();
  return 0;

fail_locked:
  close(lock_fd);
fail:
  log_close();
  return 1;
}

static int check(void) {
  tdb_runner_opts_t opts;
  int directives = load_config(&opts, 1);
  if (directives < 0)
    return 1;

  fprintf(stderr,
          "tetrisdb: configuration: rc=%s directives=%d db_dir=%s "
          "db_ipc=%s db_sessions=%d db_jar=%s db_java=%s\n",
          RC_PATH, directives, opts.dir, opts.ipc, opts.sessions, opts.jar,
          opts.java);

  unsigned char secret[64];
  int secret_len = tauth_secret_load(".", secret, sizeof secret);
  OPENSSL_cleanse(secret, sizeof secret);
  if (secret_len < 0)
    fprintf(stderr, "tetrisdb: secret: unusable path=auth/jwt_secret\n");
  else
    fprintf(stderr,
            "tetrisdb: secret: usable path=auth/jwt_secret bytes=%d\n",
            secret_len);

  int reachable = tdb_runner_wait(opts.ipc, -1, 1) == 0;
  fprintf(stderr, "tetrisdb: runner: %s socket=%s\n",
          reachable ? "reachable" : "unavailable", opts.ipc);
  return secret_len >= 0 && reachable ? 0 : 1;
}

static int read_lock_pid(int fd, const char *lock_path, pid_t *pid) {
  char buf[64];
  ssize_t n = pread(fd, buf, sizeof buf, 0);
  if (n < 0) {
    report(LOG_ERROR, "read %s: %s", lock_path, strerror(errno));
    return -1;
  }
  if (n == 0)
    return 1;

  size_t len = (size_t)n;
  if (len > 0 && buf[len - 1] == '\n')
    len--;
  int malformed = len == 0 || (size_t)n == sizeof buf;
  for (size_t i = 0; i < len && !malformed; i++)
    malformed = buf[i] < '0' || buf[i] > '9';
  if (malformed) {
    report(LOG_ERROR, "%s is malformed - expected one positive runner pid",
           lock_path);
    return -1;
  }
  buf[len] = '\0';

  char *end;
  errno = 0;
  long value = strtol(buf, &end, 10);
  if (errno != 0 || *end != '\0' || value <= 1 || value > INT_MAX) {
    report(LOG_ERROR, "%s is malformed - expected one positive runner pid",
           lock_path);
    return -1;
  }

  *pid = (pid_t)value;
  return 0;
}

static int stop(void) {
  tdb_runner_opts_t opts;
  if (load_config(&opts, 0) < 0)
    return 1;

  char lock_path[PATH_MAX];
  if (runner_lock_path(opts.dir, lock_path, sizeof lock_path) != 0)
    return 1;

  int fd = open(lock_path, O_RDWR);
  if (fd < 0) {
    if (errno == ENOENT) {
      report(LOG_INFO, "already stopped - no lockfile at %s", lock_path);
      return 0;
    }
    if (errno == EACCES || errno == EPERM)
      report(LOG_ERROR, "permission denied opening %s: %s", lock_path,
             strerror(errno));
    else
      report(LOG_ERROR, "open %s: %s", lock_path, strerror(errno));
    return 1;
  }

  /* The pid text has authority only while another open file description holds
   * this lock. If this acquisition succeeds, its contents are stale data and
   * must never be passed to kill(). */
  int unlocked = flock(fd, LOCK_EX | LOCK_NB) == 0;
  if (!unlocked && errno != EWOULDBLOCK && errno != EAGAIN) {
    report(LOG_ERROR, "inspect lock %s: %s", lock_path, strerror(errno));
    close(fd);
    return 1;
  }

  pid_t pid;
  int pid_state = read_lock_pid(fd, lock_path, &pid);
  if (pid_state > 0 && unlocked) {
    report(LOG_INFO, "already stopped - %s has no runner pid", lock_path);
    (void)flock(fd, LOCK_UN);
    close(fd);
    return 0;
  }
  if (pid_state != 0) {
    if (pid_state > 0)
      report(LOG_ERROR, "%s is malformed - it is locked but contains no pid",
             lock_path);
    if (unlocked)
      (void)flock(fd, LOCK_UN);
    close(fd);
    return 1;
  }

  /* Nobody holds the lock, so the runner that pid named is already gone - it
   * died without running its shutdown hook, because a clean stop clears the
   * pid on the way out. That is "already stopped" rather than a failure, and
   * saying otherwise breaks the documented `tetrisdb stop && tetrisdb start`
   * restart at exactly the moment an operator reaches for it: after a crash.
   * The pid is CLEARED, NEVER SIGNALLED - without the lock the text has no
   * authority, and the number may since have been recycled onto an unrelated
   * process. The crash itself is still reported, because a runner that
   * vanished is worth seeing in the log. */
  if (unlocked) {
    report(LOG_INFO,
           "already stopped - %s named pid %ld but no process holds the "
           "directory lock, so the runner died without clearing it",
           lock_path, (long)pid);
    int cleared = clear_pid(fd, lock_path);
    (void)flock(fd, LOCK_UN);
    close(fd);
    return cleared == 0 ? 0 : 1;
  }

  if (kill(pid, 0) != 0) {
    if (errno == EPERM)
      report(LOG_ERROR, "permission denied checking runner pid %ld",
             (long)pid);
    else
      report(LOG_ERROR,
             "stale runner state - %s names missing pid %ld; not signaling",
             lock_path, (long)pid);
    close(fd);
    return 1;
  }

  if (kill(pid, SIGTERM) != 0) {
    if (errno == EPERM)
      report(LOG_ERROR, "permission denied signaling runner pid %ld",
             (long)pid);
    else
      report(LOG_ERROR, "signal runner pid %ld: %s", (long)pid,
             strerror(errno));
    close(fd);
    return 1;
  }

  struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
  long long waited_ms = 0;
  while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      report(LOG_ERROR, "wait for %s: %s", lock_path, strerror(errno));
      close(fd);
      return 1;
    }
    if (waited_ms >= STOP_WAIT_MS) {
      report(LOG_ERROR,
             "timeout waiting for runner pid %ld to finish its shutdown "
             "hook; it was not killed",
             (long)pid);
      close(fd);
      return 1;
    }
    nanosleep(&pause, NULL);
    waited_ms += 50;
  }

  if (clear_pid(fd, lock_path) != 0) {
    (void)flock(fd, LOCK_UN);
    close(fd);
    return 1;
  }
  (void)flock(fd, LOCK_UN);
  close(fd);

  struct stat socket_st;
  if (lstat(opts.ipc, &socket_st) == 0) {
    report(LOG_ERROR,
           "runner pid %ld stopped but shutdown left socket %s in place",
           (long)pid, opts.ipc);
    return 1;
  }
  if (errno != ENOENT) {
    report(LOG_ERROR, "cannot verify shutdown socket cleanup at %s: %s",
           opts.ipc, strerror(errno));
    return 1;
  }

  report(LOG_INFO, "stopped runner pid=%ld socket=%s", (long)pid, opts.ipc);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: tetrisdb {start|check|stop}\n");
    return 2;
  }

  if (strcmp(argv[1], "start") == 0)
    return start();
  if (strcmp(argv[1], "check") == 0)
    return check();
  if (strcmp(argv[1], "stop") == 0)
    return stop();

  fprintf(stderr, "tetrisdb: unknown command '%s'\n", argv[1]);
  return 2;
}
