/*
 * schema.c - creating a table, and getting text safely into one.
 *
 * SimpleDB's parser has no DDL at all, so "CREATE TABLE IF NOT EXISTS" is
 * spelled here as two filesystem facts: a line in catalog.txt describing the
 * columns, and a <name>.dat heap file beside it. Both must exist before
 * PipeRunner starts, because it reads the catalog once and never rereads it.
 */

#include "libtetrisdb/tetrisdb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TDB_DEFAULT_JAR "db/dist/simpledb.jar"
#define TDB_DEFAULT_JAVA "java"
#define TDB_DEFAULT_QCAP 256

void tdb_opts_default(tdb_opts_t *opts) {
  /* dir has no default on purpose. Two daemons accepting the same built-in
     directory would land in one catalog and, sooner or later, one .dat -
     exactly the corruption this library exists to prevent. Naming the
     directory is therefore the caller's decision, and an unset one is an
     error rather than a guess. */
  opts->dir[0] = '\0';
  snprintf(opts->jar, sizeof(opts->jar), "%s", TDB_DEFAULT_JAR);
  snprintf(opts->java, sizeof(opts->java), "%s", TDB_DEFAULT_JAVA);
  opts->queue_cap = TDB_DEFAULT_QCAP;
}

/*
 * Does catalog already describe a table called name?
 *
 * Catalog lines look like "name (col type, ...)", so a table is present when
 * some line's text before its '(' is exactly name. Comparing that way (rather
 * than a substring search) keeps a table called "log" from being found inside
 * a line for "logmeta".
 */
static int catalog_has(const char *catalog, const char *name) {
  FILE *f = fopen(catalog, "r");
  if (f == NULL)
    return 0; /* no catalog yet: nothing is defined */

  char line[1024];
  size_t want = strlen(name);
  int found = 0;

  while (!found && fgets(line, sizeof(line), f) != NULL) {
    char *paren = strchr(line, '(');
    if (paren == NULL)
      continue;

    char *start = line;
    while (*start == ' ' || *start == '\t')
      start++;
    char *end = paren;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
      end--;

    found = (size_t)(end - start) == want && strncmp(start, name, want) == 0;
  }

  fclose(f);
  return found;
}

/* Create every missing component of a directory path, like mkdir -p. Returns
 * 0 if the directory exists on return. */
static int mkdir_p(const char *path) {
  char buf[PATH_MAX];
  struct stat st;

  if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
    fprintf(stderr, "tetrisdb: path too long: %s\n", path);
    return -1;
  }

  for (char *p = buf + 1; *p != '\0'; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(buf, 0755) < 0 && errno != EEXIST)
      goto fail;
    *p = '/';
  }
  if (mkdir(buf, 0755) < 0 && errno != EEXIST)
    goto fail;

  if (stat(buf, &st) < 0 || !S_ISDIR(st.st_mode)) {
    fprintf(stderr, "tetrisdb: not a directory: %s\n", buf);
    return -1;
  }
  return 0;

fail:
  fprintf(stderr, "tetrisdb: mkdir %s: %s\n", buf, strerror(errno));
  return -1;
}

/*
 * Give a freshly created heap file one empty page.
 *
 * A zero-byte file is a heap file of zero pages, which SimpleDB will happily
 * insert into (it allocates page 0) but cannot read: a scan of it aborts the
 * transaction with "Failed to read page 0". Since a new table should be
 * queryable before anything is written to it, lay down one page up front.
 * All-zero bytes is exactly what HeapPage.createEmptyPageData() produces: an
 * empty slot header, so the page reads as zero tuples.
 *
 * TDB_PAGE_SIZE must match SimpleDB's BufferPool.PAGE_SIZE (4096, and nothing
 * in this project calls setPageSize).
 */
#define TDB_PAGE_SIZE 4096

static int write_empty_page(int fd, const char *path) {
  static const char zeros[TDB_PAGE_SIZE];
  size_t left = sizeof(zeros);
  const char *p = zeros;

  while (left > 0) {
    ssize_t w = write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "tetrisdb: write %s: %s\n", path, strerror(errno));
      return -1;
    }
    p += (size_t)w;
    left -= (size_t)w;
  }
  return 0;
}

int tdb_ensure_table(const char *dir, const char *name, const char *schema) {
  char catalog[PATH_MAX + 16];
  char dat[PATH_MAX + 16];

  if (dir == NULL || dir[0] == '\0') {
    fprintf(stderr, "tetrisdb: no data directory set for table '%s'"
                    " (each daemon must own one)\n",
            name);
    return -1;
  }

  if (mkdir_p(dir) < 0)
    return -1;

  snprintf(catalog, sizeof(catalog), "%s/catalog.txt", dir);
  snprintf(dat, sizeof(dat), "%s/%s.dat", dir, name);

  /* The heap file first: a catalog line naming a .dat that does not exist
   * makes PipeRunner fail at startup, so never leave that window open.
   * O_CREAT|O_EXCL means an existing table's data is never truncated. */
  int fd = open(dat, O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (fd >= 0) {
    int rc = write_empty_page(fd, dat);
    close(fd);
    if (rc < 0) {
      unlink(dat); /* half a page is worse than no table at all */
      return -1;
    }
  } else if (errno != EEXIST) {
    fprintf(stderr, "tetrisdb: create %s: %s\n", dat, strerror(errno));
    return -1;
  }

  if (catalog_has(catalog, name))
    return 0; /* already declared - leave the existing schema alone */

  FILE *f = fopen(catalog, "a");
  if (f == NULL) {
    fprintf(stderr, "tetrisdb: open %s: %s\n", catalog, strerror(errno));
    return -1;
  }
  if (fprintf(f, "%s (%s)\n", name, schema) < 0 || fclose(f) != 0) {
    fprintf(stderr, "tetrisdb: write %s: %s\n", catalog, strerror(errno));
    return -1;
  }
  return 0;
}

char *tdb_quote(char *dst, size_t cap, const char *src) {
  size_t w = 0;

  if (cap == 0)
    return dst;

  /* Reserve room for the closing quote and NUL up front, so truncation can
   * never leave an unterminated literal - which would swallow the rest of
   * the statement rather than merely losing characters. */
  if (cap < 3) {
    snprintf(dst, cap, "%s", "");
    return dst;
  }

  dst[w++] = '\'';
  for (const char *s = src; *s != '\0'; s++) {
    /* A quote inside the text is doubled: SimpleDB's parser reads '' as
     * one literal quote and keeps scanning, so text can never terminate
     * the literal early and inject SQL of its own. */
    size_t need = (*s == '\'') ? 2 : 1;
    if (w + need + 2 > cap)
      break; /* truncate rather than emit half an escape */
    if (*s == '\'')
      dst[w++] = '\'';
    dst[w++] = *s;
  }
  dst[w++] = '\'';
  dst[w] = '\0';
  return dst;
}
