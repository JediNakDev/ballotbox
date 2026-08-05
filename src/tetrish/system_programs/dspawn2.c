/*
 * dspawn2 - run any program as a daemon, without leaving the project root.
 *
 * dspawn already daemonises, but it chdir("/")s on the way, and every path
 * this project uses - .tetrishrc, var/log, var/run, var/db_log,
 * db/dist/simpledb.jar - is relative to the project root. A daemon started
 * that way looks for all of them under /. dspawn2 is therefore dspawn with
 * the working directory kept, plus three things dspawn does not offer:
 *
 *   - a pidfile named after the program, so it can be signalled later;
 *   - a refusal to start a second copy, because two daemons writing one
 *     log.dat or one .pid would corrupt each other's files;
 *   - stdout and stderr pointed at var/log/<name>.err rather than
 *     /dev/null, so a daemon that dies during startup says why.
 *
 * usage: dspawn2 <executable> [args...]
 *
 * Stopping it is `kill $(cat var/run/<name>.pid)`.
 */

#include "tetrish/lib/daemonise.h"
#include "tetrish/system_program.h"

#define PID_DIR "var/run"
#define ERR_DIR "var/log"

/* Read the recorded pid. Returns 0 and stores it, or -1 if there is no
   pidfile or it does not hold a positive number. */
static int read_pidfile(const char *pidfile, long *out) {
  long pid = 0;
  FILE *f = fopen(pidfile, "r");

  if (f == NULL)
    return -1;

  int got = fscanf(f, "%ld", &pid);
  fclose(f);
  if (got != 1 || pid <= 0)
    return -1;

  *out = pid;
  return 0;
}

/* Remove the pidfile, but only if it still names this process. Between
   writing it and failing to exec, a racing dspawn2 may have overwritten it
   with its own pid; unlinking then would strip a live daemon of the only
   record by which it can be signalled. */
static void unlink_own_pidfile(const char *pidfile) {
  long pid = 0;

  if (read_pidfile(pidfile, &pid) == 0 && (pid_t)pid == getpid())
    unlink(pidfile);
}

static int already_running(const char *pidfile) {
  long pid = 0;

  if (read_pidfile(pidfile, &pid) < 0)
    return 0; /* no pidfile, or an unreadable one: nothing claims to run */

  /* kill(0) tests for a signalable process without sending anything; EPERM
     means it is alive but someone else's, which still counts as running. */
  if (kill((pid_t)pid, 0) == 0 || errno == EPERM) {
    fprintf(stderr,
            "dspawn2: already running (pid %ld); a second copy would fight it "
            "over its files\n",
            pid);
    return 1;
  }
  return 0;
}

static int spawn_daemon(const char *pidfile, const char *errfile) {
  /* keep_cwd, because every path this project opens - .tetrishrc, var/log,
     var/db_log, db/dist/simpledb.jar - is relative to the project root.
     keep_umask, so files the daemon creates keep ordinary permissions.
     errfile, so a daemon that dies during startup says why. */
  daemonise_opts_t opts = {
      .keep_cwd = 1,
      .keep_umask = 1,
      .err_path = errfile,
  };

  if (daemonise(&opts) < 0)
    return EXIT_FAILURE;

  /* --- daemon process from here on --- */

  /* Record the pid. Written here, by the daemon itself, because after two
     forks nobody else knows it - and getpid() is unchanged by the execvp
     below, so it stays correct. */
  FILE *f = fopen(pidfile, "w");
  if (f == NULL) {
    /* A daemon nobody can signal is worse than none. stderr is the error log
       by now, so the reason survives even though the caller has already
       detached and cannot be told. */
    perror("dspawn2: cannot write pidfile");
    return EXIT_FAILURE;
  }
  fprintf(f, "%d\n", (int)getpid());
  fclose(f);

  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  char pidfile[PATH_MAX], errfile[PATH_MAX];
  const char *slash, *name;

  if (argc < 2) {
    fprintf(stderr, "usage: dspawn2 <executable> [args...]\n");
    return EXIT_FAILURE;
  }

  /* Name the pidfile and error log after the program: "bin/tetrislogd"
     becomes var/run/tetrislogd.pid and var/log/tetrislogd.err. */
  slash = strrchr(argv[1], '/');
  name = slash != NULL ? slash + 1 : argv[1];
  snprintf(pidfile, sizeof(pidfile), "%s/%s.pid", PID_DIR, name);
  snprintf(errfile, sizeof(errfile), "%s/%s.err", ERR_DIR, name);

  if (already_running(pidfile)) {
    return EXIT_FAILURE;
  }

  if (spawn_daemon(pidfile, errfile) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }

  /* --- daemon process --- replace the image with the requested
     executable so the daemon runs it (e.g. `dspawn2 tetrislogd`). */
  execvp(argv[1], &argv[1]);

  /* execvp only returns on failure. Remove the pidfile: it names a process
     that is about to stop existing. */
  perror("dspawn2: exec failed");
  unlink_own_pidfile(pidfile);
  return EXIT_FAILURE;
}
