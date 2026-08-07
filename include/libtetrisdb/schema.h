#ifndef LIBTETRISDB_SCHEMA_H
#define LIBTETRISDB_SCHEMA_H

/**
 * @file schema.h
 * @brief Making a table exist, and getting text safely into one.
 *
 * Neither is about a transport, which is why they have a header of their own:
 * creating a table is a filesystem operation done before any runner starts,
 * and quoting is a property of SimpleDB's parser. Putting them in either
 * transport's header would make the login path include "pipe" to quote a
 * username.
 */

#include <stddef.h>

/**
 * Writes dir's catalog path into dst, truncating to cap.
 *
 * Exported because it had three copies: both runners take the catalog as an
 * argv entry, and tdb_ensure_table() writes to it.
 *
 * @param dst  Receives the path.
 * @param cap  Capacity of dst.
 * @param dir  Data directory; NULL is treated as "".
 */
void tdb_catalog_path(char *dst, size_t cap, const char *dir);

/**
 * Creates path and every missing parent, like `mkdir -p`.
 *
 * Exported because bin/tetrisdb had its own copy: both this library and the
 * launcher have to make var/db and var/run before anything can open them.
 *
 * @param path  Directory to create; an existing one is not an error.
 * @returns 0 if the directory exists on return, -1 otherwise (message on
 *          stderr).
 */
int tdb_mkdir_p(const char *path);

/**
 * Ensures a table exists, creating it if it does not.
 *
 * Must be called BEFORE THE RUNNER STARTS: a runner reads the catalog once at
 * startup and never again. SimpleDB's parser has no CREATE TABLE, so a table
 * is a line in catalog.txt plus a <name>.dat heap file beside it. An existing
 * line is left completely alone - nothing compares or migrates the schema, so
 * changing a table's columns is manual (edit catalog.txt, delete the .dat).
 *
 * A new .dat gets one empty page rather than zero bytes, because a zero-byte
 * heap file can be inserted into but not scanned.
 *
 * @param dir     Data directory.
 * @param name    Table name.
 * @param schema  Column list as it appears inside the catalog's parentheses,
 *                e.g. "id int, pid int, ts int, status string, msg string".
 * @returns 0 if the table exists on return, -1 on failure (message on stderr).
 */
int tdb_ensure_table(const char *dir, const char *name, const char *schema);

/**
 * Quotes a string for use as a SQL literal.
 *
 * Keeps sender-supplied text from breaking out of its literal and appending
 * statements of its own. The doubling survives into storage: SimpleDB's parser
 * accepts '' as an escape but does not collapse it on read, so a stored
 * message shows both.
 *
 * @param dst  Receives src wrapped in single quotes, inner quotes doubled,
 *             truncated to fit.
 * @param cap  Capacity of dst, terminating NUL included.
 * @param src  Text to quote.
 * @returns dst.
 */
char *tdb_quote(char *dst, size_t cap, const char *src);

#endif /* LIBTETRISDB_SCHEMA_H */
