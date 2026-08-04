#ifndef RC_PARSER_H
#define RC_PARSER_H

/*
 * rc_parser.h
 *
 * Pure helper for classifying lines from .tetrishrc, kept separate from the
 * shell's reader so it can be exercised without spawning the shell.
 * classify_rc_line takes one line of text and tells you whether the shell
 * should ignore it, treat it as a PATH directive, or run it as a command.
 */

typedef enum {
    RC_LINE_EMPTY,    /* blank, whitespace-only, a '#' comment, or a
                         "key = value" directive owned by another reader */
    RC_LINE_PATH,     /* "PATH" then '=', with optional spaces around it */
    RC_LINE_COMMAND   /* anything else, after trimming leading whitespace */
} rc_line_type_t;

/*
 * Classify one line from .tetrishrc.
 *
 *   On RC_LINE_PATH:    *value points to the substring after "PATH=".
 *   On RC_LINE_COMMAND: *value points to the trimmed command text.
 *   On RC_LINE_EMPTY:   *value is set to NULL.
 *
 * The returned pointer is into the input buffer. Do not free it. Do not
 * modify the contents of the input string.
 *
 * A line that starts with "PATH" but is followed by neither whitespace nor
 * '=' (for example "PATHETIC") is RC_LINE_COMMAND, not RC_LINE_PATH.
 *
 * .tetrishrc is shared with libcommon's rc_load() and tetrislogd's config
 * loader, so a "key = value" line whose key is a single bare token is
 * classified RC_LINE_EMPTY: it belongs to another reader and the shell must
 * not try to run it. A keyless "=value" is RC_LINE_EMPTY for the same reason:
 * rc_load() drops it, so nothing should react to it. A command containing '='
 * is unaffected, because its pre-'=' text spans a space ("setenv FOO=bar"
 * stays RC_LINE_COMMAND).
 */
rc_line_type_t classify_rc_line(const char *line, const char **value);

#endif
