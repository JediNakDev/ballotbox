#ifndef LIBCOMMON_LOGMSG_H
#define LIBCOMMON_LOGMSG_H

/**
 * @file logmsg.h
 * @brief The tetriSH logging IPC contract.
 *
 * Every tetriSH process (tetrisd, its per-session workers, tetrisctl, the
 * shell's system programs) reports what it is doing to one place: tetrislogd.
 * The transport is an AF_UNIX SOCK_DGRAM socket, chosen because it is
 * connectionless (a sender never blocks on a handshake and needs no reconnect
 * logic), message-framed (one datagram = one record, so interleaved writers
 * can never tear each other's lines) and local-only (no port, no network
 * exposure, filesystem permissions are the access control).
 *
 * The wire format is this fixed-size struct, sent raw. Fixed size means the
 * receiver validates a datagram with a single length comparison - a short or
 * oversized datagram is malformed by definition and is dropped. Both sides
 * are compiled from this header, so layout agreement is a build-time property.
 *
 * Two rules the daemon relies on, and every sender must respect:
 *   1. msg is a NUL-terminated C string within its 256 bytes.
 *   2. pid is the sender's own getpid().
 * Neither is enforceable across a datagram boundary, so tetrislogd treats
 * both as untrusted input: it re-terminates msg, sanitises control
 * characters, and prints pid as a claim rather than a fact.
 */

#include <stdarg.h>
#include <sys/types.h>

/* Capacity of log_msg_t.msg, including the terminating NUL. */
#define LOG_MSG_MAX 256

/* Severity of one record. The numeric values are part of the wire format:
 * they must never be reordered or renumbered. Note the declaration order is
 * not severity order - use log_level_rank() to compare severities. */
typedef enum {
    LOG_INFO  = 0,
    LOG_WARN  = 1,
    LOG_ERROR = 2,
    LOG_DEBUG = 3
} log_level_t;

/* One log record, sent as a single datagram exactly sizeof(log_msg_t) long. */
typedef struct {
    pid_t       pid;    /* sender's getpid(), advisory (see header comment) */
    log_level_t level;
    char        msg[LOG_MSG_MAX];
} log_msg_t;

/* === Level helpers (no I/O, safe anywhere) === */

/* Fixed-width name for use in log columns: "INFO ", "WARN ", "ERROR",
 * "DEBUG", or "?????" for a level outside the enum. Never returns NULL. */
const char *log_level_str(log_level_t level);

/* Severity rank for filtering: DEBUG < INFO < WARN < ERROR. An out-of-range
 * level ranks highest, so a corrupt record is never silently filtered out. */
int log_level_rank(log_level_t level);

/* Parse a level name, case-insensitive ("info", "WARN", "error", "debug").
 * Returns 0 and stores the level on success, -1 if name is not a level. */
int log_level_parse(const char *name, log_level_t *out);

/* === Sender side === */

/*
 * Bind this process to the logging socket at socket_path. Idempotent: calling
 * it again re-points the sender at a new path. The socket is non-blocking, so
 * a slow or wedged tetrislogd can never stall the caller - excess records are
 * dropped and counted instead (see log_dropped()).
 *
 * Also reads the log_send_attempts directive from RC_PATH (libcommon/rc.h) in
 * the current working directory, if present - see log_load_rc(). A process
 * that wants its rc file read from somewhere else should call log_load_rc()
 * itself afterwards; log_open() only ever consults the default location.
 *
 * Returns 0 on success, -1 on failure (errno set); on failure log_send() is
 * still safe to call, it just drops everything.
 */
int log_open(const char *socket_path);

/*
 * Read the log_send_attempts directive from the rc file at rc_path and use it
 * for every future log_send()/log_vsend() call in this process: the total
 * number of attempts (including the first) before a record is counted as
 * dropped - see the retry comment in log_vsend(). Must be a positive integer;
 * an invalid value is reported to stderr and the current setting is kept.
 *
 * A missing file, or a file without the directive, leaves the current value
 * (2, until changed) in place. log_open() already calls this against the
 * default rc path, so most callers never need to call it directly.
 */
void log_load_rc(const char *rc_path);

/*
 * Send one printf-formatted record at the given level. The text is truncated
 * to LOG_MSG_MAX-1 characters. Never blocks; a full receive queue or a
 * restarted daemon costs a dropped record, not a stalled caller (one
 * transparent reconnect is attempted when the daemon has been restarted).
 *
 * Returns 0 if the datagram was handed to the kernel, -1 if it was dropped.
 * Callers are expected to ignore the return value: logging is best-effort by
 * design, and a logging failure must never change program flow.
 */
int log_send(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* va_list form of log_send(), for callers with their own varargs wrapper. */
int log_vsend(log_level_t level, const char *fmt, va_list ap);

/* Number of records this process dropped since log_open(). Worth reporting at
 * shutdown: a non-zero count means the log is incomplete. */
unsigned long log_dropped(void);

/* Release the socket. Safe to call when never opened. */
void log_close(void);

#endif /* LIBCOMMON_LOGMSG_H */
