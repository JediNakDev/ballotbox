#ifndef LIBCOMMON_RC_H
#define LIBCOMMON_RC_H

/*
 * rc.h - the .tetrishrc grammar, and the two questions asked of it.
 *
 * One file is read by several components, which want different things from
 * it. The shell wants to know which lines are commands it should run; the
 * daemons want their own "key = value" directives and nothing else. Both
 * questions are answered here, against one definition of what a directive is,
 * because the two used to be answered in separate modules that disagreed: a
 * keyless "=value" was a directive to one and a command to the other, so the
 * shell tried to run a line the loader had silently dropped.
 *
 *   rc_classify_line() answers the shell's question, one line at a time.
 *   rc_load() answers the daemons' question, over a whole file.
 *
 * The grammar:
 *
 *   - Leading whitespace is insignificant.
 *   - A line whose first non-blank character is '#' is a comment.
 *   - A line is a directive if it contains '=' and the text before the first
 *     '=' is a single bare token. "log_path = x" is a directive;
 *     "setenv FOO=bar" is not, because its pre-'=' text spans a space.
 *   - A directive with an empty key ("=value") belongs to nobody: rc_load
 *     does not dispatch it and the shell does not run it.
 *   - Anything else is a command, which only the shell cares about.
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

typedef enum {
    RC_LINE_EMPTY,   /* blank, a comment, or a directive owned by a loader */
    RC_LINE_PATH,    /* "PATH" then '=', with optional spaces around it */
    RC_LINE_COMMAND  /* anything else, after trimming leading whitespace */
} rc_line_type_t;

/*
 * Classify one line for the shell.
 *
 *   On RC_LINE_PATH:    *value points to the substring after "PATH=".
 *   On RC_LINE_COMMAND: *value points to the trimmed command text.
 *   On RC_LINE_EMPTY:   *value is set to NULL.
 *
 * The returned pointer is into the input buffer. Do not free it, and do not
 * modify the contents of the input string.
 *
 * PATH tolerates whitespace around its '=' because every other directive in
 * the file is written "key = value" and read by a trimming parser. Requiring
 * PATH alone to be tight would make the one line that breaks everything the
 * one line that looks like all the others. A line starting with "PATH" that
 * is followed by neither whitespace nor '=' ("PATHETIC") is a command.
 *
 * Directives are RC_LINE_EMPTY rather than commands: they belong to rc_load's
 * callers, and a shell that ran them would answer a file configuring the log
 * daemon with "Command log_path not found" once per line.
 */
rc_line_type_t rc_classify_line(const char *line, const char **value);

#endif /* LIBCOMMON_RC_H */
