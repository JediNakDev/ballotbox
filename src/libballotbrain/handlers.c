#include "libballotbrain/ballotbrain.h"

#include <string.h>

/*
 * System-class request handlers. Each orchestrates: read state -> check ->
 * write, with every outside dependency (storage, crypto, PKI) reached through
 * a seam. The rules are enforced here in full; whether the store behind
 * db_exec is SimpleDB or a test substitute is not this layer's concern.
 */

/* Fetch one election. Returns BB_ERR_NOT_FOUND when the store has no such row,
 * and propagates any other store failure unchanged. */
static bb_result_t load_election(bb_ctx *ctx, const char *election_id, bb_election_t *out) {
  bb_db_cmd_t get = {0};
  get.op = BB_DB_GET_ELECTION;
  snprintf(get.election_id, BB_ID_LEN, "%s", election_id);

  bb_db_result_t res;
  memset(&res, 0, sizeof(res));
  bb_result_t r = db_exec(ctx, &get, &res);
  if (r != BB_OK) {
    return r;
  }
  if (!res.found) {
    return BB_ERR_NOT_FOUND;
  }
  if (out != NULL) {
    *out = res.election;
  }
  return BB_OK;
}

/* ---- UC-1 -------------------------------------------------------------- */

bb_result_t bb_create_election(bb_ctx *ctx, const bb_config_t *config, char out_id[BB_ID_LEN]) {
  bb_result_t vr = bb_validate_config(config);
  if (vr != BB_OK) {
    bb_log(ctx, "[create] rejected config (err=%d)", vr);
    return vr;
  }

  char id[BB_ID_LEN];
  bb_alloc_id(ctx, id);

  bb_db_cmd_t cmd = {0};
  cmd.op = BB_DB_INSERT_ELECTION;
  snprintf(cmd.election_id, BB_ID_LEN, "%s", id);
  cmd.config = config;
  bb_result_t dr = db_exec(ctx, &cmd, NULL);
  if (dr != BB_OK) {
    return dr;
  }

  if (out_id != NULL) {
    snprintf(out_id, BB_ID_LEN, "%s", id);
  }
  return BB_OK;
}

/* ---- eligibility ------------------------------------------------------- */

int bb_check_eligibility(const bb_election_t *election, const char *cert_name) {
  if (election == NULL || cert_name == NULL) {
    return 0;
  }
  for (int i = 0; i < election->eligible_count && i < BB_MAX_VOTERS; i++) {
    if (strcmp(election->eligible[i], cert_name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* ---- UC-2 -------------------------------------------------------------- */

bb_result_t bb_join(bb_ctx *ctx, const char *election_id, const char *cert_name,
                    bb_election_t *out) {
  if (election_id == NULL || cert_name == NULL) {
    return BB_ERR_NOT_FOUND;
  }

  bb_election_t election;
  bb_result_t r = load_election(ctx, election_id, &election);
  if (r != BB_OK) {
    return r;
  }

  /* Cert first: an unverifiable cert is refused whatever the election's state,
   * so a bad cert learns nothing about the election. */
  bb_cert_status_t cert = bb_verify_cert(ctx, cert_name);
  if (cert == BB_CERT_INVALID) {
    return BB_ERR_CERT_INVALID;
  }
  if (cert == BB_CERT_EXPIRED) {
    return BB_ERR_CERT_EXPIRED;
  }
  if (cert != BB_CERT_VALID) {
    return BB_ERR_NOT_ELIGIBLE;
  }

  if (!bb_check_eligibility(&election, cert_name)) {
    return BB_ERR_NOT_ELIGIBLE;
  }

  if (election.state != BB_STATE_OPEN) {
    bb_log(ctx, "[join] '%s' is %s, not OPEN", election_id, bb_state_str(election.state));
    return BB_ERR_NOT_OPEN;
  }

  if (out != NULL) {
    *out = election;
  }
  return BB_OK;
}

/* ---- UC-3 / UC-4 ------------------------------------------------------- */

/* The unlocked body of bb_record_ballot; the wrapper below holds the write
 * lock, so every early return here still releases it. */
static bb_result_t record_ballot_locked(bb_ctx *ctx, const char *election_id,
                                        const bb_ballot_t *ballot, bb_receipt_t *out) {
  bb_election_t election;
  bb_result_t r = load_election(ctx, election_id, &election);
  if (r != BB_OK) {
    return r;
  }

  /* A ballot may only be recorded while the election is open. CLOSED and
   * PUBLISHED are reported as BB_ERR_CLOSED because that is the race the voter
   * actually sees (decision-table rules 2 and 4). */
  if (election.state == BB_STATE_CLOSED || election.state == BB_STATE_PUBLISHED) {
    return BB_ERR_CLOSED;
  }
  if (election.state != BB_STATE_OPEN) {
    return BB_ERR_NOT_OPEN;
  }

  /* Eligibility is re-checked at record time, never trusted from join. */
  if (!bb_check_eligibility(&election, ballot->cert_name)) {
    return BB_ERR_NOT_ELIGIBLE;
  }

  /* Decrypt before any write, so a malformed payload leaves the store and the
   * nonce untouched. */
  int option_index = -1;
  bb_result_t cr = bb_decrypt_ballot(ctx, ballot, &option_index);
  if (cr != BB_OK) {
    return cr;
  }
  if (option_index < 0 || option_index >= election.option_count) {
    return BB_ERR_BAD_OPTION;
  }

  /* Anti-replay: a nonce is good for exactly one ballot. */
  bb_db_cmd_t seen = {0};
  seen.op = BB_DB_NONCE_SEEN;
  snprintf(seen.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(seen.nonce, BB_NONCE_LEN, "%s", ballot->nonce);
  bb_db_result_t seen_res;
  memset(&seen_res, 0, sizeof(seen_res));
  bb_result_t sr = db_exec(ctx, &seen, &seen_res);
  if (sr != BB_OK) {
    return sr;
  }
  if (seen_res.found) {
    return BB_ERR_REPLAY;
  }

  /* An existing ballot from this voter makes this an update (UC-4): the new
   * version is one past the current one, and the old row is superseded. */
  bb_db_cmd_t prior = {0};
  prior.op = BB_DB_GET_PRIOR_BALLOT;
  snprintf(prior.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(prior.cert_name, BB_CERT_LEN, "%s", ballot->cert_name);
  bb_db_result_t prior_res;
  memset(&prior_res, 0, sizeof(prior_res));
  bb_result_t pr = db_exec(ctx, &prior, &prior_res);
  if (pr != BB_OK) {
    return pr;
  }
  const int version = prior_res.found ? prior_res.row.version + 1 : 1;

  bb_result_t rr = bb_issue_receipt(ctx, ballot, version, out);
  if (rr != BB_OK) {
    return rr;
  }

  /* Persist the hash row (identity-free). */
  bb_ballot_hash_t row = {0};
  snprintf(row.hash, BB_HASH_LEN, "%s", out->hash);
  row.option_index = option_index;
  row.version = version;
  row.superseded = 0;

  bb_db_cmd_t append = {0};
  append.op = BB_DB_APPEND_BALLOT;
  snprintf(append.election_id, BB_ID_LEN, "%s", election_id);
  append.hash_row = &row;
  bb_result_t ar = db_exec(ctx, &append, NULL);
  if (ar != BB_OK) {
    return ar;
  }

  /* Supersede the prior version, so only the latest ballot is ever counted. */
  if (prior_res.found) {
    bb_db_cmd_t mark = {0};
    mark.op = BB_DB_MARK_SUPERSEDED;
    snprintf(mark.election_id, BB_ID_LEN, "%s", election_id);
    snprintf(mark.hash, BB_HASH_LEN, "%s", prior_res.row.hash);
    bb_result_t mr = db_exec(ctx, &mark, NULL);
    if (mr != BB_OK) {
      return mr;
    }
  }

  bb_db_cmd_t consume = {0};
  consume.op = BB_DB_NONCE_MARK;
  snprintf(consume.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(consume.nonce, BB_NONCE_LEN, "%s", ballot->nonce);
  return db_exec(ctx, &consume, NULL);
}

bb_result_t bb_record_ballot(bb_ctx *ctx, const char *election_id, const bb_ballot_t *ballot,
                             bb_receipt_t *out) {
  if (ctx == NULL || election_id == NULL || ballot == NULL || out == NULL) {
    return BB_ERR_DB;
  }

  /* Read-check-write on the ballot store must be atomic per voter, or two
   * concurrent submissions can both see "no prior ballot" (R1). */
  bb_write_lock(ctx);
  bb_result_t r = record_ballot_locked(ctx, election_id, ballot, out);
  bb_write_unlock(ctx);
  return r;
}

/* ---- UC-5 -------------------------------------------------------------- */

bb_result_t bb_publish_results(bb_ctx *ctx, const char *election_id) {
  if (election_id == NULL) {
    return BB_ERR_DB;
  }
  /* Publishing is a lifecycle transition like any other: CLOSED -> PUBLISHED,
   * with the current state read back from the store. */
  return bb_transition_state(ctx, election_id, BB_STATE_PUBLISHED);
}

bb_result_t bb_get_results(bb_ctx *ctx, const char *election_id, const char *cert_name,
                           bb_results_t *out) {
  if (election_id == NULL || cert_name == NULL || out == NULL) {
    return BB_ERR_DB;
  }

  bb_election_t election;
  bb_result_t r = load_election(ctx, election_id, &election);
  if (r != BB_OK) {
    return r;
  }

  /* Both gates run before anything is read out of the store, so a refusal
   * cannot leak tally or hash data. */
  if (election.state != BB_STATE_PUBLISHED) {
    return BB_ERR_NOT_PUBLISHED;
  }
  if (!bb_check_eligibility(&election, cert_name)) {
    return BB_ERR_NOT_ELIGIBLE;
  }

  bb_db_cmd_t tally = {0};
  tally.op = BB_DB_GET_TALLY;
  snprintf(tally.election_id, BB_ID_LEN, "%s", election_id);
  bb_db_result_t tally_res;
  memset(&tally_res, 0, sizeof(tally_res));
  bb_result_t tr = db_exec(ctx, &tally, &tally_res);
  if (tr != BB_OK) {
    return tr;
  }

  bb_db_cmd_t hashes = {0};
  hashes.op = BB_DB_GET_HASHES;
  snprintf(hashes.election_id, BB_ID_LEN, "%s", election_id);
  bb_db_result_t hash_res;
  memset(&hash_res, 0, sizeof(hash_res));
  bb_result_t hr = db_exec(ctx, &hashes, &hash_res);
  if (hr != BB_OK) {
    return hr;
  }

  memset(out, 0, sizeof(*out));
  out->option_count = election.option_count;
  for (int i = 0; i < election.option_count && i < BB_MAX_OPTIONS; i++) {
    out->tally[i] = tally_res.tally[i];
    snprintf(out->options[i], BB_OPTION_LEN, "%s", election.options[i]);
  }
  out->hash_count = hash_res.hash_count < BB_MAX_VOTERS ? hash_res.hash_count : BB_MAX_VOTERS;
  for (int i = 0; i < out->hash_count; i++) {
    out->hashes[i] = hash_res.hashes[i];
  }
  return BB_OK;
}

/* ---- UC-6 -------------------------------------------------------------- */

bb_result_t bb_lookup_hash(bb_ctx *ctx, const char *election_id, const char *hash,
                           bb_ballot_hash_t *out) {
  if (election_id == NULL || hash == NULL) {
    return BB_ERR_DB;
  }

  bb_db_cmd_t find = {0};
  find.op = BB_DB_FIND_HASH;
  snprintf(find.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(find.hash, BB_HASH_LEN, "%s", hash);

  bb_db_result_t res;
  memset(&res, 0, sizeof(res));
  bb_result_t r = db_exec(ctx, &find, &res);
  if (r != BB_OK) {
    return r;
  }
  /* A superseded or never-issued hash is simply absent: FIND_HASH matches live
   * rows only, so both collapse to the same dropped-ballot answer and nothing
   * is revealed about other ballots. */
  if (!res.found) {
    return BB_ERR_NOT_FOUND;
  }
  if (out != NULL) {
    *out = res.row;
  }
  return BB_OK;
}
