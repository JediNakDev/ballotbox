/*
 * End-to-end tests for ballotd: the real daemon, the real TCP+tetrissh
 * voter channel (forking the real bin/ballot_session per connection), and
 * the real local admin channel. Each test starts a fresh daemon, drives it
 * over one or both channels, and stops it with SIGTERM.
 *
 * Run from the repo root: make test
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

#include "ballotd/control_plane.h"
#include "libballotclient/codec.h"
#include "libhtttp/htttp.h"
#include "libtetrissh/tetrissh.h"

#define BALLOTD_BIN "bin/ballotd"
#define TEST_PORT 17677
#define CTL_PATH "var/run/test_ballotd.ctl"
#define CA_PATH "auth/cacsertificate.crt"

static int tests_run = 0, tests_failed = 0;

/* The daemon a test currently has running, so a failing test (which returns
 * early) cannot leak one into the next test - same convention as
 * tests/test_logd.c's g_logd. */
static pid_t g_ballotd = -1;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);    \
      return -1;                                                             \
    }                                                                        \
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

static int wait_for_path(const char *path) {
  struct stat st;
  for (int i = 0; i < 300; i++) {
    if (lstat(path, &st) == 0) return 0;
    nap(10);
  }
  return -1;
}

static pid_t start_ballotd(void) {
  unlink(CTL_PATH);

  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_buf[16];
    snprintf(port_buf, sizeof port_buf, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port_buf, "-C", CTL_PATH, (char *)NULL);
    perror("execl " BALLOTD_BIN);
    _exit(127);
  }

  if (wait_for_tcp(TEST_PORT) < 0 || wait_for_path(CTL_PATH) < 0) {
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

/* ---- voter channel: TCP + tetrissh ---------------------------------------- */

static int voter_connect(session_t *cli) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(TEST_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  if (session_connect(cli, fd, CA_PATH) != SESSION_OK) {
    close(fd);
    return -1;
  }
  return fd;
}

static int voter_send_recv(session_t *cli, const uint8_t *wire, uint32_t wlen, uint8_t *rbuf,
                           uint32_t rcap, htttp_response_t *http) {
  if (session_send(cli, wire, wlen) != SESSION_OK) return -1;
  uint32_t rlen = rcap;
  if (session_recv(cli, rbuf, &rlen) != SESSION_OK) return -1;
  return htttp_parse_response(rbuf, rlen, http) == HTTTP_OK ? 0 : -1;
}

/* ---- admin channel: local AF_UNIX, one-shot per connection ---------------- */

static int ctl_connect(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", CTL_PATH);

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int ctl_send_recv(int fd, const uint8_t *wire, uint32_t wlen, uint8_t *rbuf, uint32_t rcap,
                         htttp_response_t *http) {
  if (ctl_frame_write(fd, wire, wlen) != 0) return -1;
  uint32_t rlen = 0;
  if (ctl_frame_read(fd, rbuf, rcap, &rlen) != 0) return -1;
  return htttp_parse_response(rbuf, rlen, http) == HTTTP_OK ? 0 : -1;
}

/* ---- fixtures --------------------------------------------------------------- */

static bb_config_t valid_config(const char *title) {
  bb_config_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.title, BB_TITLE_LEN, "%s", title);
  snprintf(c.options[0], BB_OPTION_LEN, "Yes");
  snprintf(c.options[1], BB_OPTION_LEN, "No");
  c.option_count = 2;
  snprintf(c.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return c;
}

/* ---- tests: channel separation ---------------------------------------------- */

/* CREATE is an admin op; sent over the voter TCP+tetrissh channel it must be
 * refused - this is the actual enforcement of "only ballotctl manages
 * elections", not a permission check inside the domain logic. */
static int test_voter_handshake_and_wrong_channel_rejected(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  session_t cli;
  int fd = voter_connect(&cli);
  CHECK(fd >= 0, "voter handshake failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  req.config = valid_config("Should Be Rejected");

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[SESSION_MAX_FRAME];
  htttp_response_t http;
  CHECK(voter_send_recv(&cli, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "roundtrip");
  CHECK(http.status == 400, "CREATE over the voter channel must be rejected");

  session_close(&cli);
  close(fd);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* JOIN is a voter op; sent over the admin channel it must be refused too -
 * the same enforcement, the other direction. */
static int test_ctl_rejects_voter_op(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_JOIN;
  snprintf(req.election_id, BB_ID_LEN, "E-100");
  snprintf(req.cert_name, BB_CERT_LEN, "alice");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode JOIN");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);
  CHECK(http.status == 400, "JOIN over the admin channel must be rejected");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- tests: real dispatch through to libballotbrain -------------------------- */

static int test_ctl_create_ok(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  req.config = valid_config("Officers 2026");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_OK, "CREATE should succeed");
  CHECK(resp.election.id[0] != '\0', "election id should be set");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Pure-logic path (bb_validate_config), no DB dependency - deterministic
 * today regardless of the frozen DB seam. */
static int test_ctl_create_invalid_config(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  /* title left empty on purpose */
  snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = 2;
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");

  uint8_t wire[CTL_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

  uint8_t rbuf[CTL_MAX_FRAME];
  htttp_response_t http;
  CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
  close(fd);

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_ERR_CONFIG_TITLE, "empty title should be refused");
  CHECK(resp.election.id[0] == '\0', "no election id on a failed create");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Documents today's correct, expected behaviour: GET_ELECTION is a stubbed
 * read (db.c returns BB_ERR_NOT_IMPLEMENTED), so JOIN on any election id
 * comes back this way until the frozen DB work lands - not a bug. */
static int test_voter_join_not_implemented(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  session_t cli;
  int fd = voter_connect(&cli);
  CHECK(fd >= 0, "voter handshake failed");

  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_JOIN;
  snprintf(req.election_id, BB_ID_LEN, "E-100");
  snprintf(req.cert_name, BB_CERT_LEN, "alice");

  uint8_t wire[SESSION_MAX_FRAME];
  uint32_t wlen = sizeof wire;
  CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode JOIN");

  uint8_t rbuf[SESSION_MAX_FRAME];
  htttp_response_t http;
  CHECK(voter_send_recv(&cli, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "roundtrip");

  bcl_response_t resp;
  CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
  CHECK(resp.status == BB_ERR_NOT_IMPLEMENTED, "GET_ELECTION is still a stubbed read");

  session_close(&cli);
  close(fd);
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* Two CREATEs, two fresh ctl connections, one daemon: distinct allocated
 * ids proves both hit the SAME admin_thread / bb_ctx, not two independent
 * ones - the property the whole single-admin-thread design exists for. */
static int test_two_creates_share_admin_thread(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  char ids[2][BB_ID_LEN];

  for (int i = 0; i < 2; i++) {
    int fd = ctl_connect();
    CHECK(fd >= 0, "ctl connect failed");

    bcl_request_t req;
    memset(&req, 0, sizeof req);
    req.op = BCL_CREATE;
    snprintf(req.cert_name, BB_CERT_LEN, "admin");
    req.config = valid_config(i == 0 ? "First" : "Second");

    uint8_t wire[CTL_MAX_FRAME];
    uint32_t wlen = sizeof wire;
    CHECK(bcl_encode_request(&req, wire, &wlen) == 0, "encode CREATE");

    uint8_t rbuf[CTL_MAX_FRAME];
    htttp_response_t http;
    CHECK(ctl_send_recv(fd, wire, wlen, rbuf, sizeof rbuf, &http) == 0, "ctl roundtrip");
    close(fd);

    bcl_response_t resp;
    CHECK(bcl_decode_response(&http, &resp) == 0, "decode response");
    CHECK(resp.status == BB_OK, "CREATE should succeed");
    snprintf(ids[i], BB_ID_LEN, "%s", resp.election.id);
  }

  CHECK(strcmp(ids[0], ids[1]) != 0, "two CREATEs must get distinct ids from one shared bb_ctx");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* ---- tests: hostile / malformed input ---------------------------------------- */

static int test_ctl_malformed_http_gets_400(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  const char *junk = "not an htttp request at all";
  CHECK(ctl_frame_write(fd, (const uint8_t *)junk, (uint32_t)strlen(junk)) == 0, "send junk");

  uint8_t rbuf[CTL_MAX_FRAME];
  uint32_t rlen = 0;
  CHECK(ctl_frame_read(fd, rbuf, sizeof rbuf, &rlen) == 0, "expected a reply frame");
  htttp_response_t http;
  CHECK(htttp_parse_response(rbuf, rlen, &http) == HTTTP_OK, "the reply itself must be well-formed");
  CHECK(http.status == 400, "malformed body should get 400");
  close(fd);

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  return 0;
}

/* A frame whose declared length exceeds CTL_MAX_FRAME is refused before any
 * of it is read as HTTTP; the connection is just closed, no reply - the
 * daemon (and ctl_thread) must survive it either way. */
static int test_ctl_oversized_frame_closed_without_reply(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  int fd = ctl_connect();
  CHECK(fd >= 0, "ctl connect failed");

  uint8_t prefix[4] = {0xFF, 0xFF, 0xFF, 0xFF}; /* hand-written: ctl_frame_write
                                                  * would refuse to build this */
  CHECK(write(fd, prefix, sizeof prefix) == (ssize_t)sizeof prefix, "send oversized prefix");

  uint8_t rbuf[CTL_MAX_FRAME];
  uint32_t rlen = 0;
  CHECK(ctl_frame_read(fd, rbuf, sizeof rbuf, &rlen) != 0, "must not reply to an oversized frame");
  close(fd);

  CHECK(stop_ballotd(pid) == 0, "daemon must survive an oversized frame");
  return 0;
}

/* ---- tests: lifecycle --------------------------------------------------------- */

static int test_sigterm_shutdown_idle(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");
  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero");
  CHECK(access(CTL_PATH, F_OK) != 0, "ctl socket file should be removed on shutdown");
  return 0;
}

/* A worker (bin/ballot_session) is still attached, mid-handshake-complete
 * but idle, when the daemon is asked to stop - it must still exit cleanly
 * and reap the worker (admin_teardown's kill+reap loop). */
static int test_sigterm_shutdown_with_worker_attached(void) {
  pid_t pid = start_ballotd();
  CHECK(pid > 0, "daemon did not start");

  session_t cli;
  int fd = voter_connect(&cli);
  CHECK(fd >= 0, "voter handshake failed");

  CHECK(stop_ballotd(pid) == 0, "daemon exited non-zero with a worker still attached");

  session_close(&cli);
  close(fd);
  return 0;
}

/* ---- harness ------------------------------------------------------------------ */

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
    fprintf(stderr, "test_ballotd: %s not built (run make)\n", BALLOTD_BIN);
    return 1;
  }
  if (stat("bin/ballot_session", &st) != 0) {
    fprintf(stderr, "test_ballotd: bin/ballot_session not built (run make)\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  printf("ballotd end-to-end tests\n");
  run("voter handshake + wrong-channel CREATE rejected", test_voter_handshake_and_wrong_channel_rejected);
  run("ctl channel rejects voter op (JOIN)", test_ctl_rejects_voter_op);
  run("ctl CREATE succeeds", test_ctl_create_ok);
  run("ctl CREATE invalid config refused", test_ctl_create_invalid_config);
  run("voter JOIN: not-implemented (stub DB read)", test_voter_join_not_implemented);
  run("two CREATEs share one admin thread", test_two_creates_share_admin_thread);
  run("ctl malformed HTTTP gets 400", test_ctl_malformed_http_gets_400);
  run("ctl oversized frame closed, no reply", test_ctl_oversized_frame_closed_without_reply);
  run("SIGTERM shutdown while idle", test_sigterm_shutdown_idle);
  run("SIGTERM shutdown with a worker attached", test_sigterm_shutdown_with_worker_attached);

  unlink(CTL_PATH);
  printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
  return tests_failed == 0 ? 0 : 1;
}
