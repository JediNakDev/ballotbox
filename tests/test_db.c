/* Tests for libtetrisdb: table creation, SQL quoting, and one end-to-end
 * round trip through a real PipeRunner child process.
 *
 * The round trip needs a JVM and db/dist/simpledb.jar. When either is
 * missing those tests are skipped rather than failed - a machine without a
 * JVM can still build and test the rest of tetriSH, which is the whole reason
 * database mirroring is opt-in.
 *
 * Run from the repo root: make test */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libtetrisdb/tetrisdb.h"

#define TEST_DIR "var/db_test"
#define TEST_JAR "db/dist/simpledb.jar"
#define TEST_SCHEMA "id int, pid int, ts int, status string, msg string"

static int tests_run = 0, tests_failed = 0;

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

/* Remove the scratch directory so each run starts from nothing. */
static void reset_dir(void) {
  (void)unlink(TEST_DIR "/catalog.txt");
  (void)unlink(TEST_DIR "/log.dat");
  (void)rmdir(TEST_DIR);
}

static long file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* Read a whole file into buf (NUL-terminated). Returns 0 on success. */
static int slurp(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return 0;
}

/* === Table creation === */

static int test_creates_table(void) {
  reset_dir();
  CHECK(tdb_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

  char catalog[1024];
  CHECK(slurp(TEST_DIR "/catalog.txt", catalog, sizeof(catalog)) == 0,
        "no catalog written");
  CHECK(strstr(catalog, "log (" TEST_SCHEMA ")") != NULL,
        "catalog line missing or malformed");

  /* One empty page, not zero bytes: a zero-length heap file cannot be
   * scanned, so a fresh table would fail every SELECT until first write. */
  CHECK(file_size(TEST_DIR "/log.dat") == 4096, "log.dat is not one page");
  return 0;
}

static int test_create_is_idempotent(void) {
  reset_dir();
  CHECK(tdb_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "first failed");

  /* Put a byte in the data file so truncation would be visible. */
  int fd = open(TEST_DIR "/log.dat", O_WRONLY);
  CHECK(fd >= 0, "cannot open log.dat");
  CHECK(write(fd, "x", 1) == 1, "cannot write log.dat");
  close(fd);

  CHECK(tdb_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "second failed");

  char catalog[1024];
  CHECK(slurp(TEST_DIR "/catalog.txt", catalog, sizeof(catalog)) == 0,
        "no catalog");
  int lines = 0;
  for (const char *p = catalog; (p = strstr(p, "log (")) != NULL; p++)
    lines++;
  CHECK(lines == 1, "catalog gained a duplicate line");
  CHECK(file_size(TEST_DIR "/log.dat") == 4096, "existing data was truncated");

  char first = 0;
  fd = open(TEST_DIR "/log.dat", O_RDONLY);
  CHECK(fd >= 0 && read(fd, &first, 1) == 1, "cannot reread log.dat");
  close(fd);
  CHECK(first == 'x', "existing data was overwritten");
  return 0;
}

/* === Quoting === */

static int test_quote_plain(void) {
  char out[64];
  tdb_quote(out, sizeof(out), "hello");
  CHECK(strcmp(out, "'hello'") == 0, "plain text not wrapped");
  return 0;
}

static int test_quote_doubles_quotes(void) {
  char out[64];
  tdb_quote(out, sizeof(out), "it's");
  CHECK(strcmp(out, "'it''s'") == 0, "quote not doubled");
  return 0;
}

static int test_quote_injection_stays_one_literal(void) {
  char out[128];
  tdb_quote(out, sizeof(out), "x'); insert into log values (9);--");

  /* Every quote in the payload must be doubled, so the parser sees one
   * literal and never a second statement. Counting is enough: an odd number
   * of consecutive quotes anywhere would end the literal early. */
  int runs_of_odd_quotes = 0;
  for (const char *p = out + 1; *p != '\0' && p[1] != '\0';) {
    if (*p != '\'') {
      p++;
      continue;
    }
    int n = 0;
    while (p[n] == '\'')
      n++;
    if (n % 2 != 0)
      runs_of_odd_quotes++;
    p += n;
  }
  CHECK(out[0] == '\'', "missing opening quote");
  CHECK(out[strlen(out) - 1] == '\'', "missing closing quote");
  CHECK(runs_of_odd_quotes == 0, "an unescaped quote survived");
  return 0;
}

static int test_quote_truncates_safely(void) {
  char out[8];
  tdb_quote(out, sizeof(out), "''''''''''''''''");
  CHECK(strlen(out) < sizeof(out), "overflowed the buffer");
  CHECK(out[0] == '\'' && out[strlen(out) - 1] == '\'',
        "truncation left an unterminated literal");
  return 0;
}

/* === End to end === */

/* Is a JVM and the jar available? If not, the round-trip tests are skipped. */
static int have_java(void) {
  if (access(TEST_JAR, R_OK) != 0)
    return 0;

  pid_t pid = fork();
  if (pid < 0)
    return 0;
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execlp("java", "java", "-version", (char *)NULL);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    ;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void test_opts(tdb_opts_t *opts) {
  tdb_opts_default(opts);
  snprintf(opts->dir, sizeof(opts->dir), "%s", TEST_DIR);
  snprintf(opts->jar, sizeof(opts->jar), "%s", TEST_JAR);
}

/* Write two rows, shut down, then reopen and read them back - which also
 * proves the clean shutdown flushed them to disk. */
static int test_round_trip(void) {
  tdb_opts_t opts;
  unsigned long dropped = 1, errors = 1;

  reset_dir();
  test_opts(&opts);
  CHECK(tdb_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

  tdb_t *db = tdb_start(&opts, NULL, NULL, 0);
  CHECK(db != NULL, "cannot start PipeRunner");

  char quoted[64];
  char sql[256];
  tdb_quote(quoted, sizeof(quoted), "it's fine");
  snprintf(sql, sizeof(sql),
           "insert into log values (1, 42, 7, 'INFO', %s);", quoted);
  CHECK(tdb_submit(db, sql) == 0, "submit rejected");
  CHECK(tdb_submit(db, "insert into log values (2, 42, 8, 'WARN', 'second');") ==
            0,
        "submit rejected");

  tdb_stop(db, &dropped, &errors);
  CHECK(dropped == 0, "statements were dropped");
  CHECK(errors == 0, "SimpleDB rejected a statement");

  /* Reopen and let the startup probe read the table back. */
  char body[2048];
  db = tdb_start(&opts, "select max(id) from log;", body, sizeof(body));
  CHECK(db != NULL, "cannot restart PipeRunner");
  tdb_stop(db, NULL, NULL);

  CHECK(strstr(body, "max (log.id)") != NULL, "probe returned no result");
  /* The rule line, then the value: 2, because two rows were written. */
  const char *rule = strstr(body, "---");
  CHECK(rule != NULL, "probe output has no result table");
  const char *nl = strchr(rule, '\n');
  CHECK(nl != NULL && atoi(nl + 1) == 2, "max(id) is not 2 after two inserts");
  return 0;
}

/* A statement the parser rejects must be counted, not silently swallowed,
 * and must not take the connection down with it. */
static int test_bad_sql_is_counted(void) {
  tdb_opts_t opts;
  unsigned long dropped = 0, errors = 0;

  reset_dir();
  test_opts(&opts);
  CHECK(tdb_ensure_table(TEST_DIR, "log", TEST_SCHEMA) == 0, "create failed");

  tdb_t *db = tdb_start(&opts, NULL, NULL, 0);
  CHECK(db != NULL, "cannot start PipeRunner");

  CHECK(tdb_submit(db, "insert into nosuchtable values (1);") == 0,
        "submit rejected");
  CHECK(tdb_submit(db, "insert into log values (1, 1, 1, 'INFO', 'after');") ==
            0,
        "submit rejected");

  tdb_stop(db, &dropped, &errors);
  CHECK(errors == 1, "bad statement was not counted as an error");
  CHECK(dropped == 0, "good statement after a bad one was dropped");
  return 0;
}

int main(void) {
  printf("test_db\n");

  run("creates catalog entry and one-page data file", test_creates_table);
  run("second create leaves existing table alone", test_create_is_idempotent);
  run("quotes plain text", test_quote_plain);
  run("doubles embedded quotes", test_quote_doubles_quotes);
  run("keeps an injection payload inside one literal",
      test_quote_injection_stays_one_literal);
  run("truncates without breaking the literal", test_quote_truncates_safely);

  if (have_java()) {
    run("writes rows and reads them back after restart", test_round_trip);
    run("counts a rejected statement and keeps going",
        test_bad_sql_is_counted);
  } else {
    printf("  (skipping round-trip tests: no java or " TEST_JAR ")\n");
  }

  reset_dir();
  printf("%d test(s), %d failed\n", tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
