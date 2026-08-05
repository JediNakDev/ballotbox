/*
 * dspawn - run a program as a daemon, detached from the terminal.
 *
 * The strict form: root working directory, cleared umask, output discarded.
 * Use dspawn2 instead for anything that resolves paths relative to the
 * project root or that needs its startup errors kept.
 *
 * usage: dspawn <executable> [args...]
 */

#include "tetrish/lib/daemonise.h"
#include "tetrish/system_program.h"

int main(int argc, char *argv[]) {
  daemonise_opts_t opts = {0}; /* chdir("/"), umask(0), output discarded */

  if (argc < 2) {
    fprintf(stderr, "usage: dspawn <executable> [args...]\n");
    return EXIT_FAILURE;
  }

  if (daemonise(&opts) < 0)
    return EXIT_FAILURE;

  /* --- daemon process --- replace the image with the requested
     executable so the daemon runs it (e.g. `dspawn tetrisd`). */
  execvp(argv[1], &argv[1]);

  /* execvp only returns on failure. */
  return EXIT_FAILURE;
}
