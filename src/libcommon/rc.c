/*
 * rc.c - the .tetrishrc grammar. See libcommon/rc.h for what it is.
 *
 * is_directive() is the single point where "is this line a directive?" is
 * decided. Both rc_load() and rc_classify_line() ask it, so the shell and the
 * daemons cannot drift into disagreeing about the same line.
 */

#include "libcommon/rc.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Skip leading whitespace, returning a pointer into s. */
static char *lstrip(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Trim trailing whitespace from s in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/*
 * Is this line a "key = value" directive? p must already be lstripped.
 *
 * The test is deliberately narrow: the text before the first '=' must be a
 * single bare token. That is what separates a directive ("log_path = x") from
 * a command that happens to contain '=' ("setenv FOO=bar"), whose pre-'='
 * text spans a space. An empty key ("=value") still counts as a directive
 * here - it is nobody's, and callers drop it - so that the shell and rc_load
 * agree on it rather than one running what the other discarded.
 */
static int is_directive(const char *p)
{
    const char *eq = strchr(p, '=');
    if (eq == NULL)
        return 0;

    for (const char *q = p; q < eq; q++) {
        if (isspace((unsigned char)*q)) {
            /* Trailing space before '=' is still a directive ("key = v"); an
               embedded one means a multi-word command. */
            while (q < eq && isspace((unsigned char)*q))
                q++;
            return q == eq;
        }
    }
    return 1;
}

void rc_load(const char *path, rc_directive_fn fn, void *ctx)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return; /* no rc file: fn is simply never called */

    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        char *key = lstrip(line);
        if (*key == '\0' || *key == '#')
            continue; /* blank line, or a comment */

        if (!is_directive(key))
            continue; /* not ours - e.g. a shell command, or PATH-less text */

        char *eq = strchr(key, '=');
        *eq = '\0';
        rstrip(key);
        if (*key == '\0')
            continue; /* "=value" with no key */

        char *value = lstrip(eq + 1);
        rstrip(value);

        fn(key, value, ctx);
    }

    fclose(f);
}

rc_line_type_t rc_classify_line(const char *line, const char **value)
{
    *value = NULL;
    if (line == NULL)
        return RC_LINE_EMPTY;

    const char *p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;

    if (*p == '\0')
        return RC_LINE_EMPTY;

    if (strncmp(p, "PATH", 4) == 0) {
        const char *q = p + 4;
        while (isspace((unsigned char)*q))
            q++;
        if (*q == '=') {
            q++;
            while (isspace((unsigned char)*q))
                q++;
            *value = q; /* substring after "PATH=" (may be empty) */
            return RC_LINE_PATH;
        }
    }

    /* Comments and the loaders' directives are not ours to run. */
    if (*p == '#' || is_directive(p))
        return RC_LINE_EMPTY;

    *value = p;
    return RC_LINE_COMMAND;
}
