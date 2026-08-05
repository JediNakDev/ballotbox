/*
 * daemonise.c - the double-fork detach, once.
 *
 * See tetrish/lib/daemonise.h for what the options mean and why this lives
 * here rather than in libcommon.
 */

#include "tetrish/lib/daemonise.h"

#include "tetrish/system_program.h"

int daemonise(const daemonise_opts_t *opts) {
  pid_t pid;
  int x;

  pid = fork();
  if (pid < 0) {
    perror("daemonise: first fork");
    return -1;
  }
  if (pid > 0) {
    /* _exit, not exit: this process shares the caller's stdio buffers, and
       flushing them here would print whatever they hold a second time. */
    _exit(EXIT_SUCCESS);
  }

  /* --- intermediate process from here on --- */

  /* Become session leader and lose the controlling TTY. */
  if (setsid() < 0) {
    perror("daemonise: setsid");
    _exit(EXIT_FAILURE);
  }

  /* Ignore SIGCHLD (no zombies) and SIGHUP (so the daemon is not killed when
     this session leader terminates). */
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);

  pid = fork();
  if (pid < 0) {
    perror("daemonise: second fork");
    _exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    _exit(EXIT_SUCCESS);
  }

  /* --- daemon process from here on --- */

  if (!opts->keep_umask)
    umask(0);

  if (!opts->keep_cwd && chdir("/") < 0) {
    perror("daemonise: chdir");
    _exit(EXIT_FAILURE);
  }

  /* Close everything, then rebuild 0/1/2. Done in this order so the caller
     cannot be left with a half-detached process holding terminal
     descriptors. */
  for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--)
    close(x);

  if (open("/dev/null", O_RDONLY) < 0) /* stdin -> fd 0 */
    _exit(EXIT_FAILURE);

  int out = -1;
  if (opts->err_path != NULL)
    out = open(opts->err_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
  if (out < 0)
    out = open("/dev/null", O_WRONLY); /* stdout -> fd 1 */
  if (out < 0)
    _exit(EXIT_FAILURE);

  if (dup(out) < 0) /* stderr -> fd 2 */
    _exit(EXIT_FAILURE);

  return 0;
}
