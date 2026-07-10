#ifndef TETRISD_CONFIG_H
#define TETRISD_CONFIG_H

/*
 * config.h - daemon-wide configuration parsed from .tetrishrc.
 *
 * The config loader reuses the pure key=value helper in
 * src/tetrish/lib/rc_parser.c (rc_split_kv) rather than shipping a second
 * parser. It reads a small, fixed set of directives; every field has a
 * sensible built-in default so tetrisd runs from an empty (or absent)
 * config file.
 *
 * Recognised directives (all optional):
 *   listen_port   public TCP port for game clients
 *   cert_path     server certificate (libtetrissh)
 *   key_path      server private key
 *   ca_path       trusted CA bundle for client auth
 *   log_path      file tetrislogd writes records to
 *   log_ipc       Unix datagram socket for the logger IPC (ADR 0006)
 *   ctl_socket    Unix stream socket for the control plane (ADR 0007)
 *
 * Unknown keys are ignored (with a warning) so a shared .tetrishrc that
 * also carries shell directives like PATH= does not break the daemon.
 */

#include <limits.h>

typedef struct {
  int listen_port;
  char cert_path[PATH_MAX];
  char key_path[PATH_MAX];
  char ca_path[PATH_MAX];
  char log_path[PATH_MAX];
  char log_ipc[PATH_MAX];
  char ctl_socket[PATH_MAX];
} tetrisd_config_t;

/*
 * Populate cfg with built-in defaults, then overlay any directives found
 * in the file at path. A missing file is not an error: the defaults stand.
 *
 * Returns 0 on success, -1 if the file exists but could not be opened/read.
 * A malformed line is skipped (with a warning) and does not fail the load.
 */
int config_load(const char *path, tetrisd_config_t *cfg);

/* Fill cfg with the built-in defaults only. */
void config_defaults(tetrisd_config_t *cfg);

/* Print the effective config (one directive per line) for inspection. */
void config_print(const tetrisd_config_t *cfg);

#endif /* TETRISD_CONFIG_H */
