/*
 * tetrislogd - the tetriSH logging daemon.
 *
 * Collects log records from every other tetriSH process over an AF_UNIX
 * datagram socket and appends them, timestamped, to one file. Centralising
 * this is what makes the log readable: tetrisd forks a worker per session, and
 * a dozen processes appending to the same file would interleave partial lines.
 * Here a datagram is the unit of atomicity, and exactly one process writes.
 *
 * Runs in the foreground and logs its own lifecycle, so it can be supervised
 * (or started by tetrisd) rather than double-forking away from its parent.
 * Terminate it with SIGINT/SIGTERM; rotate its file with mv + SIGHUP.
 *
 * Every value has a built-in default, but .tetrishrc must exist and every
 * log_ directive in it must be valid. Precedence, low to high: built-in
 * default, then .tetrishrc, then the matching command-line flag. Each layer
 * only overrides what the one below it set.
 *
 * Usage: tetrislogd [-s socket] [-f logfile] [-l level] [-e] [-h]
 */

#include "tetrislogd/tetrislogd.h"

#include "libtetrisutil/rc.h"

#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_terminate(int sig) {
  (void)sig;
  logd_stop();
}

static void on_hangup(int sig) {
  (void)sig;
  logd_reopen();
}

/*
 * Install one handler without SA_RESTART: the receive loop relies on the
 * blocking recvfrom() failing with EINTR to notice the flag a handler set.
 */
static int install(int sig, void (*handler)(int)) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction(sig, &sa, NULL) < 0) {
    perror("tetrislogd: sigaction");
    return -1;
  }
  return 0;
}

static void usage(FILE *out, const char *argv0) {
  logd_opts_t defaults;
  logd_opts_default(&defaults);

  fprintf(out,
          "usage: %s [-s socket] [-f logfile] [-l level] [-e]"
          " [-d dbdir] [-D jar]\n"
          "  -s socket   Unix datagram socket to bind (default %s)\n"
          "  -f logfile  file to append records to, '-' for stdout"
          " (default %s)\n"
          "  -l level    minimum severity: debug|info|warn|error"
          " (default debug)\n"
          "  -e          also mirror every record to stderr\n"
          "  -d dbdir    also mirror every record into SimpleDB, storing\n"
          "              catalog.txt and log.dat in dbdir (default off,\n"
          "              dbdir defaults to %s)\n"
          "  -D jar      simpledb.jar to run (default %s); implies -d\n"
          "  -h          show this help\n",
          argv0, defaults.socket_path, defaults.log_path, defaults.db.dir,
          defaults.db.jar);
}

int main(int argc, char **argv) {
  logd_opts_t opts;
  logd_opts_default(&opts);
  if (logd_load_rc(RC_PATH, &opts) < 0)
    return 1;

  int opt;
  while ((opt = getopt(argc, argv, "s:f:l:ed:D:h")) != -1) {
    switch (opt) {
    case 's':
      snprintf(opts.socket_path, sizeof(opts.socket_path), "%s", optarg);
      break;
    case 'f':
      snprintf(opts.log_path, sizeof(opts.log_path), "%s", optarg);
      break;
    case 'l':
      if (log_level_parse(optarg, &opts.min_level) < 0) {
        fprintf(stderr, "tetrislogd: unknown level '%s'\n", optarg);
        return 2;
      }
      break;
    case 'e':
      opts.echo = 1;
      break;
    case 'd':
      /* -d turns mirroring on and names the data directory in one go: the
         two are never useful apart. */
      opts.db_enable = 1;
      snprintf(opts.db.dir, sizeof(opts.db.dir), "%s", optarg);
      break;
    case 'D':
      opts.db_enable = 1;
      snprintf(opts.db.jar, sizeof(opts.db.jar), "%s", optarg);
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 0;
    default:
      usage(stderr, argv[0]);
      return 2;
    }
  }
  if (optind != argc) {
    fprintf(stderr, "tetrislogd: unexpected argument '%s'\n", argv[optind]);
    usage(stderr, argv[0]);
    return 2;
  }

  /* A sender that vanishes mid-send must not kill us; we never write to a
   * pipe, but a supervisor closing our stdout could otherwise be fatal. */
  if (install(SIGPIPE, SIG_IGN) < 0 || install(SIGINT, on_terminate) < 0 ||
      install(SIGTERM, on_terminate) < 0 || install(SIGHUP, on_hangup) < 0)
    return 1;

  logd_stats_t stats;
  if (logd_run(&opts, &stats) < 0)
    return 1;

  /* Repeat the counters on stderr: whoever started us in a terminal should
   * not have to open the log file to see whether records were lost. */
  fprintf(stderr,
          "tetrislogd: exiting (received=%lu filtered=%lu malformed=%lu "
          "truncated=%lu)\n",
          stats.received, stats.filtered, stats.malformed, stats.truncated);
  return 0;
}
