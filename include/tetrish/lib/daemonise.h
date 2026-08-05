#ifndef TETRISH_LIB_DAEMONISE_H
#define TETRISH_LIB_DAEMONISE_H

/*
 * daemonise.h - detach the calling process from its terminal.
 *
 * The double-fork dance is short but unforgiving: get the order wrong, or
 * exit() where _exit() was needed, and the failure shows up as duplicated
 * output or a daemon that dies with its terminal. dspawn and dspawn2 each had
 * their own copy of it, which is how the same exit()/_exit() bug came to exist
 * in both and be fixed in one.
 *
 * It lives in tetrish/lib rather than libcommon because its callers are system
 * programs, and the Makefile links those against tetrish/lib sources and no
 * libraries at all.
 *
 * The three things the two callers actually disagree about are the options
 * below. Everything else - fork, setsid, ignore SIGCHLD and SIGHUP, fork
 * again, close every descriptor, reopen 0/1/2 - is the same for both and is
 * therefore not their business.
 */

/* How to detach. Zero-initialising this struct gives the strictest form: root
   directory, cleared umask, output discarded. */
typedef struct {
  /* Keep the current working directory instead of chdir("/"). Set this when
     the program being started resolves paths relative to the project root,
     which everything here does: .tetrishrc, var/log, var/db_log, and
     db/dist/simpledb.jar are all relative. */
  int keep_cwd;

  /* Keep the inherited umask instead of umask(0). Set this when files the
     daemon creates should keep ordinary permissions rather than being
     world-writable. */
  int keep_umask;

  /* Where stdout and stderr go. NULL discards them (/dev/null); a path
     appends, so a daemon that dies during startup leaves a reason behind. If
     the file cannot be opened, output falls back to /dev/null rather than
     failing the detach. */
  const char *err_path;
} daemonise_opts_t;

/*
 * Detach, and return only in the daemon process. The two intermediate
 * processes _exit() rather than return, so a caller can treat a return as
 * "I am the daemon now".
 *
 * _exit and not exit: the intermediates share the caller's stdio buffers, and
 * flushing them on the way out prints whatever they hold a second time.
 *
 * Returns 0 in the daemon, or -1 only if the very first fork failed, which is
 * the one failure the original process is still around to hear about. Every
 * later step is past the point of no return: the original process has already
 * exited successfully by then, so a -1 would be returned into a detached
 * process whose exit status nobody is waiting for. Those failures print to
 * stderr - still the caller's terminal until the descriptors are closed - and
 * _exit(EXIT_FAILURE) here rather than handing back a value that implies a
 * caller who can act on it.
 *
 * Note that every descriptor is closed, including any the caller opened
 * before this call. Do the detaching first, then open things.
 */
int daemonise(const daemonise_opts_t *opts);

#endif /* TETRISH_LIB_DAEMONISE_H */
