#ifndef LIBCOMMON_RC_H
#define LIBCOMMON_RC_H

/*
 * rc.h - minimal key=value directive reader, shared by every tetriSH
 * component that reads .tetrishrc for its own configuration.
 *
 * This is deliberately not the shell's line classifier
 * (tetrish/lib/rc_parser.h): that one turns each line into a shell command
 * to run, with PATH= as its one special case. This one turns "key = value"
 * lines into calls to a callback and skips everything else - comments,
 * blank lines, PATH=, bare shell commands - so several independent readers
 * (tetrislogd's own options, libcommon's sender tuning, ...) can share one
 * rc file without stepping on each other's directives or on the shell's.
 */

/* The rc file every tetriSH component reads by default, relative to its
 * working directory. One name, defined once, so "which file did this value
 * come from" always has the same answer across the whole codebase. */
#define RC_PATH ".tetrishrc"

/* Called once per recognised "key=value" line. key and value are trimmed of
 * surrounding whitespace and NUL-terminated. Both point into a buffer that
 * is only valid for the duration of the call - copy anything you need to
 * keep. */
typedef void (*rc_directive_fn)(const char *key, const char *value, void *ctx);

/*
 * Read path line by line, calling fn for every key=value directive found.
 *
 * A line is skipped, not an error, if it is blank, starts with '#' (after
 * leading whitespace), has no '=', or has an empty key. A missing file is
 * not an error either - fn is simply never called.
 */
void rc_load(const char *path, rc_directive_fn fn, void *ctx);

#endif /* LIBCOMMON_RC_H */
