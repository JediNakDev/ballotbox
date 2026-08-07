#ifndef LIBTETRISDB_SOCKET_CONF_H
#define LIBTETRISDB_SOCKET_CONF_H

/**
 * @file socket/conf.h
 * @brief The settled values of the db_ .tetrishrc namespace.
 *
 * Included by socket/runner.c, socket/socket.c and libtetrisauth's authconf.c.
 * ADR 0002 gives bin/tetrisdb the db_ namespace, but the login path is the
 * actual consumer of two of its keys, so the defaults and bounds were written
 * three times across two libraries. They are here once, so a disagreement
 * between the launcher and the login path is something you would have to write
 * on purpose.
 */

#include <stddef.h>
#include <sys/un.h>

/** db_ipc default: SocketRunner's unix socket. Relative, resolved by the
 * caller against the project root, because only the caller knows its root. */
#define TDB_DEFAULT_IPC "var/run/tetrisdb.sock"

/** Longest db_ipc value: a unix socket path must fit sun_path, which is far
 * shorter than PATH_MAX everywhere this builds. */
#define TDB_IPC_MAX (sizeof(((struct sockaddr_un *)0)->sun_path) - 1)

#define TDB_DEFAULT_TIMEOUT_MS 2000 /**< db_timeout default, from #52. */
/** db_timeout floor; below this a typo fails every login before the runner
 * can answer. */
#define TDB_TIMEOUT_MIN_MS 100
/** db_timeout ceiling; above this a typo parks a session process for a
 * minute. */
#define TDB_TIMEOUT_MAX_MS 60000

#define TDB_DEFAULT_DIR "var/db"                /**< db_dir default. */
#define TDB_DEFAULT_JAR "db/dist/simpledb.jar"  /**< db_jar default. */
#define TDB_DEFAULT_JAVA "java"                 /**< db_java default. */

/** db_sessions default: connections the runner serves concurrently. 16 rather
 * than SocketRunner's own 8, because a session holds a connection only for one
 * login and #51 measured 16 comfortably inside the NO STEAL BufferPool. Raise
 * --pages with it if transactions ever get larger. */
#define TDB_DEFAULT_SESSIONS 16

#endif /* LIBTETRISDB_SOCKET_CONF_H */
