/*
 * config.c - rc-file overlay for tetrislogd's paths and level.
 *
 * Directive parsing itself lives in libcommon/rc.c, shared with every other
 * tetriSH component that reads .tetrishrc; this file only knows what
 * "log_ipc", "log_path" and "log_level" mean.
 */

#include "tetrislogd/tetrislogd.h"

#include "libcommon/rc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read an on/off directive, keeping fallback if the word is not one we
 * recognise. Spelled generously (on/off, yes/no, true/false, 1/0) because
 * this file is edited by hand. */
static int parse_bool(const char *value, int fallback) {
  static const char *yes[] = {"on", "yes", "true", "1"};
  static const char *no[] = {"off", "no", "false", "0"};

  for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
    if (strcasecmp(value, yes[i]) == 0)
      return 1;
    if (strcasecmp(value, no[i]) == 0)
      return 0;
  }
  fprintf(stderr, "tetrislogd: rc: invalid boolean '%s', keeping %s\n", value,
          fallback ? "on" : "off");
  return fallback;
}

/* Apply one recognised directive to opts (passed as ctx). Unknown keys are
 * ignored without comment: this file is shared with tetrish's .tetrishrc,
 * which carries directives (PATH=, commands) that mean nothing to us. */
static void apply(const char *key, const char *value, void *ctx) {
  logd_opts_t *opts = ctx;

  if (strcmp(key, "log_ipc") == 0) {
    snprintf(opts->socket_path, sizeof(opts->socket_path), "%s", value);
  } else if (strcmp(key, "log_path") == 0) {
    snprintf(opts->log_path, sizeof(opts->log_path), "%s", value);
  } else if (strcmp(key, "log_level") == 0) {
    log_level_t lvl;
    if (log_level_parse(value, &lvl) == 0)
      opts->min_level = lvl;
    else
      fprintf(stderr,
              "tetrislogd: rc: invalid log_level '%s', keeping default\n",
              value);
  } else if (strcmp(key, "log_db") == 0) {
    opts->db_enable = parse_bool(value, opts->db_enable);
  } else if (strcmp(key, "log_db_dir") == 0) {
    snprintf(opts->db.dir, sizeof(opts->db.dir), "%s", value);
  } else if (strcmp(key, "log_db_jar") == 0) {
    snprintf(opts->db.jar, sizeof(opts->db.jar), "%s", value);
  } else if (strcmp(key, "log_db_java") == 0) {
    snprintf(opts->db.java, sizeof(opts->db.java), "%s", value);
  } else if (strcmp(key, "log_db_queue") == 0) {
    char *end;
    long n = strtol(value, &end, 10);
    if (*end == '\0' && n > 0)
      opts->db.queue_cap = (size_t)n;
    else
      fprintf(stderr,
              "tetrislogd: rc: invalid log_db_queue '%s', keeping %zu\n", value,
              opts->db.queue_cap);
  }
}

void logd_load_rc(const char *rc_path, logd_opts_t *opts) {
  rc_load(rc_path, apply, opts);
}
