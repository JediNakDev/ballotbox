#include "tetrisd/config.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tetrish/lib/rc_parser.h"

/* Built-in defaults. Relative paths are resolved against the daemon's cwd,
 * matching the var/log and var/run layout created by the Makefile. */
#define DEFAULT_LISTEN_PORT 4242
#define DEFAULT_CERT_PATH "auth/server.crt"
#define DEFAULT_KEY_PATH "auth/server.key"
#define DEFAULT_CA_PATH "auth/ca.crt"
#define DEFAULT_LOG_PATH "var/log/tetrisd.log"
#define DEFAULT_LOG_IPC "var/run/tetrislogd.sock"
#define DEFAULT_CTL_SOCKET "var/run/tetrisd.ctl"

/* Copy value into a fixed field, trimming trailing whitespace/newline. */
static void set_str(char *dst, size_t cap, const char *value) {
  size_t len = strlen(value);
  while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
                     value[len - 1] == ' ' || value[len - 1] == '\t'))
    len--;
  if (len > cap - 1)
    len = cap - 1;
  memcpy(dst, value, len);
  dst[len] = '\0';
}

void config_defaults(tetrisd_config_t *cfg) {
  cfg->listen_port = DEFAULT_LISTEN_PORT;
  set_str(cfg->cert_path, sizeof(cfg->cert_path), DEFAULT_CERT_PATH);
  set_str(cfg->key_path, sizeof(cfg->key_path), DEFAULT_KEY_PATH);
  set_str(cfg->ca_path, sizeof(cfg->ca_path), DEFAULT_CA_PATH);
  set_str(cfg->log_path, sizeof(cfg->log_path), DEFAULT_LOG_PATH);
  set_str(cfg->log_ipc, sizeof(cfg->log_ipc), DEFAULT_LOG_IPC);
  set_str(cfg->ctl_socket, sizeof(cfg->ctl_socket), DEFAULT_CTL_SOCKET);
}

/* Table of the string-valued directives: name -> where it lives in the
 * struct. listen_port is handled separately because it needs parsing. */
#define STR_FIELD(name)                                                        \
  {#name, offsetof(tetrisd_config_t, name),                                    \
   sizeof(((tetrisd_config_t *)0)->name)}

static const struct {
  const char *key;
  size_t offset;
  size_t size;
} str_fields[] = {
    STR_FIELD(cert_path),  STR_FIELD(key_path), STR_FIELD(ca_path),
    STR_FIELD(log_path),   STR_FIELD(log_ipc),  STR_FIELD(ctl_socket),
};

/* Apply one recognised directive to cfg. Unknown keys are reported but not
 * fatal - .tetrishrc may be shared with the shell (PATH=, etc.). */
static void apply_directive(tetrisd_config_t *cfg, const char *key,
                            const char *value) {
  if (strcmp(key, "listen_port") == 0) {
    char *end = NULL;
    long port = strtol(value, &end, 10);
    if (end == value || port < 1 || port > 65535) {
      fprintf(stderr, "tetrisd: config: invalid listen_port '%s', keeping %d\n",
              value, cfg->listen_port);
      return;
    }
    cfg->listen_port = (int)port;
    return;
  }

  for (size_t i = 0; i < sizeof(str_fields) / sizeof(str_fields[0]); i++) {
    if (strcmp(key, str_fields[i].key) == 0) {
      set_str((char *)cfg + str_fields[i].offset, str_fields[i].size, value);
      return;
    }
  }

  if (strcmp(key, "PATH") == 0)
    return; /* shell directive from a shared .tetrishrc: silently ignored */

  fprintf(stderr, "tetrisd: config: ignoring unknown directive '%s'\n", key);
}

int config_load(const char *path, tetrisd_config_t *cfg) {
  config_defaults(cfg);

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    if (errno == ENOENT)
      return 0; /* no file: defaults stand */
    fprintf(stderr, "tetrisd: config: cannot open %s: %s\n", path,
            strerror(errno));
    return -1;
  }

  char line[PATH_MAX + 64];
  char key[64];
  const char *value;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (rc_split_kv(line, key, sizeof(key), &value))
      apply_directive(cfg, key, value);
  }

  fclose(f);
  return 0;
}

void config_print(const tetrisd_config_t *cfg) {
  printf("tetrisd: config:\n");
  printf("  listen_port = %d\n", cfg->listen_port);
  printf("  cert_path   = %s\n", cfg->cert_path);
  printf("  key_path    = %s\n", cfg->key_path);
  printf("  ca_path     = %s\n", cfg->ca_path);
  printf("  log_path    = %s\n", cfg->log_path);
  printf("  log_ipc     = %s\n", cfg->log_ipc);
  printf("  ctl_socket  = %s\n", cfg->ctl_socket);
}
