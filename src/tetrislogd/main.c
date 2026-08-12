#include "logger.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_terminate(int sig)
{
    (void)sig;
    logd_stop();
}

static void on_hangup(int sig)
{
    (void)sig;
    logd_reopen();
}

/*
 * Install one handler without SA_RESTART: the receive loop relies on the
 * blocking recvfrom() failing with EINTR to notice the flag a handler set.
 */
static int install(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(sig, &sa, NULL) < 0)
    {
        perror("tetrislogd: sigaction");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    logd_opts_t opts;
    memset(&opts, 0, sizeof opts);
    if (config(&opts) < 0)
        return 1;

    int opt;
    while ((opt = getopt(argc, argv, "s:f:l:h")) != -1)
    {
        switch (opt)
        {
        case 's':
            snprintf(opts.socket_path, sizeof opts.socket_path, "%s", optarg);
            break;
        case 'f':
            snprintf(opts.log_path, sizeof opts.log_path, "%s", optarg);
            break;
        case 'l':
            if (log_level_parse(optarg, &opts.min_level) < 0)
            {
                fprintf(stderr, "tetrislogd: unknown level '%s'\n", optarg);
                return 2;
            }
            break;
        case 'h':
            printf("usage: %s [-s socket] [-f logfile] [-l debug|info|warn|error]\n", argv[0]);
            return 0;
        default:
            return 2;
        }
    }
    if (optind != argc)
    {
        fprintf(stderr, "tetrislogd: unexpected argument '%s'\n", argv[optind]);
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

    fprintf(stderr,
            "tetrislogd: exiting (received=%lu filtered=%lu malformed=%lu "
            "truncated=%lu dropped=%lu)\n",
            stats.received, stats.filtered, stats.malformed, stats.truncated,
            stats.dropped);
    return 0;
}
