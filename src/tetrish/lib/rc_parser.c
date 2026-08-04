/*
 * rc_parser.c
 *
 * Classifies one line of .tetrishrc for the shell: a PATH directive, a command
 * to run, or something the shell should ignore. The expected behaviour is
 * documented in rc_parser.h.
 */

#include "tetrish/lib/rc_parser.h"

#include <ctype.h>
#include <string.h>

/* Advance past leading whitespace, mirroring libcommon/rc.c's lstrip(). */
static const char *skip_ws(const char *p) {
  while (*p != '\0' && isspace((unsigned char)*p))
    p++;
  return p;
}

/*
 * Is this a "key = value" directive belonging to another reader?
 *
 * .tetrishrc is shared: libcommon's rc_load() and tetrislogd's config loader
 * both read their own keys out of the same file (see libcommon/rc.h). Without
 * this check the shell would try to execvp() every one of them, so a file
 * configuring the log daemon greets the user with "Command log_path not
 * found" once per directive.
 *
 * The test is deliberately narrow: the text before the first '=' must be a
 * single bare token. That distinguishes a directive ("log_path = x") from a
 * command that happens to contain '=' ("setenv FOO=bar"), whose pre-'=' text
 * spans a space.
 *
 * A line with no key at all ("=value") counts as a directive here. It is not
 * one, but rc_load() discards it rather than dispatching it, so it belongs to
 * no reader; calling it a command would leave the shell as the only component
 * that reacts to it, with "Command =value not found".
 */
static int is_directive(const char *p) {
  const char *eq = strchr(p, '=');
  if (eq == NULL)
    return 0;

  for (const char *q = p; q < eq; q++) {
    if (isspace((unsigned char)*q)) {
      /* Trailing space before '=' is still a directive ("key = v"); an
       * embedded one means a multi-word command. */
      return skip_ws(q) == eq;
    }
  }
  return 1;
}

rc_line_type_t classify_rc_line(const char *line, const char **value) {
  *value = NULL;
  if (line == NULL)
    return RC_LINE_EMPTY;

  const char *p = skip_ws(line);

  if (*p == '\0')
    return RC_LINE_EMPTY;

  /* A PATH directive is "PATH" followed by '=', with optional whitespace on
   * either side of the '='. A line like "PATHETIC" (PATH followed by neither
   * whitespace nor '=') is a command.
   *
   * The spacing tolerance matters because every other directive in the file
   * is written "key = value" and is read by a trimming parser
   * (libcommon/rc_load). Requiring PATH alone to be tight makes the one line
   * that breaks everything the one line that looks like all the others. */
  if (strncmp(p, "PATH", 4) == 0) {
    const char *q = skip_ws(p + 4);
    if (*q == '=') {
      *value = skip_ws(q + 1); /* substring after "PATH=" (may be empty) */
      return RC_LINE_PATH;
    }
  }

  /* Comments and other readers' directives are not ours to run. */
  if (*p == '#' || is_directive(p))
    return RC_LINE_EMPTY;

  *value = p;
  return RC_LINE_COMMAND;
}
