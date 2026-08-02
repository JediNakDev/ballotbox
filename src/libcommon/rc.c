/*
 * rc.c - line-oriented key=value reader for .tetrishrc.
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

        char *eq = strchr(key, '=');
        if (eq == NULL)
            continue; /* not a key=value line - e.g. a shell command */

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
