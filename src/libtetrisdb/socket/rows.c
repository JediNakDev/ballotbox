/**
 * @file socket/rows.c
 * @brief Reading a select reply.
 *
 * The table itself is Query.execute()'s
 * (db/src/java/simpledb/execution/Query.java:102-123):
 *
 *     id\tsalt\tdigest\titers\t        <- header, one trailing tab
 *     ----------------------------     <- rule, dashes only
 *     7\tb3..\t1f..\t600000            <- one row, fields joined by tabs
 *     (blank line)
 *      1 rows.                         <- note the leading space
 *
 * BUT IT IS NOT THE WHOLE BODY, and assuming it was is the mistake this file
 * is written to avoid. The parser narrates on the way past, so a real reply
 * carries preamble and epilogue that no header documents:
 *
 *     Started a new transaction tid = 3
 *     Added scan of table user
 *     Added select list field user.salt
 *     The query plan is:
 *       π(user.salt),card:0
 *       |
 *     scan(user)
 *     (blank line)
 *     ...the table above...
 *     Transaction 3 committed.
 *
 * So the table is FOUND, not indexed to: the rule line is the anchor, the
 * blank line ends the rows, and the trailer is read from the line after it -
 * never from "the last line", which is the commit notice. The chatter is
 * SimpleDB's and is not a stable contract, which is the second reason to
 * anchor on the shape rather than on any of its text.
 *
 * Values print RAW - unpadded and unquoted - so there is nothing to strip and
 * nothing to unescape, and a TAB inside a stored string would shift every
 * field after it (which is why the username charset excludes TAB and the
 * digest is hex). One consequence has no defence and is accepted: a select of
 * a SINGLE column whose value is the empty string prints an empty line, which
 * is indistinguishable from the blank line that ends the rows. Project more
 * than one column, or a column that cannot be empty.
 *
 * Nothing here allocates, and nothing here is a result-set API. It exists
 * because the layout is a property of the wire, and putting "find the rule,
 * stop at the blank, do not mistake ' 1 rows.' for a row" into the caller
 * would hide wire knowledge where nobody would look for it - and where it
 * could not be tested against a literal string with no JVM running.
 */

#include "libtetrisdb/socket/db.h"

#include <stddef.h>
#include <string.h>

/* Start of the line after p, or NULL if p was the last one. */
static const char *next_line(const char *p) {
  const char *nl = strchr(p, '\n');
  return nl != NULL ? nl + 1 : NULL;
}

/* Length of the line at p, excluding its newline. */
static size_t line_len(const char *p) {
  const char *nl = strchr(p, '\n');
  return nl != NULL ? (size_t)(nl - p) : strlen(p);
}

/* Is this line nothing but dashes? That is the rule under a header, and it is
 * the one line in the output whose shape is unmistakable. */
static int is_rule(const char *p) {
  size_t n = line_len(p);

  if (n == 0)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (p[i] != '-')
      return 0;
  return 1;
}

/** The first data row of a select reply, or NULL if body carries no table.
 * Found by scanning for the rule line rather than by counting lines from the
 * top, because the parser's narration comes first and is not a contract. The
 * header itself is not inspected: column names are the caller's business and a
 * projection can name anything.
 * The first rule line wins. A row of dashes further down is data, and data
 * cannot appear before the rule that introduces it. */
static const char *rows_begin(const char *body) {
  if (body == NULL)
    return NULL;

  /* From the second line on: a rule needs a header above it, and a body that
   * opens with dashes is not a table. */
  for (const char *p = next_line(body); p != NULL && *p != '\0';
       p = next_line(p))
    if (is_rule(p))
      return next_line(p);

  return NULL;
}

/** How many rows the reply says it has, or -1 if it does not say.
 * rows is where the row block starts. The block ends at the first blank line
 * and the trailer is the next line, which must read " N rows." - the commit
 * notice after it is exactly why this is not "the last line of the body".
 * The trailer is what proves the reply is COMPLETE. A body truncated by a
 * caller's undersized buffer ends mid-table with no trailer, and reporting the
 * rows that happened to fit would turn a truncated read into a confident wrong
 * answer - "no such user" for a user who exists. */
static int block_count(const char *rows) {
  int n = 0;

  for (const char *p = rows; p != NULL && *p != '\0'; p = next_line(p), n++) {
    if (line_len(p) > 0)
      continue;

    /* The blank line: the trailer is next. */
    const char *t = next_line(p);
    if (t == NULL)
      return -1;
    while (*t == ' ')
      t++;

    int said = 0, digits = 0;
    while (*t >= '0' && *t <= '9') {
      said = said * 10 + (*t++ - '0');
      digits++;
    }
    if (digits == 0 || strncmp(t, " rows.", 6) != 0)
      return -1;

    /* The runner's count and the lines it printed disagreeing means this is
     * not the output it looks like, and guessing which one is right is how a
     * salt gets read as a digest. */
    return said == n ? n : -1;
  }
  return -1; /* no blank line: the body was cut off mid-table */
}

int tdb_row_count(const char *body) {
  const char *rows = rows_begin(body);

  return rows != NULL ? block_count(rows) : -1;
}

int tdb_row_fields(const char *body, int row, const char *f[], size_t len[],
                   int max) {
  int count = tdb_row_count(body);
  if (count < 0 || row < 0 || row >= count)
    return -1;

  const char *p = rows_begin(body);
  for (int i = 0; i < row; i++) {
    p = next_line(p);
    if (p == NULL)
      return -1; /* the trailer promised more rows than the body carries */
  }

  size_t left = line_len(p);
  int n = 0;

  /* Fields are what tabs separate, including empty ones: a stored empty
   * string is a real value and dropping it would shift every field after it,
   * which is the failure this whole file is arranged to avoid. */
  for (;;) {
    const char *tab = memchr(p, '\t', left);
    size_t flen = tab != NULL ? (size_t)(tab - p) : left;

    if (n < max) {
      f[n] = p;
      len[n] = flen;
    }
    n++;

    if (tab == NULL)
      break;
    left -= flen + 1;
    p = tab + 1;
  }
  return n;
}
