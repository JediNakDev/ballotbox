/*
 * End-to-end tests for the real client transport (bcl_connect/bcl_send,
 * src/libballotclient/transport.c) against a real bin/ballotd - the other
 * half of test_ballotd.c's picture. That file drives the daemon from raw
 * sockets to prove the server side; this one drives the real
 * libballotclient API (the same calls ballotu.c makes) to prove the client
 * side actually works, not just the harness that talks to it directly.
 *
 * Run from the repo root: make test
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libballotbrain/db.h"
#include "libballotclient/voter.h"
#include "libtetrisdb/schema.h"
#include "libtetrisdb/socket/runner.h"

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17678
#define CTL_PATH "var/run/test_client_transport.ctl"
#define CA_PATH "auth/cacsertificate.crt"
#define HOST "127.0.0.1"

/* Same isolation reasoning as test_ballotd.c: never touch the shared var/db
 * defaults, and give the DB-unreachable tests a socket nothing binds. */
#define UNREACH_DB_DIR "var/db/test_client_transport_unreachable"
#define UNREACH_DB_SOCK "var/run/test_client_transport_unreachable.sock"
#define LIVE_DB_DIR "var/db/test_client_transport_live"
#define LIVE_DB_SOCK "var/run/test_client_transport_live.sock"
#define TEST_JAR "db/dist/simpledb.jar"

static int tests_run = 0, tests_failed = 0;
static pid_t g_ballotd = -1;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      return -1;                                                          \
    }                                                                     \
  } while (0)

static void nap(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static int wait_for_tcp(int port) {
  for (int i = 0; i < 300; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof addr);
      addr.sin_family = AF_INET;
      addr.sin_port = htons((unsigned short)port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        close(fd);
        return 0;
      }
      close(fd);
    }
    nap(10);
  }
  return -1;
}

static pid_t start_ballotd_ex(const char *db_dir, const char *db_sock) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_buf[16];
    snprintf(port_buf, sizeof port_buf, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port_buf, "-C", CTL_PATH, "-d", db_dir, "-i", db_sock,
          (char *)NULL);
    perror("execl " BALLOTD_BIN);
    _exit(127);
  }
  if (wait_for_tcp(TEST_PORT) < 0) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  g_ballotd = pid;
  return pid;
}

/* Default fixture: DB deliberately unreachable. */
static pid_t start_ballotd(void) { return start_ballotd_ex(UNREACH_DB_DIR, UNREACH_DB_SOCK); }

/* Fixture for the runner-guarded block below. */
static pid_t start_ballotd_live(void) { return start_ballotd_ex(LIVE_DB_DIR, LIVE_DB_SOCK); }

static int stop_ballotd(pid_t pid) {
  int status = 0;
  if (kill(pid, SIGTERM) < 0) return -1;
  if (waitpid(pid, &status, 0) != pid) return -1;
  g_ballotd = -1;
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

/* ---- tests ------------------------------------------------------------- */

static int test_connect_succeeds(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");

  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect should succeed");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

static int test_connect_fails_when_daemon_down(void) {
  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");

  /* Nothing listens on TEST_PORT here - no daemon was started this test. */
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_ERR_DB,
        "bcl_connect must fail cleanly against a closed port");

  bcl_disconnect(ctx); /* no-op: never connected */
  bcl_destroy(ctx);
  return 0;
}

/* Proves the full real round trip (connect, encode, session_send,
 * session_recv, decode, classify) end to end. With the store unreachable,
 * db_exec's connect failure (BB_ERR_DB) falls into bu_classify_join's
 * default case, same as any other transport-level failure, so the outcome
 * is TIMEOUT - see test_join_admitted_via_live_store below for the
 * reachable-store path all the way to BU_JOIN_ADMITTED. */
static int test_bu_join_round_trips_through_real_daemon(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bu_session_t session;
  memset(&session, 0, sizeof(session));
  bu_join_outcome_t outcome = bu_join(ctx, &session, "E-100", "alice");

  CHECK(outcome == BU_JOIN_TIMEOUT, "expected TIMEOUT (store unreachable)");
  CHECK(session.joined == 0, "session must not be joined on this outcome");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Same proof, but through bcl_send directly (RESULTS has no bu_* wrapper,
 * so ballotu.c calls bcl_send itself - this exercises that exact path).
 * Store unreachable: db_exec's connect failure comes back as BB_ERR_DB. */
static int test_bcl_send_results_round_trips(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "E-042");
  snprintf(req.cert_name, BB_CERT_LEN, "alice");

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_ERR_DB, "RESULTS with no reachable store must fail cleanly");
  CHECK(resp.status == BB_ERR_DB, "resp.status should match rc on a real round trip");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

static bcl_request_t make_create_request(const char *title) {
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  snprintf(req.config.title, BB_TITLE_LEN, "%s", title);
  snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = 2;
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return req;
}

/* bcl_send routes CREATE to the ctl socket regardless of whether a voter
 * session is connected - bcl_connect() alone (no bcl_set_ctl_path) must not
 * make an admin op fall back to the voter session; it should fail cleanly
 * instead. The actual "wrong channel" rejection (ballot_session's own gate
 * refusing an admin op over TCP+tetrissh) is exercised at the wire level in
 * test_ballotd.c - the client library's routing means there is no longer a
 * public bcl_send path that could even reach that gate. */
static int test_bcl_send_admin_op_without_ctl_path_configured(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bcl_request_t req = make_create_request("Should be rejected");
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_ERR_DB, "admin op with no ctl_path configured must fail cleanly");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* The real admin path: bcl_set_ctl_path, no bcl_connect at all - ballotctl
 * never touches the voter channel. Needs a live store: CREATE really writes
 * now. */
static int test_bcl_send_create_via_ctl_succeeds(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  bcl_set_ctl_path(ctx, CTL_PATH);

  bcl_request_t req = make_create_request("Officers 2026");
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_OK, "CREATE over the real ctl socket should succeed");
  CHECK(resp.election.id[0] != '\0', "election id should be set");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* bu_join's ADMITTED path, all the way through the real client library: a
 * real CREATE, a real OPEN (bu_join only admits an OPEN election), then a
 * real JOIN as an eligible voter over the encrypted channel. */
static int test_join_admitted_via_live_store(void) {
  pid_t pid = start_ballotd_live();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *actl = bcl_create();
  CHECK(actl != NULL, "bcl_create failed");
  bcl_set_ctl_path(actl, CTL_PATH);

  bcl_request_t creq = make_create_request("Live Join");
  snprintf(creq.config.eligible[0], BB_CERT_LEN, "alice");
  creq.config.eligible_count = 1;
  bcl_response_t cresp;
  memset(&cresp, 0, sizeof cresp);
  CHECK(bcl_send(actl, &creq, &cresp) == BB_OK, "CREATE should succeed");

  bcl_request_t oreq;
  memset(&oreq, 0, sizeof oreq);
  oreq.op = BCL_OPEN;
  snprintf(oreq.election_id, BB_ID_LEN, "%s", cresp.election.id);
  snprintf(oreq.cert_name, BB_CERT_LEN, "admin");
  bcl_response_t oresp;
  memset(&oresp, 0, sizeof oresp);
  CHECK(bcl_send(actl, &oreq, &oresp) == BB_OK, "OPEN should succeed");
  bcl_destroy(actl);

  bcl_ctx *voter = bcl_create();
  CHECK(voter != NULL, "bcl_create failed");
  CHECK(bcl_connect(voter, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bu_session_t session;
  memset(&session, 0, sizeof session);
  bu_join_outcome_t outcome = bu_join(voter, &session, cresp.election.id, "alice");

  CHECK(outcome == BU_JOIN_ADMITTED, "eligible voter on an OPEN election should be admitted");
  CHECK(session.joined == 1, "session should be marked joined");
  CHECK(strcmp(session.election_id, cresp.election.id) == 0, "session should record the election id");

  bcl_disconnect(voter);
  bcl_destroy(voter);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Pure-logic path (bb_validate_config), no DB dependency - deterministic
 * today regardless of the frozen DB seam, same as test_ballotd.c's version
 * of this case but through the real client library instead of raw sockets. */
static int test_bcl_send_create_invalid_config_via_ctl(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  bcl_set_ctl_path(ctx, CTL_PATH);

  bcl_request_t req = make_create_request(""); /* empty title */
  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(ctx, &req, &resp);

  CHECK(rc == BB_ERR_CONFIG_TITLE, "empty title should be refused");
  CHECK(resp.election.id[0] == '\0', "no election id on a failed create");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

static int test_send_after_disconnect_fails_cleanly(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");
  bcl_disconnect(ctx);

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "E-042");

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  CHECK(bcl_send(ctx, &req, &resp) == BB_ERR_DB, "send after disconnect must fail, not crash");

  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- fixture: a live SocketRunner ------------------------------------------ */

static int runner_disabled(void) { return getenv("TETRISH_NO_RUNNER") != NULL; }
static int have_jar(void) { return access(TEST_JAR, R_OK) == 0; }

static int have_java(void) {
  pid_t pid = fork();
  if (pid < 0) return 0;
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
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static pid_t g_runner = -1;

static void stop_runner(void) {
  if (g_runner <= 0) return;
  kill(g_runner, SIGTERM);
  while (waitpid(g_runner, NULL, 0) < 0 && errno == EINTR);
  g_runner = -1;
  unlink(LIVE_DB_SOCK);
}

static int start_live_runner(void) {
  if (tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      tdb_ensure_table(LIVE_DB_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0) {
    fprintf(stderr, "test_client_transport: fixture: failed to provision tables\n");
    return -1;
  }

  tdb_runner_opts_t opts;
  tdb_runner_opts_default(&opts);
  snprintf(opts.dir, sizeof opts.dir, "%s", LIVE_DB_DIR);
  snprintf(opts.jar, sizeof opts.jar, "%s", TEST_JAR);
  snprintf(opts.ipc, sizeof opts.ipc, "%s", LIVE_DB_SOCK);
  opts.sessions = 8;
  opts.recover = 0;

  g_runner = tdb_runner_spawn(&opts, -1);
  if (g_runner <= 0) {
    fprintf(stderr, "test_client_transport: fixture: failed to spawn a runner\n");
    return -1;
  }
  if (tdb_runner_wait(LIVE_DB_SOCK, g_runner, 20000) != 0) {
    fprintf(stderr, "test_client_transport: fixture: runner never accepted a connection\n");
    stop_runner();
    return -1;
  }
  return 0;
}

/* ---- harness ------------------------------------------------------------- */

static void run(const char *name, int (*fn)(void)) {
  tests_run++;
  printf("  %-52s", name);
  fflush(stdout);
  if (fn() == 0) {
    printf("ok\n");
  } else {
    tests_failed++;
    printf("  -> FAILED\n");
  }
  if (g_ballotd > 0) {
    kill(g_ballotd, SIGKILL);
    waitpid(g_ballotd, NULL, 0);
    g_ballotd = -1;
  }
}

int main(void) {
  struct stat st;
  if (stat(BALLOTD_BIN, &st) != 0) {
    fprintf(stderr, "test_client_transport: %s not built (run make)\n", BALLOTD_BIN);
    return 1;
  }
  if (stat("bin/ballot_session", &st) != 0) {
    fprintf(stderr, "test_client_transport: bin/ballot_session not built (run make)\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  printf("client transport end-to-end tests\n");
  run("bcl_connect succeeds against a real ballotd", test_connect_succeeds);
  run("bcl_connect fails cleanly against a closed port", test_connect_fails_when_daemon_down);
  run("bu_join round-trips through the real daemon", test_bu_join_round_trips_through_real_daemon);
  run("bcl_send(RESULTS) round-trips through the real daemon", test_bcl_send_results_round_trips);
  run("bcl_send admin op fails cleanly with no ctl_path set",
      test_bcl_send_admin_op_without_ctl_path_configured);
  run("bcl_send(CREATE) via ctl: invalid config refused",
      test_bcl_send_create_invalid_config_via_ctl);
  run("bcl_send after bcl_disconnect fails cleanly", test_send_after_disconnect_fails_cleanly);

  if (!runner_disabled() && have_jar() && have_java()) {
    if (start_live_runner() == 0) {
      run("bcl_send(CREATE) via ctl succeeds (live store)", test_bcl_send_create_via_ctl_succeeds);
      run("bu_join admits an eligible voter (live store)", test_join_admitted_via_live_store);
      stop_runner();
    } else {
      tests_failed++;
    }
  } else {
    printf("  (skipping live-store tests: %s)\n",
           runner_disabled()   ? "TETRISH_NO_RUNNER is set"
           : !have_jar()       ? "no " TEST_JAR " - run `ant dist` in db/"
                                : "no java on PATH");
    if (!runner_disabled() && getenv("TETRISH_REQUIRE_RUNNER") != NULL) {
      fprintf(stderr, "    FAIL: TETRISH_REQUIRE_RUNNER is set and the runner tests could not run\n");
      tests_failed++;
    }
  }

  unlink(CTL_PATH);
  printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
  return tests_failed == 0 ? 0 : 1;
}
