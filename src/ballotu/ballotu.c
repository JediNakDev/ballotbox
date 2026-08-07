/*
 * ballotu.c - the real voter client entry point.
 *
 * Replaces the demo mock (main.c/mock.c/mock.h/screens.c, left on disk
 * untouched) with a client that actually talks to ballotd: TCP + tetrissh
 * to the voter channel, the shared HTTTP codec, and libballotclient's
 * bu_join/bu_submit_vote session flows. One self-contained file rather than
 * a main.c + screens.c split, so the old files and this one can never be
 * wildcarded together into the same binary by accident.
 *
 * IMPORTANT, read before running this against a real ballotd: the database
 * seam (db_exec) is still a stub - every read (GET_ELECTION, GET_TALLY,
 * FIND_HASH, ...) returns BB_ERR_NOT_IMPLEMENTED. JOIN needs a read before
 * it can do anything else, so today EVERY join attempt fails that way -
 * this is the frozen DB boundary working as intended, not a bug here. Once
 * that lands, nothing in this file needs to change.
 *
 * Also: bb_verify_cert (the daemon's cert-identity seam) is a placeholder
 * that accepts any non-empty name, and this channel authenticates the
 * SERVER to the client (tetrissh), not the other way around - so login here
 * cannot reject a bad cert name the way the old mock did. That check has
 * genuinely moved to JOIN in the real protocol; it just cannot succeed yet
 * either, for the reason above.
 */

#include "libballotclient/voter.h"
#include "libtetrisui/tetrisui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7676
#define DEFAULT_CA_PATH "auth/cacsertificate.crt"

static bcl_ctx *g_ctx;
static bu_session_t g_session;

static char g_host[64];
static int g_port;
static char g_ca_path[512];

/* ---- bb_result_t -> voter-facing text -------------------------------- */

static const char *result_text(bb_result_t rc) {
  switch (rc) {
    case BB_OK:
      return "OK";
    case BB_ERR_NOT_OPEN:
      return "The election is not open.";
    case BB_ERR_CLOSED:
      return "Election closed mid-submit. Rejected by System.";
    case BB_ERR_NOT_ELIGIBLE:
      return "Your cert is not on the eligible-voter list.";
    case BB_ERR_CERT_INVALID:
      return "Cert rejected.";
    case BB_ERR_CERT_EXPIRED:
      return "Cert expired or forged.";
    case BB_ERR_REPLAY:
      return "That ballot was already submitted (replay).";
    case BB_ERR_BAD_OPTION:
      return "Selected option was out of range.";
    case BB_ERR_DECRYPT:
      return "Ballot could not be decrypted.";
    case BB_ERR_NOT_PUBLISHED:
      return "Results not available.";
    case BB_ERR_NOT_FOUND:
      return "Not found.";
    case BB_ERR_NOT_IMPLEMENTED:
      return "The backend storage is not wired up yet - try again once it is.";
    case BB_ERR_DB:
      return "Could not reach ballotd.";
    default:
      return "Rejected by System.";
  }
}

/* ---- login / connect ---------------------------------------------------- */

static int screen_login(void) {
  for (;;) {
    char name[BB_CERT_LEN] = "";
    if (tetrisui_input("ballotu - login", "Enter your cert name (Esc to quit):", name,
                       sizeof(name)) != 0) {
      return 0;
    }
    if (strlen(name) == 0) continue;

    const char *steps[] = {"Opening secure session", "Presenting cert (tetrissh handshake)"};
    tetrisui_progress_begin("Authenticating", steps, 2);
    bb_result_t rc = bcl_connect(g_ctx, g_host, g_port, g_ca_path);
    /* One real round trip covers both steps - see client.h's bcl_connect
     * doc comment for why the two are not reported separately. */
    tetrisui_progress_step(0, rc == BB_OK);
    tetrisui_progress_step(1, rc == BB_OK);
    tetrisui_progress_end();

    if (rc != BB_OK) {
      char line[96];
      snprintf(line, sizeof(line), "Could not reach ballotd at %s:%d.", g_host, g_port);
      const char *lines[] = {line, "Check the server is running and try again."};
      tetrisui_message("Connection failed", lines, 2);
      continue;
    }

    memset(&g_session, 0, sizeof(g_session));
    snprintf(g_session.cert_name, BB_CERT_LEN, "%s", name);
    tetrisui_set_status("ballotu", name, "");
    return 1;
  }
}

/* ---- UC-2: join ----------------------------------------------------------- */

static void screen_join_election(void) {
  char id[BB_ID_LEN] = "";
  if (tetrisui_input("Join election (UC-2)", "Enter election ID (e.g. E-100):", id, sizeof(id)) !=
      0)
    return;
  if (strlen(id) == 0) return;

  const char *steps[] = {"Contacting ballotd"};
  tetrisui_progress_begin("Joining election", steps, 1);
  bu_join_outcome_t outcome = bu_join(g_ctx, &g_session, id, g_session.cert_name);
  tetrisui_progress_step(0, outcome != BU_JOIN_TIMEOUT);
  tetrisui_progress_end();

  switch (outcome) {
    case BU_JOIN_TIMEOUT: {
      const char *lines[] = {"Could not reach the election.",
                             result_text(BB_ERR_NOT_IMPLEMENTED)};
      tetrisui_message("Join failed", lines, 2);
      return;
    }
    case BU_JOIN_NOT_FOUND: {
      char line[64];
      snprintf(line, sizeof(line), "Election '%s' not found.", id);
      const char *lines[] = {line};
      tetrisui_message("Join failed", lines, 1);
      return;
    }
    case BU_JOIN_NOT_ELIGIBLE: {
      const char *lines[] = {"Your cert is not on the eligible-voter list",
                             "for this election. Refused."};
      tetrisui_message("Join refused", lines, 2);
      return;
    }
    case BU_JOIN_NOT_OPEN: {
      char line[96];
      snprintf(line, sizeof(line), "Cannot join %s: election is not Open.", id);
      const char *lines[] = {line, "Refused."};
      tetrisui_message("Join refused", lines, 2);
      return;
    }
    case BU_JOIN_ADMITTED:
      break;
  }

  if (g_session.has_ballot) {
    const char *lines[] = {"You already have a ballot for this election.",
                           "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
    return;
  }

  char lines_buf[BB_MAX_OPTIONS + 2][96];
  const char *lines[BB_MAX_OPTIONS + 2];
  snprintf(lines_buf[0], sizeof(lines_buf[0]), "Joined %s: %s", g_session.election_id,
          g_session.title);
  lines[0] = lines_buf[0];
  snprintf(lines_buf[1], sizeof(lines_buf[1]), "Ballot options:");
  lines[1] = lines_buf[1];
  for (int i = 0; i < g_session.option_count; i++) {
    snprintf(lines_buf[i + 2], sizeof(lines_buf[i + 2]), "  %d) %s", i + 1, g_session.options[i]);
    lines[i + 2] = lines_buf[i + 2];
  }
  tetrisui_message("Join successful", lines, g_session.option_count + 2);
}

/* ---- UC-3 / UC-4: cast / update -------------------------------------------- */

static void cast_common(int is_update) {
  if (!g_session.joined) {
    const char *lines[] = {"You must join an election first (UC-2)."};
    tetrisui_message("Not joined", lines, 1);
    return;
  }

  bu_vote_action_t action = bu_route_vote(&g_session);
  if (is_update && action == BU_CAST) {
    const char *lines[] = {"You have no prior ballot yet.", "Routing you to Cast Vote (UC-3)."};
    tetrisui_message("Nothing to update", lines, 2);
    is_update = 0;
  } else if (!is_update && action == BU_UPDATE) {
    const char *lines[] = {"You already have a final ballot.",
                           "Routing you to Update Vote (UC-4)."};
    tetrisui_message("Already voted", lines, 2);
    is_update = 1;
  }

  const char *items[BB_MAX_OPTIONS];
  for (int i = 0; i < g_session.option_count; i++) items[i] = g_session.options[i];

  char title[96];
  if (is_update) {
    snprintf(title, sizeof(title), "Update vote (prior ballot v%d exists)",
            g_session.ballot_version);
  } else {
    snprintf(title, sizeof(title), "Cast vote: %s", g_session.title);
  }
  int sel = tetrisui_menu(title, items, g_session.option_count, "Enter to select your option");
  if (sel < 0) return;

  char q[96];
  snprintf(q, sizeof(q), "Confirm vote for '%s'?", g_session.options[sel]);
  if (!tetrisui_confirm("Confirm ballot", q)) return;

  char nonce[BB_NONCE_LEN];
  snprintf(nonce, sizeof(nonce), "%08lx%08x", (unsigned long)time(NULL), rand());

  const char *steps[] = {"Encrypting ballot and submitting"};
  tetrisui_progress_begin(is_update ? "Submitting updated ballot" : "Submitting ballot", steps, 1);
  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof(receipt));
  bb_result_t rc = bu_submit_vote(g_ctx, &g_session, sel, nonce, &receipt);
  tetrisui_progress_step(0, rc == BB_OK);
  tetrisui_progress_end();

  if (rc != BB_OK) {
    const char *lines[] = {result_text(rc)};
    tetrisui_message("Vote rejected", lines, 1);
    return;
  }

  const char *lines[] = {
      is_update ? "Vote updated. New receipt issued:" : "Vote recorded. Receipt issued:",
      receipt.hash, "Keep this hash to check your vote later (UC-6)."};
  tetrisui_message("Success", lines, 3);
}

static void screen_cast_vote(void) { cast_common(0); }
static void screen_update_vote(void) { cast_common(1); }

/* ---- UC-5: results ---------------------------------------------------------- */

static void screen_view_results(void) {
  char id[BB_ID_LEN] = "";
  if (tetrisui_input("View results (UC-5)", "Enter election ID:", id, sizeof(id)) != 0) return;
  if (strlen(id) == 0) return;

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_RESULTS;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.cert_name, BB_CERT_LEN, "%s", g_session.cert_name);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  if (status != BB_OK) {
    const char *lines[] = {result_text(status)};
    tetrisui_message("Not published", lines, 1);
    return;
  }

  enum { COL_W = 66, MAX_LINES = BB_MAX_OPTIONS + BB_MAX_VOTERS + 6 };
  static char buf[MAX_LINES][BB_MAX_OPTIONS * COL_W + 1];
  const char *lines[MAX_LINES];
  int n = 0;

  snprintf(buf[n], sizeof(buf[n]), "%s", id);
  lines[n] = buf[n];
  n++;
  snprintf(buf[n], sizeof(buf[n]), "Tally:");
  lines[n] = buf[n];
  n++;
  for (int i = 0; i < resp.option_count; i++) {
    char bar[32];
    int fill = resp.tally[i] > 20 ? 20 : resp.tally[i];
    if (fill < 0) fill = 0;
    memset(bar, '#', (size_t)fill);
    bar[fill] = '\0';
    snprintf(buf[n], sizeof(buf[n]), "  %-10s %3d %s", resp.options[i], resp.tally[i], bar);
    lines[n] = buf[n];
    n++;
  }
  snprintf(buf[n], sizeof(buf[n]), "Ballot hashes (counted votes, one column per option):");
  lines[n] = buf[n];
  n++;

  /* Group counted (non-superseded) hashes by option, one column each -
   * same layout the old mock UI used. */
  int per[BB_MAX_OPTIONS][BB_MAX_VOTERS];
  int cnt[BB_MAX_OPTIONS] = {0};
  int max_cnt = 0;
  for (int i = 0; i < resp.hash_count; i++) {
    if (resp.hashes[i].superseded) continue;
    int o = resp.hashes[i].option_index;
    if (o < 0 || o >= BB_MAX_OPTIONS) continue;
    per[o][cnt[o]++] = i;
    if (cnt[o] > max_cnt) max_cnt = cnt[o];
  }
  int pos = 0;
  for (int o = 0; o < resp.option_count; o++)
    pos += snprintf(buf[n] + pos, sizeof(buf[n]) - (size_t)pos, "%-*s", COL_W, resp.options[o]);
  lines[n] = buf[n];
  n++;
  for (int r = 0; r < max_cnt; r++) {
    pos = 0;
    for (int o = 0; o < resp.option_count; o++)
      pos += snprintf(buf[n] + pos, sizeof(buf[n]) - (size_t)pos, "%-*s", COL_W,
                      r < cnt[o] ? resp.hashes[per[o][r]].hash : "");
    lines[n] = buf[n];
    n++;
  }
  tetrisui_list_view("Election results", lines, n);
}

/* ---- UC-6: check your vote -------------------------------------------------- */

static void screen_check_vote(void) {
  char key[64] = "";
  if (tetrisui_input("Check your vote (UC-6)", "Enter your secret ballot key:", key,
                     sizeof(key)) != 0)
    return;
  if (strlen(key) == 0) return;

  char id[BB_ID_LEN] = "";
  if (tetrisui_input("Check your vote (UC-6)", "Enter the election ID to check against:", id,
                     sizeof(id)) != 0)
    return;
  if (strlen(id) == 0) return;

  char hash[BB_HASH_LEN];
  memset(hash, 0, sizeof(hash));
  bu_derive_receipt(g_ctx, key, hash);

  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  req.op = BCL_CHECK;
  snprintf(req.election_id, BB_ID_LEN, "%s", id);
  snprintf(req.hash, BB_HASH_LEN, "%s", hash);

  bcl_response_t resp;
  memset(&resp, 0, sizeof(resp));
  bb_result_t rc = bcl_send(g_ctx, &req, &resp);
  bb_result_t status = (rc != BB_OK) ? rc : resp.status;

  char line1[96];
  snprintf(line1, sizeof(line1), "Receipt hash: %s", hash);

  bu_check_outcome_t outcome = bu_classify_check(status, resp.found);
  switch (outcome) {
    case BU_CHECK_COUNTED: {
      char line2[96];
      snprintf(line2, sizeof(line2), "Your vote is option index %d and is included in the tally.",
              resp.found_option);
      const char *lines[] = {line1, line2};
      tetrisui_message("Verified", lines, 2);
      return;
    }
    case BU_CHECK_DROPPED: {
      const char *lines[] = {line1, "Hash not found in the published tally.",
                             "Verification FAILED - dropped ballot.",
                             "Please raise this with the Admin."};
      tetrisui_message("Verification failed", lines, 4);
      return;
    }
    case BU_CHECK_UNAVAILABLE: {
      const char *lines[] = {line1, result_text(status)};
      tetrisui_message("Verification unavailable", lines, 2);
      return;
    }
  }
}

/* ---- entry point ------------------------------------------------------------ */

static void usage(FILE *out, const char *argv0) {
  fprintf(out,
          "usage: %s [-H host] [-p port] [-C ca_cert] [-h]\n"
          "  -H host     ballotd's voter-channel host, dotted-quad (default %s)\n"
          "  -p port     ballotd's voter-channel TCP port (default %d)\n"
          "  -C ca_cert  CA certificate PEM to verify ballotd against (default %s)\n"
          "  -h          show this help\n",
          argv0, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_CA_PATH);
}

int main(int argc, char **argv) {
  snprintf(g_host, sizeof(g_host), "%s", DEFAULT_HOST);
  g_port = DEFAULT_PORT;
  snprintf(g_ca_path, sizeof(g_ca_path), "%s", DEFAULT_CA_PATH);

  int opt;
  while ((opt = getopt(argc, argv, "H:p:C:h")) != -1) {
    switch (opt) {
      case 'H':
        snprintf(g_host, sizeof(g_host), "%s", optarg);
        break;
      case 'p':
        g_port = atoi(optarg);
        break;
      case 'C':
        snprintf(g_ca_path, sizeof(g_ca_path), "%s", optarg);
        break;
      case 'h':
        usage(stdout, argv[0]);
        return 0;
      default:
        usage(stderr, argv[0]);
        return 2;
    }
  }

  srand((unsigned)time(NULL));

  g_ctx = bcl_create();
  if (g_ctx == NULL) {
    fprintf(stderr, "ballotu: bcl_create failed\n");
    return 1;
  }

  tetrisui_init();
  tetrisui_set_status("ballotu", "(not logged in)", "");

  if (screen_login()) {
    const char *items[] = {"Join election (UC-2)", "Cast vote (UC-3)", "Update vote (UC-4)",
                           "View results (UC-5)", "Check your vote (UC-6)", "Quit"};
    for (;;) {
      int sel = tetrisui_menu("ballotu - voter menu", items, 6, NULL);
      if (sel < 0 || sel == 5) break;
      switch (sel) {
        case 0:
          screen_join_election();
          break;
        case 1:
          screen_cast_vote();
          break;
        case 2:
          screen_update_vote();
          break;
        case 3:
          screen_view_results();
          break;
        case 4:
          screen_check_vote();
          break;
      }
    }
  }

  tetrisui_shutdown();
  bcl_disconnect(g_ctx);
  bcl_destroy(g_ctx);
  return 0;
}
