/*
 * Path-complete system tests for README UC-1 through UC-8.
 *
 * This executable is intentionally strict. It runs the public libballotclient
 * API against real ballotd, ballot_session, tetrisauth, tetriSH, and SimpleDB
 * processes. Missing runtime dependencies are failures, never skips.
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
#include "libtetrisdb/socket/conf.h"
#include "libtetrisdb/socket/runner.h"

#define BALLOTD_BIN "bin/ballotd"
#define BALLOT_SESSION_BIN "bin/ballot_session"
#define TEST_JAR "db/dist/simpledb.jar"
#define CA_PATH "auth/cacsertificate.crt"
#define CTL_PATH "var/run/test_system_e2e.ctl"
#define LOG_PATH "var/run/test_system_e2e.log"
#define HOST "127.0.0.1"
#define TEST_PORT 17679
#define TEST_PASSWORD "correcthorsebatterystaple"

static pid_t g_ballotd = -1;
static int g_run;
static int g_failed;

#define REQUIRE(cond, fmt, ...)                                                \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "    expected: " fmt "\n", ##__VA_ARGS__);             \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static void nap(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static int wait_for_tcp(void) {
  for (int i = 0; i < 300; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof addr);
      addr.sin_family = AF_INET;
      addr.sin_port = htons(TEST_PORT);
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

static int command_available(const char *command, const char *arg) {
  pid_t pid = fork();
  if (pid < 0) return 0;
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    execlp(command, command, arg, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int start_runner(void) {
  if (db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_ELECTION, BB_DB_SCHEMA_ELECTION) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_OPTION, BB_DB_SCHEMA_OPTION) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_ELIGIBLE, BB_DB_SCHEMA_ELIGIBLE) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_BALLOT, BB_DB_SCHEMA_BALLOT) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_OWNER, BB_DB_SCHEMA_OWNER) != 0 ||
      db_ensure_table(DB_DEFAULT_DIR, BB_DB_TABLE_NONCE, BB_DB_SCHEMA_NONCE) != 0)
    return -1;
  (void)system("./bin/tetrisdb start >/dev/null 2>&1");
  db_socket_opts_t opts;
  db_socket_opts_load(&opts);
  for (int i = 0; i < 100; i++) {
    db_socket_t *probe = db_socket_open(&opts);
    if (probe != NULL) {
      db_socket_close(probe);
      return 0;
    }
    nap(100);
  }
  return -1;
}

static int start_ballotd(void) {
  unlink(CTL_PATH);
  unlink(LOG_PATH);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0 || dup2(log_fd, STDERR_FILENO) < 0) _exit(126);
    close(log_fd);
    char port[16];
    snprintf(port, sizeof port, "%d", TEST_PORT);
    execl(BALLOTD_BIN, BALLOTD_BIN, "-p", port, "-C", CTL_PATH, "-d", DB_DEFAULT_DIR, "-i",
          DB_DEFAULT_IPC, (char *)NULL);
    _exit(127);
  }
  g_ballotd = pid;
  return wait_for_tcp();
}

static void cleanup(void) {
  if (g_ballotd > 0) {
    kill(g_ballotd, SIGTERM);
    waitpid(g_ballotd, NULL, 0);
    g_ballotd = -1;
  }
  unlink(CTL_PATH);
  unlink(LOG_PATH);
}

static bcl_ctx *admin_client(void) {
  bcl_ctx *ctx = bcl_create();
  if (ctx != NULL) bcl_set_ctl_path(ctx, CTL_PATH);
  return ctx;
}

static bcl_request_t create_request(const char *title, int option_count, const char *eligible) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = BCL_CREATE;
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  snprintf(req.config.title, BB_TITLE_LEN, "%s", title);
  if (option_count > 0) snprintf(req.config.options[0], BB_OPTION_LEN, "Yes");
  if (option_count > 1) snprintf(req.config.options[1], BB_OPTION_LEN, "No");
  req.config.option_count = option_count;
  if (eligible != NULL) {
    snprintf(req.config.eligible[0], BB_CERT_LEN, "%s", eligible);
    req.config.eligible_count = 1;
  }
  snprintf(req.config.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(req.config.close_time, BB_TIME_LEN, "2026-01-02T00:00:00Z");
  return req;
}

static bb_result_t send_admin(bcl_ctx *ctx, bcl_op_t op, const char *id,
                              bcl_response_t *response) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = op;
  if (id != NULL) snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.cert_name, BB_CERT_LEN, "admin");
  memset(response, 0, sizeof *response);
  return bcl_send(ctx, &req, response);
}

static int create_election(bcl_ctx *admin, const char *title, const char *eligible,
                           char id[BB_ID_LEN]) {
  bcl_request_t req = create_request(title, 2, eligible);
  bcl_response_t resp;
  memset(&resp, 0, sizeof resp);
  if (bcl_send(admin, &req, &resp) != BB_OK) return -1;
  snprintf(id, BB_ID_LEN, "%s", resp.election.id);
  return id[0] == '\0' ? -1 : 0;
}

static bcl_ctx *voter_client(const char *username) {
  bcl_ctx *ctx = bcl_create();
  if (ctx == NULL) return NULL;
  if (bcl_connect(ctx, HOST, TEST_PORT, CA_PATH) != BB_OK) goto fail;
  int status = 0;
  if (bcl_auth(ctx, "LOGIN", username, TEST_PASSWORD, &status) == 0 && status == 200) return ctx;
  if (status == 404 && bcl_auth(ctx, "REGISTER", username, TEST_PASSWORD, &status) == 0 &&
      status == 200)
    return ctx;
fail:
  bcl_disconnect(ctx);
  bcl_destroy(ctx);
  return NULL;
}

static void close_voter(bcl_ctx *ctx) {
  if (ctx == NULL) return;
  bcl_disconnect(ctx);
  bcl_destroy(ctx);
}

static bb_result_t voter_request(bcl_ctx *ctx, bcl_op_t op, const char *id, const char *user,
                                 const char *hash, bcl_response_t *resp) {
  bcl_request_t req;
  memset(&req, 0, sizeof req);
  req.op = op;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  if (user != NULL) snprintf(req.cert_name, BB_CERT_LEN, "%s", user);
  if (hash != NULL) snprintf(req.hash, BB_HASH_LEN, "%s", hash);
  memset(resp, 0, sizeof *resp);
  return bcl_send(ctx, &req, resp);
}

static int test_uc1_create_paths(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  bcl_response_t resp;
  char id[BB_ID_LEN];
  REQUIRE(create_election(admin, "UC1 happy", "e2ealice", id) == 0, "valid create succeeds");
  REQUIRE(send_admin(admin, BCL_OPEN, id, &resp) == BB_OK, "DRAFT opens to OPEN");

  struct {
    const char *name;
    int options;
    const char *open_time;
    const char *close_time;
    bb_result_t expected;
  } invalid[] = {{"", 2, NULL, NULL, BB_ERR_CONFIG_TITLE},
                 {"zero options", 0, NULL, NULL, BB_ERR_CONFIG_OPTIONS},
                 {"one option", 1, NULL, NULL, BB_ERR_CONFIG_OPTIONS},
                 {"equal times", 2, "2026-01-01T00:00:00Z", "2026-01-01T00:00:00Z",
                  BB_ERR_CONFIG_TIME},
                 {"reversed times", 2, "2026-01-03T00:00:00Z", "2026-01-02T00:00:00Z",
                  BB_ERR_CONFIG_TIME}};
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
    bcl_request_t req = create_request(invalid[i].name, invalid[i].options, "e2ealice");
    if (invalid[i].open_time != NULL) {
      snprintf(req.config.open_time, BB_TIME_LEN, "%s", invalid[i].open_time);
      snprintf(req.config.close_time, BB_TIME_LEN, "%s", invalid[i].close_time);
    }
    memset(&resp, 0, sizeof resp);
    REQUIRE(bcl_send(admin, &req, &resp) == invalid[i].expected,
            "UC-1 invalid partition %zu returns its documented error", i + 1);
    REQUIRE(resp.election.id[0] == '\0', "UC-1 invalid partition %zu creates no election", i + 1);
  }

  bcl_request_t duplicate = create_request("duplicate", 2, "e2ealice");
  snprintf(duplicate.election_id, BB_ID_LEN, "%s", id);
  REQUIRE(bcl_send(admin, &duplicate, &resp) == BB_ERR_CONFIG_ID_TAKEN,
          "duplicate requested id is refused");
  REQUIRE(send_admin(admin, BCL_CLOSE, id, &resp) == BB_OK,
          "duplicate refusal leaves original OPEN election unchanged");
  bcl_destroy(admin);
  return 0;
}

static int prepare_state(bcl_ctx *admin, bb_state_t state, const char *user, char id[BB_ID_LEN]) {
  bcl_response_t resp;
  if (create_election(admin, "state fixture", user, id) != 0) return -1;
  if (state >= BB_STATE_OPEN && send_admin(admin, BCL_OPEN, id, &resp) != BB_OK) return -1;
  if (state >= BB_STATE_CLOSED && send_admin(admin, BCL_CLOSE, id, &resp) != BB_OK) return -1;
  if (state >= BB_STATE_PUBLISHED && send_admin(admin, BCL_PUBLISH, id, &resp) != BB_OK) return -1;
  return 0;
}

static int test_uc2_join_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "admin and authenticated voter clients");
  char id[BB_ID_LEN];
  bcl_response_t resp;
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED, "eligible OPEN join");
  REQUIRE(session.joined && strcmp(session.title, "state fixture") == 0 && session.option_count == 2,
          "join exposes title and options");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, "E-DOES-NOT-EXIST", "e2ealice") == BU_JOIN_NOT_FOUND,
          "unknown election is not found");
  REQUIRE(!session.joined, "unknown election creates no joined session");

  char ineligible_id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "somebodyelse", ineligible_id) == 0,
          "ineligible fixture");
  REQUIRE(bu_join(alice, &session, ineligible_id, "e2ealice") == BU_JOIN_NOT_ELIGIBLE,
          "unlisted authenticated voter is refused");
  REQUIRE(!session.joined, "ineligible refusal returns no session");

  for (bb_state_t state = BB_STATE_DRAFT; state <= BB_STATE_PUBLISHED; state++) {
    if (state == BB_STATE_OPEN) continue;
    char state_id[BB_ID_LEN];
    REQUIRE(prepare_state(admin, state, "e2ealice", state_id) == 0, "state fixture %d", state);
    memset(&session, 0, sizeof session);
    REQUIRE(bu_join(alice, &session, state_id, "e2ealice") == BU_JOIN_NOT_OPEN,
            "DRAFT/CLOSED/PUBLISHED join returns not-open for state %d", state);
    REQUIRE(!session.joined, "non-OPEN state %d creates no joined session", state);
  }

  bcl_ctx *down = bcl_create();
  REQUIRE(down != NULL, "unreachable client allocation");
  REQUIRE(bcl_connect(down, HOST, TEST_PORT + 100, CA_PATH) == BB_ERR_DB,
          "unreachable ballotd fails within the transport deadline");
  bcl_destroy(down);
  (void)resp;
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int test_uc3_uc4_vote_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  bb_receipt_t first, second;
  memset(&first, 0, sizeof first);
  memset(&second, 0, sizeof second);
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc3-not-joined", NULL) == BB_ERR_NOT_JOINED,
          "UC-3/4 not-joined submission is refused locally");
  REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED, "join");
  REQUIRE(bu_route_vote(&session) == BU_CAST, "no prior ballot routes to cast");
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc3-first-cast", &first) == BB_OK,
          "valid cast returns receipt");
  REQUIRE(first.hash[0] != '\0' && session.ballot_version == 1, "cast persists version one");
  REQUIRE(bu_route_vote(&session) == BU_UPDATE, "prior ballot routes to update");
  REQUIRE(bu_submit_vote(alice, &session, 1, "uc4-valid-update", &second) == BB_OK,
          "valid update returns receipt");
  REQUIRE(strcmp(first.hash, second.hash) != 0 && session.ballot_version == 2,
          "update produces a fresh version-two receipt");

  bcl_disconnect(alice);
  bcl_destroy(alice);
  alice = voter_client("e2ealice");
  REQUIRE(alice != NULL, "reconnected voter");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED,
          "returning voter rejoins");
  REQUIRE(session.has_ballot && session.ballot_version == 2 && bu_route_vote(&session) == BU_UPDATE,
          "rejoin recovers authoritative prior-ballot version");

  char closing_id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", closing_id) == 0,
          "close-mid-submit fixture");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, closing_id, "e2ealice") == BU_JOIN_ADMITTED,
          "selecting voter joins before close");
  bcl_response_t resp;
  REQUIRE(send_admin(admin, BCL_CLOSE, closing_id, &resp) == BB_OK,
          "admin closes between selection and submission");
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc3-after-close", NULL) == BB_ERR_CLOSED,
          "cast arriving after close is rejected");
  REQUIRE(!session.has_ballot, "rejected cast does not move client ballot state");

  char update_id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", update_id) == 0,
          "closed-mid-update fixture");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, update_id, "e2ealice") == BU_JOIN_ADMITTED, "update join");
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc4-before-close", &first) == BB_OK,
          "prior ballot before close");
  REQUIRE(send_admin(admin, BCL_CLOSE, update_id, &resp) == BB_OK,
          "admin closes before update reaches server");
  REQUIRE(bu_submit_vote(alice, &session, 1, "uc4-after-close", NULL) == BB_ERR_CLOSED,
          "update arriving after close is rejected");
  REQUIRE(session.ballot_version == 1 && strcmp(session.my_hash, first.hash) == 0,
          "rejected update preserves the prior client receipt");
  REQUIRE(voter_request(alice, BCL_CHECK, update_id, "e2ealice", first.hash, &resp) == BB_OK &&
              resp.found_option == 0,
          "rejected update leaves the prior stored ballot live");

  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int log_has_voter_choice_link(const char *voter) {
  FILE *log = fopen(LOG_PATH, "r");
  if (log == NULL) return -1;
  char line[1024];
  int linked = 0;
  while (fgets(line, sizeof line, log) != NULL) {
    if (strstr(line, voter) != NULL && strstr(line, "option=") != NULL) {
      linked = 1;
      break;
    }
  }
  fclose(log);
  return linked;
}

static int test_uc3_log_secrecy(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *voter = voter_client("secrecyvoter");
  REQUIRE(admin != NULL && voter != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "secrecyvoter", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "secrecyvoter") == BU_JOIN_ADMITTED, "join");
  REQUIRE(bu_submit_vote(voter, &session, 1, "secrecy-system-cast", NULL) == BB_OK,
          "successful system cast");
  REQUIRE(log_has_voter_choice_link("secrecyvoter") == 0,
          "no system log record links voter identity to option");
  close_voter(voter);
  bcl_destroy(admin);
  return 0;
}

static int test_uc5_results_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  bcl_response_t resp;
  for (bb_state_t state = BB_STATE_DRAFT; state <= BB_STATE_CLOSED; state++) {
    char id[BB_ID_LEN];
    REQUIRE(prepare_state(admin, state, "e2ealice", id) == 0, "results state fixture %d", state);
    REQUIRE(voter_request(alice, BCL_RESULTS, id, "e2ealice", NULL, &resp) ==
                BB_ERR_NOT_PUBLISHED,
            "results unavailable in state %d", state);
    REQUIRE(resp.option_count == 0 && resp.hash_count == 0,
            "unavailable results expose no tally or hashes in state %d", state);
  }

  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_PUBLISHED, "e2ealice", id) == 0, "published fixture");
  REQUIRE(voter_request(alice, BCL_RESULTS, id, "e2ealice", NULL, &resp) == BB_OK,
          "eligible observer sees published results");
  REQUIRE(resp.option_count == 2 && strcmp(resp.election.title, "state fixture") == 0,
          "observer sees exact title and option set");
  REQUIRE(send_admin(admin, BCL_ADMIN_RESULTS, id, &resp) == BB_OK,
          "local admin sees results without voter membership");

  char restricted[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_PUBLISHED, "somebodyelse", restricted) == 0,
          "restricted published fixture");
  REQUIRE(voter_request(alice, BCL_RESULTS, restricted, "e2ealice", NULL, &resp) ==
              BB_ERR_NOT_ELIGIBLE,
          "ineligible observer is refused");
  REQUIRE(resp.option_count == 0 && resp.hash_count == 0,
          "ineligible observer receives no tally or hashes");
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static int test_uc6_check_paths(void) {
  bcl_ctx *admin = admin_client();
  bcl_ctx *alice = voter_client("e2ealice");
  REQUIRE(admin != NULL && alice != NULL, "clients");
  char id[BB_ID_LEN];
  REQUIRE(prepare_state(admin, BB_STATE_OPEN, "e2ealice", id) == 0, "OPEN fixture");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(alice, &session, id, "e2ealice") == BU_JOIN_ADMITTED, "join");
  bb_receipt_t old_receipt, latest_receipt;
  REQUIRE(bu_submit_vote(alice, &session, 0, "uc6-old", &old_receipt) == BB_OK, "first vote");
  bcl_response_t resp;
  REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", old_receipt.hash, &resp) == BB_OK &&
              resp.found && resp.found_option == 0,
          "live key verifies before publication with recorded choice");
  REQUIRE(bu_submit_vote(alice, &session, 1, "uc6-latest", &latest_receipt) == BB_OK, "update");
  REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", old_receipt.hash, &resp) ==
              BB_ERR_NOT_FOUND &&
              !resp.found,
          "superseded key has the same miss outcome as an unknown key");
  REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice",
                        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", &resp) ==
              BB_ERR_NOT_FOUND &&
              !resp.found,
          "unknown key exposes no ballot");
  REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", latest_receipt.hash, &resp) == BB_OK &&
              resp.found_option == 1,
          "latest key resolves to updated choice");
  REQUIRE(send_admin(admin, BCL_CLOSE, id, &resp) == BB_OK, "close");
  REQUIRE(send_admin(admin, BCL_PUBLISH, id, &resp) == BB_OK, "publish");
  REQUIRE(voter_request(alice, BCL_CHECK, id, "e2ealice", latest_receipt.hash, &resp) == BB_OK &&
              resp.found,
          "live key verifies after publication");
  close_voter(alice);
  bcl_destroy(admin);
  return 0;
}

static bcl_op_t transition_op(bb_state_t state) {
  if (state == BB_STATE_OPEN) return BCL_OPEN;
  if (state == BB_STATE_CLOSED) return BCL_CLOSE;
  return BCL_PUBLISH;
}

static int test_uc7_uc8_transition_matrix(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  for (bb_state_t from = BB_STATE_DRAFT; from <= BB_STATE_PUBLISHED; from++) {
    for (bb_state_t to = BB_STATE_OPEN; to <= BB_STATE_PUBLISHED; to++) {
      char id[BB_ID_LEN];
      REQUIRE(prepare_state(admin, from, "e2ealice", id) == 0, "source state %d", from);
      bcl_response_t resp;
      bb_result_t actual = send_admin(admin, transition_op(to), id, &resp);
      bb_result_t expected = (to == from + 1) ? BB_OK : BB_ERR_ILLEGAL_TRANSITION;
      REQUIRE(actual == expected, "transition %d -> %d returns %d, got %d", from, to, expected,
              actual);
      if (actual == BB_ERR_ILLEGAL_TRANSITION && from < BB_STATE_PUBLISHED) {
        REQUIRE(send_admin(admin, transition_op((bb_state_t)(from + 1)), id, &resp) == BB_OK,
                "illegal transition %d -> %d leaves state unchanged", from, to);
      }
    }
  }
  bcl_destroy(admin);
  return 0;
}

static int test_complete_election_journey(void) {
  bcl_ctx *admin = admin_client();
  REQUIRE(admin != NULL, "admin client");
  char id[BB_ID_LEN];
  REQUIRE(create_election(admin, "Complete journey", "journeyvoter", id) == 0, "create DRAFT");
  bcl_response_t resp;
  REQUIRE(send_admin(admin, BCL_OPEN, id, &resp) == BB_OK, "open");
  bcl_ctx *voter = voter_client("journeyvoter");
  REQUIRE(voter != NULL, "authenticated voter");
  bu_session_t session;
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "journeyvoter") == BU_JOIN_ADMITTED, "join");
  bb_receipt_t first, second;
  REQUIRE(bu_submit_vote(voter, &session, 0, "journey-first", &first) == BB_OK, "cast");
  close_voter(voter);

  voter = voter_client("journeyvoter");
  REQUIRE(voter != NULL, "reconnect and authenticate");
  memset(&session, 0, sizeof session);
  REQUIRE(bu_join(voter, &session, id, "journeyvoter") == BU_JOIN_ADMITTED &&
              bu_route_vote(&session) == BU_UPDATE,
          "rejoin recovers prior ballot and routes update");
  REQUIRE(bu_submit_vote(voter, &session, 1, "journey-second", &second) == BB_OK, "update");
  REQUIRE(strcmp(first.hash, second.hash) != 0, "receipts differ");
  REQUIRE(send_admin(admin, BCL_CLOSE, id, &resp) == BB_OK, "close");
  REQUIRE(send_admin(admin, BCL_PUBLISH, id, &resp) == BB_OK, "publish");
  REQUIRE(voter_request(voter, BCL_RESULTS, id, "journeyvoter", NULL, &resp) == BB_OK,
          "view published results");
  REQUIRE(resp.tally[0] == 0 && resp.tally[1] == 1 && resp.hash_count == 1,
          "exact tally counts only the latest ballot");
  REQUIRE(strcmp(resp.hashes[0].hash, second.hash) == 0, "published hash is latest receipt");
  REQUIRE(voter_request(voter, BCL_CHECK, id, "journeyvoter", first.hash, &resp) ==
              BB_ERR_NOT_FOUND,
          "first receipt is superseded");
  REQUIRE(voter_request(voter, BCL_CHECK, id, "journeyvoter", second.hash, &resp) == BB_OK &&
              resp.found_option == 1,
          "latest receipt remains live");
  close_voter(voter);
  bcl_destroy(admin);
  return 0;
}

static void run_case(const char *id, const char *name, int (*fn)(void)) {
  g_run++;
  printf("  %-9s %-58s", id, name);
  fflush(stdout);
  if (fn() == 0) {
    printf("ok\n");
  } else {
    g_failed++;
    printf("FAILED\n");
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  struct stat st;
  if (stat(BALLOTD_BIN, &st) != 0 || stat(BALLOT_SESSION_BIN, &st) != 0) {
    fprintf(stderr, "system E2E: required ballot executables are not built\n");
    return 1;
  }
  if (access(TEST_JAR, R_OK) != 0) {
    fprintf(stderr, "system E2E: required %s is unavailable\n", TEST_JAR);
    return 1;
  }
  if (!command_available("java", "-version")) {
    fprintf(stderr, "system E2E: java is unavailable\n");
    return 1;
  }
  if (start_runner() != 0) {
    fprintf(stderr, "system E2E: SimpleDB did not become reachable\n");
    return 1;
  }
  if (start_ballotd() != 0) {
    fprintf(stderr, "system E2E: ballotd did not become reachable\n");
    cleanup();
    return 1;
  }

  printf("path-complete system E2E tests\n");
  run_case("UC1-E2E", "create, validation, duplicate-id, and open paths", test_uc1_create_paths);
  run_case("UC2-E2E", "join happy, unreachable, missing, eligibility, and state paths",
           test_uc2_join_paths);
  run_case("UC3/4-E2E", "cast, routing, update, reconnect, and close-race paths",
           test_uc3_uc4_vote_paths);
  run_case("UC3-LOG", "successful cast logs no voter-to-choice link", test_uc3_log_secrecy);
  run_case("UC5-E2E", "observer/admin results, eligibility, and publication gates",
           test_uc5_results_paths);
  run_case("UC6-E2E", "pre/post publish, unknown, superseded, and latest checks",
           test_uc6_check_paths);
  run_case("UC7/8-E2E", "complete legal and illegal lifecycle transition matrix",
           test_uc7_uc8_transition_matrix);
  run_case("JOURNEY-E2E", "create through latest-receipt verification",
           test_complete_election_journey);

  cleanup();
  printf("%d/%d system groups passed, 0 skipped\n", g_run - g_failed, g_run);
  return g_failed == 0 ? 0 : 1;
}
