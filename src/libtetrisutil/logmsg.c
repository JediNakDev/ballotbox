/*
 * logmsg.c - sender side of the tetriSH logging IPC, plus level helpers.
 *
 * The sender is deliberately tiny and failure-tolerant: one process-wide
 * non-blocking datagram socket, connect()ed to tetrislogd's path so that a
 * send needs no address and the kernel can report a vanished daemon. Nothing
 * here can block, allocate, or abort, because every tetriSH component calls
 * it from paths where a stall would be visible to a player.
 */

#include "libtetrisutil/logmsg.h"

#include "libtetrisutil/rc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Process-wide sender state. -1 fd means "not open": every send is a drop. */
static int  g_fd = -1;
static char g_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static unsigned long g_dropped;
static int  g_max_attempts = 2; /* overridable via log_send_attempts in rc */

static void apply_rc(const char *key, const char *value, void *ctx)
{
    (void)ctx;
    if (strcmp(key, "log_send_attempts") != 0)
        return;

    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0' || n < 1 || n > 1000) {
        fprintf(stderr,
                "libtetrisutil: rc: invalid log_send_attempts '%s', keeping %d\n",
                value, g_max_attempts);
        return;
    }
    g_max_attempts = (int)n;
}

void log_load_rc(const char *rc_path)
{
    if (rc_load(rc_path, apply_rc, NULL) < 0)
        return; /* sender has no startup to refuse; keep the current value */
}

const char *log_level_str(log_level_t level)
{
    switch (level) {
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_DEBUG: return "DEBUG";
    }
    return "?????";
}

int log_level_rank(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return 0;
    case LOG_INFO:  return 1;
    case LOG_WARN:  return 2;
    case LOG_ERROR: return 3;
    }
    return 4; /* unknown: ranks above ERROR so filtering never hides it */
}

int log_level_parse(const char *name, log_level_t *out)
{
    static const struct {
        const char *name;
        log_level_t level;
    } names[] = {
        { "info",  LOG_INFO  },
        { "warn",  LOG_WARN  },
        { "error", LOG_ERROR },
        { "debug", LOG_DEBUG },
    };

    if (name == NULL || out == NULL)
        return -1;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(name, names[i].name) == 0) {
            *out = names[i].level;
            return 0;
        }
    }
    return -1;
}

/* Open and connect the datagram socket to g_path. Returns 0 or -1 (errno). */
static int sender_connect(void)
{
    struct sockaddr_un addr;

    if (g_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    /* Non-blocking: a wedged reader must cost records, never wall-clock. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC); /* never leak into exec'd children */

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Length was validated in log_open(), so this cannot truncate. */
    memcpy(addr.sun_path, g_path, strlen(g_path) + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    g_fd = fd;
    return 0;
}

int log_open(const char *socket_path)
{
    if (socket_path == NULL || socket_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(socket_path) >= sizeof(g_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    log_close();
    memcpy(g_path, socket_path, strlen(socket_path) + 1);
    log_load_rc(RC_PATH);
    return sender_connect();
}

void log_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

unsigned long log_dropped(void)
{
    return g_dropped;
}

int log_vsend(log_level_t level, const char *fmt, va_list ap)
{
    log_msg_t m;

    if (fmt == NULL) {
        g_dropped++;
        return -1;
    }

    /* Zero the whole struct: the padding between fields travels over the
     * wire, and leaking stack bytes to another process is a disclosure bug. */
    memset(&m, 0, sizeof(m));
    m.pid = getpid();
    m.level = level;
    /* vsnprintf always NUL-terminates and truncates - exactly the contract
     * logmsg.h promises the receiver. A negative return (encoding error)
     * leaves msg as the empty string, which is still a valid record. */
    (void)vsnprintf(m.msg, sizeof(m.msg), fmt, ap);

    if (g_fd < 0 && sender_connect() < 0) {
        g_dropped++;
        return -1;
    }

    for (int attempt = 0; attempt < g_max_attempts; attempt++) {
        if (send(g_fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m))
            return 0;

        /* The daemon was restarted (its socket file is a new inode) or has
         * not bound yet: our connected fd is stale. Re-connect once and
         * retry. Any other error - notably EAGAIN, the receive queue being
         * full - is a plain drop.
         *
         * The exact errno for "peer went away" is not portable: macOS reports
         * ECONNRESET, Linux ECONNREFUSED, and a never-bound path ENOENT, so
         * treat the whole family as recoverable. */
        if (errno != ECONNRESET && errno != ECONNREFUSED && errno != ENOENT &&
            errno != ENOTCONN && errno != EPIPE)
            break;

        log_close();
        if (sender_connect() < 0)
            break;
    }

    g_dropped++;
    return -1;
}

int log_send(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = log_vsend(level, fmt, ap);
    va_end(ap);
    return rc;
}
