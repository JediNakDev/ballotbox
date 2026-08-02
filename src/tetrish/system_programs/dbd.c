/*
 * dbd - run the logging daemon, with its SimpleDB mirror, in the background.
 *
 * tetrislogd runs in the foreground by design, so something has to detach it.
 * dspawn already does that, but it chdir("/")s on the way, and every path
 * tetriSH uses - .tetrishrc, var/log, var/run, var/db, db/dist/simpledb.jar -
 * is relative to the project root. A daemon started that way would look for
 * all of them under /. dbd is therefore dspawn with the working directory
 * kept, plus the one thing dspawn cannot offer: it records the daemon's pid,
 * so "stop" and "status" are answerable without grepping ps output.
 *
 * usage: dbd start [dbdir] | stop | status
 *
 * The database is what makes this worth a program of its own: bringing the
 * mirror up means tetrislogd spawns a JVM, creates the log table if this is a
 * first run, and becomes the sole owner of that table. Only one may run at a
 * time - two daemons writing the same log.dat would corrupt it, each through
 * its own private page cache - which is exactly what the pidfile prevents.
 */

#include "tetrish/system_program.h"

#define LOGD_BIN     "bin/tetrislogd"
#define PID_PATH     "var/run/tetrislogd.pid"
#define ERR_PATH     "var/log/tetrislogd.err"
#define DEFAULT_DB   "var/db"

/* How long to wait for a stopped daemon to actually exit, in 50ms steps.
 * Generous because a clean shutdown drains the queue and waits for the JVM
 * child to flush its dirty pages. */
#define STOP_WAIT_STEPS 200

/* Read the recorded pid. Returns 0 and stores it, or -1 if there is no
 * pidfile or it does not hold a number. */
static int read_pid(pid_t *out) {
  FILE *f = fopen(PID_PATH, "r");
  if (f == NULL)
    return -1;

  long v = 0;
  int got = fscanf(f, "%ld", &v);
  fclose(f);

  if (got != 1 || v <= 0)
    return -1;
  *out = (pid_t)v;
  return 0;
}

/* Is that pid still ours to talk to? kill(0) answers "alive and signalable"
 * without sending anything; EPERM means alive but owned by someone else,
 * which for our purposes still counts as running. */
static int alive(pid_t pid) {
  return kill(pid, 0) == 0 || errno == EPERM;
}

/* Refuse to start a second daemon on top of a live one, and clean up after a
 * daemon that died without removing its pidfile. Returns 0 if starting is
 * safe. */
static int check_not_running(void) {
  pid_t pid;

  if (read_pid(&pid) < 0)
    return 0; /* no pidfile: nothing claims to be running */

  if (alive(pid)) {
    fprintf(stderr,
            "dbd: tetrislogd already running (pid %d)\n"
            "     two daemons would share one log table and corrupt it\n",
            (int)pid);
    return -1;
  }

  fprintf(stderr, "dbd: removing stale pidfile for pid %d\n", (int)pid);
  unlink(PID_PATH);
  return 0;
}

/* Point stdin at /dev/null and stdout/stderr at the error log, so a daemon
 * that complains (a missing jar, a failed insert) leaves a trace instead of
 * writing into whatever fd it inherited. */
static void redirect_io(void) {
  int null_fd = open("/dev/null", O_RDONLY);
  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    close(null_fd);
  }

  int err_fd = open(ERR_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);
  if (err_fd >= 0) {
    dup2(err_fd, STDOUT_FILENO);
    dup2(err_fd, STDERR_FILENO);
    close(err_fd);
  }
}

/*
 * Double-fork tetrislogd into the background and report the pid it ended up
 * with through pid_pipe.
 *
 * The second fork is what keeps the daemon from being a session leader, so it
 * can never acquire a controlling terminal. The pipe exists because that same
 * fork hides the final pid from the original process - the intermediate is
 * the only one that knows it.
 *
 * Note what is deliberately missing compared to dspawn: no chdir("/"), so
 * relative paths still resolve against the project root, and no umask(0),
 * since the log file and the database are not meant to be world-writable.
 */
static int spawn(const char *dbdir, int pid_pipe[2]) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("dbd: fork");
    return -1;
  }
  if (pid > 0) {
    close(pid_pipe[1]);
    return 0; /* parent: the caller reads the pid */
  }

  /* --- intermediate --- */
  close(pid_pipe[0]);

  if (setsid() < 0)
    _exit(EXIT_FAILURE);

  signal(SIGHUP, SIG_IGN); /* outliving this session must not kill it */

  pid = fork();
  if (pid < 0)
    _exit(EXIT_FAILURE);
  if (pid > 0) {
    /* Hand the daemon's real pid back before disappearing. */
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", (int)pid);
    (void)write(pid_pipe[1], buf, (size_t)n);
    close(pid_pipe[1]);
    _exit(EXIT_SUCCESS);
  }

  /* --- daemon --- */
  close(pid_pipe[1]);
  redirect_io();

  execl(LOGD_BIN, LOGD_BIN, "-d", dbdir, (char *)NULL);
  perror("dbd: exec " LOGD_BIN);
  _exit(127);
}

static int cmd_start(const char *dbdir) {
  if (check_not_running() < 0)
    return EXIT_FAILURE;

  int pid_pipe[2];
  if (pipe(pid_pipe) < 0) {
    perror("dbd: pipe");
    return EXIT_FAILURE;
  }

  if (spawn(dbdir, pid_pipe) < 0) {
    close(pid_pipe[0]);
    close(pid_pipe[1]);
    return EXIT_FAILURE;
  }

  char buf[32] = {0};
  ssize_t n = read(pid_pipe[0], buf, sizeof(buf) - 1);
  close(pid_pipe[0]);

  /* Reap the intermediate, which exits as soon as it has written the pid. */
  int status;
  wait(&status);

  if (n <= 0) {
    fprintf(stderr, "dbd: daemon failed to start (see %s)\n", ERR_PATH);
    return EXIT_FAILURE;
  }

  pid_t pid = (pid_t)atoi(buf);
  FILE *f = fopen(PID_PATH, "w");
  if (f == NULL) {
    /* The daemon is up; only our bookkeeping failed. Say so rather than
       leaving a running process nobody has a handle on. */
    fprintf(stderr, "dbd: started (pid %d) but cannot write %s: %s\n",
            (int)pid, PID_PATH, strerror(errno));
    return EXIT_FAILURE;
  }
  fprintf(f, "%d\n", (int)pid);
  fclose(f);

  printf("tetrislogd started: pid " COLOR_GREEN "%d" COLOR_RESET
         ", database %s\n",
         (int)pid, dbdir);
  return EXIT_SUCCESS;
}

static int cmd_stop(void) {
  pid_t pid;

  if (read_pid(&pid) < 0) {
    fprintf(stderr, "dbd: no pidfile at %s - nothing to stop\n", PID_PATH);
    return EXIT_FAILURE;
  }

  /* SIGTERM, never SIGKILL: the daemon's shutdown path is what drains the
     pending inserts and lets SimpleDB flush its pages to disk. */
  if (kill(pid, SIGTERM) < 0) {
    fprintf(stderr, "dbd: cannot signal pid %d: %s\n", (int)pid,
            strerror(errno));
    unlink(PID_PATH);
    return EXIT_FAILURE;
  }

  for (int i = 0; i < STOP_WAIT_STEPS; i++) {
    if (!alive(pid)) {
      unlink(PID_PATH);
      printf("tetrislogd stopped (pid %d)\n", (int)pid);
      return EXIT_SUCCESS;
    }
    usleep(50000);
  }

  fprintf(stderr,
          "dbd: pid %d still running after %d seconds; leaving %s in place\n",
          (int)pid, STOP_WAIT_STEPS / 20, PID_PATH);
  return EXIT_FAILURE;
}

static int cmd_status(void) {
  pid_t pid;

  if (read_pid(&pid) < 0) {
    printf("tetrislogd: " COLOR_YELLOW "not running" COLOR_RESET "\n");
    return EXIT_FAILURE;
  }
  if (!alive(pid)) {
    printf("tetrislogd: " COLOR_RED "not running" COLOR_RESET
           " (stale pidfile for %d)\n",
           (int)pid);
    return EXIT_FAILURE;
  }

  printf("tetrislogd: " COLOR_GREEN "running" COLOR_RESET " (pid %d)\n",
         (int)pid);
  return EXIT_SUCCESS;
}

static void usage(FILE *out, const char *argv0) {
  fprintf(out,
          "usage: %s start [dbdir] | stop | status\n"
          "  start   run tetrislogd in the background with its SimpleDB\n"
          "          mirror on, storing the log table in dbdir"
          " (default %s)\n"
          "  stop    ask it to shut down cleanly and flush the database\n"
          "  status  report whether it is running\n"
          "\n"
          "Run from the project root: every path is relative to it.\n",
          argv0, DEFAULT_DB);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "start") == 0)
    return cmd_start(argc > 2 ? argv[2] : DEFAULT_DB);
  if (strcmp(argv[1], "stop") == 0)
    return cmd_stop();
  if (strcmp(argv[1], "status") == 0)
    return cmd_status();
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
    usage(stdout, argv[0]);
    return EXIT_SUCCESS;
  }

  fprintf(stderr, "dbd: unknown command '%s'\n", argv[1]);
  usage(stderr, argv[0]);
  return EXIT_FAILURE;
}
