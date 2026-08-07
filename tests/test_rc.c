/* Public-interface tests for the .tetrishrc configuration contract.
 *
 * Run from the repo root: make bin/test_rc && ./bin/test_rc */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libcommon/rc.h"
#include "libtetrisauth/config.h"
#include "libtetrisdb/socket/runner.h"
#include "tetrislogd/tetrislogd.h"

static int tests_run;
static int tests_failed;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static void run(const char *name, int (*fn)(void)) {
  printf("  %s\n", name);
  tests_run++;
  if (fn() != 0)
    tests_failed++;
}

static int write_fixture(char *path, size_t cap, const char *text) {
  snprintf(path, cap, "/tmp/tetrish-rc-XXXXXX");
  int fd = mkstemp(path);
  if (fd < 0)
    return -1;
  size_t len = strlen(text);
  ssize_t n = write(fd, text, len);
  close(fd);
  return n == (ssize_t)len ? 0 : -1;
}

static int test_db_table_and_defaults(void) {
  static const char *const expected[] = {
      "db_dir", "db_ipc", "db_sessions", "db_jar", "db_java", "db_timeout",
      NULL,
  };

  size_t i = 0;
  for (; expected[i] != NULL; i++)
    CHECK(tdb_rc_keys[i] != NULL && strcmp(tdb_rc_keys[i], expected[i]) == 0,
          "db key table differs from the settled surface");
  CHECK(tdb_rc_keys[i] == NULL, "db key table has an unexpected key");

  tdb_runner_opts_t opts;
  tdb_runner_opts_default(&opts);
  CHECK(strcmp(opts.dir, "var/db") == 0, "db_dir default");
  CHECK(strcmp(opts.ipc, "var/run/tetrisdb.sock") == 0, "db_ipc default");
  CHECK(opts.sessions == 16, "db_sessions default");
  CHECK(strcmp(opts.jar, "db/dist/simpledb.jar") == 0, "db_jar default");
  CHECK(strcmp(opts.java, "java") == 0, "db_java default");
  return 0;
}

static int test_db_loads_valid_file(void) {
  char path[64];
  CHECK(write_fixture(path, sizeof(path),
                      "db_dir = data\n"
                      "db_ipc = run/db.sock\n"
                      "db_sessions = 254\n"
                      "db_jar = simpledb.jar\n"
                      "db_java = /usr/bin/java\n"
                      "db_timeout = 60000\n"
                      "auth_max_attempts = 5\n") == 0,
        "write db fixture");

  tdb_runner_opts_t opts;
  tdb_runner_opts_default(&opts);
  int applied = tdb_runner_opts_load(path, &opts);
  unlink(path);

  CHECK(applied == 7, "db loader directive count");
  CHECK(strcmp(opts.dir, "data") == 0, "db_dir overlay");
  CHECK(strcmp(opts.ipc, "run/db.sock") == 0, "db_ipc overlay");
  CHECK(opts.sessions == 254, "db_sessions upper bound");
  CHECK(strcmp(opts.jar, "simpledb.jar") == 0, "db_jar overlay");
  CHECK(strcmp(opts.java, "/usr/bin/java") == 0, "db_java overlay");
  return 0;
}

static int test_auth_table_and_valid_file(void) {
  static const char *const expected[] = {
      "auth_max_attempts", "auth_token_ttl", "auth_pbkdf2_iters", NULL,
  };

  size_t i = 0;
  for (; expected[i] != NULL; i++)
    CHECK(tauth_rc_keys[i] != NULL &&
              strcmp(tauth_rc_keys[i], expected[i]) == 0,
          "auth key table differs from the settled surface");
  CHECK(tauth_rc_keys[i] == NULL, "auth key table has an unexpected key");

  char path[64];
  CHECK(write_fixture(path, sizeof(path),
                      "auth_max_attempts = 100\n"
                      "auth_token_ttl = 31536000\n"
                      "auth_pbkdf2_iters = 10000000\n"
                      "db_timeout = 2000\n") == 0,
        "write auth fixture");
  int applied = tauth_rc_validate(path);
  unlink(path);
  CHECK(applied == 4, "auth validator rejected settled upper bounds");
  CHECK(TAUTH_DEFAULT_MAX_ATTEMPTS == 5, "auth_max_attempts default");
  CHECK(TAUTH_DEFAULT_TOKEN_TTL == 604800, "auth_token_ttl default");
  CHECK(TAUTH_DEFAULT_PBKDF2_ITERS == 600000, "auth_pbkdf2_iters default");
  return 0;
}

static int test_log_table_defaults_and_valid_file(void) {
  static const char *const expected[] = {
      "log_path", "log_ipc", "log_level", "log_send_attempts", "log_db",
      "log_db_dir", "log_db_jar", "log_db_java", "log_db_queue", NULL,
  };

  size_t i = 0;
  for (; expected[i] != NULL; i++)
    CHECK(logd_rc_keys[i] != NULL &&
              strcmp(logd_rc_keys[i], expected[i]) == 0,
          "log key table differs from the settled surface");
  CHECK(logd_rc_keys[i] == NULL, "log key table has an unexpected key");

  logd_opts_t opts;
  logd_opts_default(&opts);
  CHECK(strcmp(opts.log_path, "var/log/tetrisd.log") == 0,
        "log_path default");
  CHECK(strcmp(opts.socket_path, "var/run/tetrislogd.sock") == 0,
        "log_ipc default");
  CHECK(opts.min_level == LOG_DEBUG, "log_level default");
  CHECK(opts.db_enable == 0, "log_db default");
  CHECK(strcmp(opts.db.dir, "var/db_log") == 0, "log_db_dir default");
  CHECK(opts.db.queue_cap == 256, "log_db_queue default");

  char path[64];
  CHECK(write_fixture(path, sizeof(path),
                      "log_path = -\n"
                      "log_ipc = run/log.sock\n"
                      "log_level = error\n"
                      "log_send_attempts = 1000\n"
                      "log_db = yes\n"
                      "log_db_dir = logs\n"
                      "log_db_jar = simpledb.jar\n"
                      "log_db_java = /usr/bin/java\n"
                      "log_db_queue = 1\n"
                      "auth_max_attempts = 5\n") == 0,
        "write log fixture");
  int applied = logd_load_rc(path, &opts);
  unlink(path);

  CHECK(applied == 10, "log loader directive count");
  CHECK(strcmp(opts.log_path, "-") == 0, "log_path overlay");
  CHECK(strcmp(opts.socket_path, "run/log.sock") == 0, "log_ipc overlay");
  CHECK(opts.min_level == LOG_ERROR, "log_level overlay");
  CHECK(opts.db_enable == 1, "log_db overlay");
  CHECK(strcmp(opts.db.dir, "logs") == 0, "log_db_dir overlay");
  CHECK(opts.db.queue_cap == 1, "log_db_queue lower bound");
  return 0;
}

typedef struct {
  int db[6];
  int auth[3];
  int log[9];
  int unknown_owned;
} sample_keys_t;

static int find_key(const char *const *table, const char *key) {
  for (int i = 0; table[i] != NULL; i++)
    if (strcmp(table[i], key) == 0)
      return i;
  return -1;
}

static void count_sample_key(const char *key, const char *value, void *ctx) {
  (void)value;
  sample_keys_t *keys = ctx;
  int i;

  if ((i = find_key(tdb_rc_keys, key)) >= 0)
    keys->db[i]++;
  else if ((i = find_key(tauth_rc_keys, key)) >= 0)
    keys->auth[i]++;
  else if ((i = find_key(logd_rc_keys, key)) >= 0)
    keys->log[i]++;
  else if (strncmp(key, "db_", 3) == 0 ||
           strncmp(key, "auth_", 5) == 0 ||
           strncmp(key, "log_", 4) == 0)
    keys->unknown_owned++;
}

static int test_sample_matches_exported_surface(void) {
  sample_keys_t keys = {0};
  int applied = rc_load("sample.tetrishrc", count_sample_key, &keys);
  CHECK(applied >= 0, "sample.tetrishrc is readable");
  CHECK(keys.unknown_owned == 0, "sample has an unknown owned key");

  for (int i = 0; tdb_rc_keys[i] != NULL; i++)
    CHECK(keys.db[i] == 1, "db key missing or duplicated in sample");
  for (int i = 0; tauth_rc_keys[i] != NULL; i++)
    CHECK(keys.auth[i] == 1, "auth key missing or duplicated in sample");
  for (int i = 0; logd_rc_keys[i] != NULL; i++)
    CHECK(keys.log[i] == 1, "log key missing or duplicated in sample");

  tdb_runner_opts_t db;
  tdb_runner_opts_default(&db);
  CHECK(tdb_runner_opts_load("sample.tetrishrc", &db) == applied,
        "sample has an invalid db_ value");
  CHECK(tauth_rc_validate("sample.tetrishrc") == applied,
        "sample has an invalid auth_ value");
  logd_opts_t log;
  logd_opts_default(&log);
  CHECK(logd_load_rc("sample.tetrishrc", &log) == applied,
        "sample has an invalid log_ value");

  return 0;
}

static int db_rejects(const char *line) {
  char path[64];
  if (write_fixture(path, sizeof(path), line) != 0)
    return 0;
  tdb_runner_opts_t opts;
  tdb_runner_opts_default(&opts);
  int rejected = tdb_runner_opts_load(path, &opts) < 0;
  unlink(path);
  return rejected;
}

static int auth_rejects(const char *line) {
  char path[64];
  if (write_fixture(path, sizeof(path), line) != 0)
    return 0;
  int rejected = tauth_rc_validate(path) < 0;
  unlink(path);
  return rejected;
}

static int log_rejects(const char *line) {
  char path[64];
  if (write_fixture(path, sizeof(path), line) != 0)
    return 0;
  logd_opts_t opts;
  logd_opts_default(&opts);
  int rejected = logd_load_rc(path, &opts) < 0;
  unlink(path);
  return rejected;
}

static int test_invalid_values_and_unknown_owned_keys(void) {
  static const char *const bad_db[] = {
      "db_dir =\n",          "db_ipc =\n",         "db_sessions = 0\n",
      "db_sessions = 255\n", "db_jar =\n",          "db_java =\n",
      "db_timeout = 99\n",   "db_timeout = 60001\n", "db_typo = 1\n",
  };
  static const char *const bad_auth[] = {
      "auth_max_attempts = 0\n",       "auth_max_attempts = 101\n",
      "auth_token_ttl = 59\n",         "auth_token_ttl = 31536001\n",
      "auth_pbkdf2_iters = 0\n",       "auth_pbkdf2_iters = 10000001\n",
      "auth_unknown = 1\n",
  };
  static const char *const bad_log[] = {
      "log_path =\n",          "log_ipc =\n",
      "log_level = verbose\n", "log_send_attempts = 0\n",
      "log_send_attempts = 1001\n",
      "log_db = maybe\n",      "log_db_dir =\n",
      "log_db_jar =\n",        "log_db_java =\n",
      "log_db_queue = 0\n",    "log_db_queue = 12records\n",
      "log_unknown = 1\n",
  };

  for (size_t i = 0; i < sizeof(bad_db) / sizeof(bad_db[0]); i++)
    CHECK(db_rejects(bad_db[i]), "invalid or unknown db_ directive accepted");
  for (size_t i = 0; i < sizeof(bad_auth) / sizeof(bad_auth[0]); i++)
    CHECK(auth_rejects(bad_auth[i]),
          "invalid or unknown auth_ directive accepted");
  for (size_t i = 0; i < sizeof(bad_log) / sizeof(bad_log[0]); i++)
    CHECK(log_rejects(bad_log[i]),
          "invalid or unknown log_ directive accepted");
  return 0;
}

static int socket_path_result(int is_log, size_t len) {
  char value[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
  memset(value, 's', len);
  value[len] = '\0';

  char line[sizeof(value) + 32];
  snprintf(line, sizeof(line), "%s = %s\n", is_log ? "log_ipc" : "db_ipc",
           value);

  char path[64];
  if (write_fixture(path, sizeof(path), line) != 0)
    return -1;
  int result;
  if (is_log) {
    logd_opts_t opts;
    logd_opts_default(&opts);
    result = logd_load_rc(path, &opts);
  } else {
    tdb_runner_opts_t opts;
    tdb_runner_opts_default(&opts);
    result = tdb_runner_opts_load(path, &opts);
  }
  unlink(path);
  return result;
}

static int test_socket_path_bounds(void) {
  size_t cap = sizeof(((struct sockaddr_un *)0)->sun_path);
  CHECK(socket_path_result(0, cap - 1) == 1,
        "db_ipc rejected the longest usable socket path");
  CHECK(socket_path_result(0, cap) < 0,
        "db_ipc accepted a socket path that cannot fit");
  CHECK(socket_path_result(1, cap - 1) == 1,
        "log_ipc rejected the longest usable socket path");
  CHECK(socket_path_result(1, cap) < 0,
        "log_ipc accepted a socket path that cannot fit");
  return 0;
}

static int test_missing_files_are_distinct(void) {
  const char *missing = "/tmp/tetrish-rc-file-that-does-not-exist";
  unlink(missing);
  sample_keys_t keys = {0};
  CHECK(rc_load(missing, count_sample_key, &keys) == -1,
        "rc_load reported a missing file as an empty file");

  tdb_runner_opts_t db;
  tdb_runner_opts_default(&db);
  CHECK(tdb_runner_opts_load(missing, &db) == -1,
        "db loader accepted a missing file");
  CHECK(tauth_rc_validate(missing) == -1,
        "auth validator accepted a missing file");
  logd_opts_t log;
  logd_opts_default(&log);
  CHECK(logd_load_rc(missing, &log) == -1,
        "log loader accepted a missing file");
  return 0;
}

static int wait_for_exit(pid_t pid, int *status) {
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int i = 0; i < 200; i++) {
    pid_t done = waitpid(pid, status, WNOHANG);
    if (done == pid)
      return 0;
    if (done < 0)
      return -1;
    nanosleep(&pause, NULL);
  }
  kill(pid, SIGKILL);
  waitpid(pid, status, 0);
  return -1;
}

static int test_invalid_log_config_refuses_startup(void) {
  char root[PATH_MAX];
  CHECK(getcwd(root, sizeof(root)) != NULL, "get repo working directory");
  char binary[PATH_MAX];
  CHECK(snprintf(binary, sizeof(binary), "%s/bin/tetrislogd", root) <
            (int)sizeof(binary),
        "tetrislogd path fits");

  char dir[] = "/tmp/tetrish-logd-XXXXXX";
  CHECK(mkdtemp(dir) != NULL, "create logd fixture directory");
  char rc_path[PATH_MAX];
  snprintf(rc_path, sizeof(rc_path), "%s/.tetrishrc", dir);
  FILE *rc = fopen(rc_path, "w");
  CHECK(rc != NULL, "create invalid logd rc file");
  CHECK(fputs("log_level = verbose\n", rc) >= 0, "write invalid logd rc file");
  fclose(rc);

  pid_t pid = fork();
  CHECK(pid >= 0, "fork tetrislogd");
  if (pid == 0) {
    if (chdir(dir) != 0)
      _exit(126);
    char *const argv[] = {binary, NULL};
    execv(binary, argv);
    _exit(127);
  }

  int status = 0;
  int exited = wait_for_exit(pid, &status);
  unlink(rc_path);
  rmdir(dir);
  CHECK(exited == 0, "invalid config left tetrislogd running");
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0,
        "invalid config did not make tetrislogd refuse startup");
  return 0;
}

int main(void) {
  printf("test_rc\n");
  run("db table and defaults", test_db_table_and_defaults);
  run("db loads a valid file", test_db_loads_valid_file);
  run("auth table and valid file", test_auth_table_and_valid_file);
  run("log table, defaults, and valid file",
      test_log_table_defaults_and_valid_file);
  run("sample matches exported surface", test_sample_matches_exported_surface);
  run("invalid values and unknown owned keys",
      test_invalid_values_and_unknown_owned_keys);
  run("socket path bounds", test_socket_path_bounds);
  run("missing files are distinct", test_missing_files_are_distinct);
  run("invalid log config refuses startup",
      test_invalid_log_config_refuses_startup);
  printf("%d test(s), %d failed\n", tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
