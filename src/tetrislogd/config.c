/**
 * @file config.c
 * @brief The complete log_ namespace owned by tetrislogd.
 */

#include "tetrislogd/tetrislogd.h"

#include "libtetrisutil/rc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEFAULT_SOCKET_PATH "var/run/tetrislogd.sock"
#define DEFAULT_LOG_PATH "var/log/tetrisd.log"
#define DEFAULT_DB_DIR "var/db_log"

const char *const logd_rc_keys[] = {
    "log_path", "log_ipc", "log_level", "log_send_attempts", "log_db",
    "log_db_dir", "log_db_jar", "log_db_java", "log_db_queue", NULL,
};

/* log_level's spelling is tetrislogd's own, so it comes through RC_CUSTOM
 * rather than being a type libtetrisutil has to know about. */
static int parse_level(const char *value, void *field) {
  return log_level_parse(value, (log_level_t *)field);
}

/*
 * The complete log_ namespace, as a table.
 *
 * log_send_attempts is check_only: tetrislogd owns the namespace and refuses a
 * bad value, but the key is consumed by libtetrisutil's sender, not by the daemon.
 */
static const rc_key_t LOG_KEYS[] = {
    {.key = "log_path",
     .type = RC_STR,
     .off = offsetof(logd_opts_t, log_path),
     .cap = sizeof(((logd_opts_t *)0)->log_path)},
    {.key = "log_ipc",
     .type = RC_STR,
     .off = offsetof(logd_opts_t, socket_path),
     .cap = sizeof(((logd_opts_t *)0)->socket_path),
     .max_len = sizeof(((struct sockaddr_un *)0)->sun_path) - 1},
    {.key = "log_level",
     .type = RC_CUSTOM,
     .off = offsetof(logd_opts_t, min_level),
     .parse = parse_level},
    {.key = "log_send_attempts",
     .type = RC_INT,
     .lo = 1,
     .hi = 1000,
     .check_only = true},
    {.key = "log_db", .type = RC_BOOL, .off = offsetof(logd_opts_t, db_enable)},
    {.key = "log_db_dir",
     .type = RC_STR,
     .off = offsetof(logd_opts_t, db) + offsetof(tdb_opts_t, dir),
     .cap = sizeof(((tdb_opts_t *)0)->dir)},
    {.key = "log_db_jar",
     .type = RC_STR,
     .off = offsetof(logd_opts_t, db) + offsetof(tdb_opts_t, jar),
     .cap = sizeof(((tdb_opts_t *)0)->jar)},
    {.key = "log_db_java",
     .type = RC_STR,
     .off = offsetof(logd_opts_t, db) + offsetof(tdb_opts_t, java),
     .cap = sizeof(((tdb_opts_t *)0)->java)},
    {.key = "log_db_queue",
     .type = RC_SIZE,
     .off = offsetof(logd_opts_t, db) + offsetof(tdb_opts_t, queue_cap),
     .lo = 1,
     .hi = INT_MAX},
};

void logd_opts_default(logd_opts_t *opts) {
  snprintf(opts->socket_path, sizeof(opts->socket_path), "%s",
           DEFAULT_SOCKET_PATH);
  snprintf(opts->log_path, sizeof(opts->log_path), "%s", DEFAULT_LOG_PATH);
  opts->min_level = LOG_DEBUG;
  opts->echo = 0;
  opts->db_enable = 0;
  tdb_opts_default(&opts->db);
  snprintf(opts->db.dir, sizeof(opts->db.dir), "%s", DEFAULT_DB_DIR);
}

int logd_load_rc(const char *rc_path, logd_opts_t *opts) {
  if (rc_path == NULL || opts == NULL)
    return -1;

  logd_opts_t scratch = *opts;
  rc_defect_t defect;

  int applied = rc_bind(rc_path, LOG_KEYS, sizeof LOG_KEYS / sizeof LOG_KEYS[0],
                        &scratch, "log_", &defect);
  if (applied == RC_E_OPEN) {
    fprintf(stderr, "tetrislogd: cannot read %s: %s\n", rc_path,
            strerror(errno));
    return -1;
  }
  if (applied < 0) {
    fprintf(stderr, "tetrislogd: %s: invalid directive (%s = %s)\n", rc_path,
            defect.key, defect.value);
    return -1;
  }

  *opts = scratch;
  return applied;
}
