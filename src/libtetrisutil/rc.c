/**
 * @file rc.c
 * @brief Line-oriented key=value reader for .tetrishrc.
 */

#include "libtetrisutil/rc.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

int rc_load(const char *path, rc_directive_fn fn, void *ctx)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1; /* no rc file, which is not the same as no directives */

    int applied = 0;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        char *key = lstrip(line);
        if (*key == '\0' || *key == '#')
            continue; /* blank line, or a comment */

        if (!is_directive(key))
            continue; /* not ours - e.g. a shell command containing '=' */

        char *eq = strchr(key, '=');
        *eq = '\0';
        rstrip(key);
        if (*key == '\0')
            continue; /* "=value" with no key */

        char *value = lstrip(eq + 1);
        rstrip(value);

        fn(key, value, ctx);
        applied++;
    }

    fclose(f);
    return applied;
}

/* ====================================================================== *
 * Typed keys - the rc_bind() layer over rc_load()                         *
 * ====================================================================== */

/* What the directive callback is filling in. The callback cannot return a
 * status, so the ctx carries one: defect is the whole failure state, and an
 * empty key means every key of ours parsed. */
typedef struct {
    const rc_key_t *keys;
    size_t          nkeys;
    void           *dst;
    const char     *owned_prefix;
    rc_defect_t    *defect;
    int             failure; /* RC_E_VALUE, RC_E_UNKNOWN, or 0 */
} rc_bind_ctx_t;

static int rc_parse_int(const char *value, long lo, long hi, int *out)
{
    char *end;
    long  v = strtol(value, &end, 10);

    if (end == value || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static void note_defect(rc_bind_ctx_t *c, int failure, const char *key,
                        const char *value)
{
    if (c->failure != 0)
        return;
    c->failure = failure;
    if (c->defect != NULL) {
        snprintf(c->defect->key, sizeof c->defect->key, "%s", key);
        snprintf(c->defect->value, sizeof c->defect->value, "%s", value);
    }
}

/* on/yes/true/1 and off/no/false/0, case-insensitively. */
static int rc_parse_bool(const char *value, int *out)
{
    static const char *const yes[] = {"on", "yes", "true", "1"};
    static const char *const no[]  = {"off", "no", "false", "0"};

    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
        if (strcasecmp(value, yes[i]) == 0) {
            *out = 1;
            return 0;
        }
        if (strcasecmp(value, no[i]) == 0) {
            *out = 0;
            return 0;
        }
    }
    return -1;
}

/*
 * One directive against one table entry. Returns 0 if the value was accepted.
 *
 * A check_only key parses into a scratch slot and is thrown away. The range is
 * still enforced, which is the whole point: the namespace owner validates a
 * key somebody else consumes.
 */
static int bind_one(const rc_key_t *k, const char *value, void *dst)
{
    union {
        int    i;
        size_t z;
        char   s[256];
    } scratch;
    char *field = k->check_only ? scratch.s : (char *)dst + k->off;

    switch (k->type) {
    case RC_INT:
        return rc_parse_int(value, k->lo, k->hi, (int *)(void *)field);
    case RC_SIZE: {
        int n;
        if (rc_parse_int(value, k->lo, k->hi, &n) != 0)
            return -1;
        *(size_t *)(void *)field = (size_t)n;
        return 0;
    }
    case RC_BOOL:
        return rc_parse_bool(value, (int *)(void *)field);
    case RC_CUSTOM:
        return k->parse == NULL ? -1 : k->parse(value, field);
    case RC_STR:
        break;
    }

    size_t max = k->max_len != 0 ? k->max_len : k->cap - 1;
    size_t cap = k->check_only ? sizeof scratch.s : k->cap;
    size_t len = strlen(value);
    if (len == 0 || len > max || len >= cap)
        return -1;
    memcpy(field, value, len + 1);
    return 0;
}

static void bind_directive(const char *key, const char *value, void *ctx)
{
    rc_bind_ctx_t *c = ctx;

    for (size_t i = 0; i < c->nkeys; i++) {
        if (strcmp(key, c->keys[i].key) != 0)
            continue;
        if (bind_one(&c->keys[i], value, c->dst) != 0)
            note_defect(c, RC_E_VALUE, key, value);
        return;
    }

    /* Not ours. Only the operator-facing readers, which own a whole namespace,
     * are entitled to call that an error. */
    if (c->owned_prefix != NULL &&
        strncmp(key, c->owned_prefix, strlen(c->owned_prefix)) == 0)
        note_defect(c, RC_E_UNKNOWN, key, value);
}

int rc_bind(const char *path, const rc_key_t *keys, size_t nkeys, void *dst,
            const char *owned_prefix, rc_defect_t *defect)
{
    rc_bind_ctx_t c;

    memset(&c, 0, sizeof c);
    c.keys = keys;
    c.nkeys = nkeys;
    c.dst = dst;
    c.owned_prefix = owned_prefix;
    c.defect = defect;
    if (defect != NULL)
        memset(defect, 0, sizeof *defect);

    int applied = rc_load(path, bind_directive, &c);
    if (applied < 0)
        return RC_E_OPEN;
    return c.failure != 0 ? c.failure : applied;
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
