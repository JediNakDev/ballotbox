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

#include "libballotclient/voter.h"

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17678
#define CTL_PATH "var/run/test_client_transport.ctl"
#define CA_PATH "auth/cacsertificate.crt"
#define HOST "127.0.0.1"

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

static pid_t start_ballotd(void) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_buf[16];
    snprintf(port_buf, sizeof port_buf, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port_buf, "-C", CTL_PATH, (char *)NULL);
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
 * session_recv, decode, classify) end to end. The outcome is TIMEOUT
 * because GET_ELECTION is still a stubbed read (BB_ERR_NOT_IMPLEMENTED),
 * which bu_classify_join's default case maps to BU_JOIN_TIMEOUT - that is
 * the correct, expected answer today, not a wiring failure. */
static int test_bu_join_round_trips_through_real_daemon(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  bcl_ctx *ctx = bcl_create();
  CHECK(ctx != NULL, "bcl_create failed");
  CHECK(bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) == BB_OK, "bcl_connect failed");

  bu_session_t session;
  memset(&session, 0, sizeof(session));
  bu_join_outcome_t outcome = bu_join(ctx, &session, "E-100", "alice");

  CHECK(outcome == BU_JOIN_TIMEOUT, "expected TIMEOUT (GET_ELECTION is still stubbed)");
  CHECK(session.joined == 0, "session must not be joined on this outcome");

  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Same proof, but through bcl_send directly (RESULTS has no bu_* wrapper,
 * so ballotu.c calls bcl_send itself - this exercises that exact path). */
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

  CHECK(rc == BB_ERR_NOT_IMPLEMENTED, "GET_ELECTION is still a stubbed read");
  CHECK(resp.status == BB_ERR_NOT_IMPLEMENTED, "resp.status should match rc on a real round trip");

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
 * never touches the voter channel. */
static int test_bcl_send_create_via_ctl_succeeds(void) {
  pid_t pid = start_ballotd();
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
  run("bcl_send(CREATE) via ctl succeeds", test_bcl_send_create_via_ctl_succeeds);
  run("bcl_send(CREATE) via ctl: invalid config refused",
      test_bcl_send_create_invalid_config_via_ctl);
  run("bcl_send after bcl_disconnect fails cleanly", test_send_after_disconnect_fails_cleanly);

  unlink(CTL_PATH);
  printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
  return tests_failed == 0 ? 0 : 1;
}
