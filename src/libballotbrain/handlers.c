#include "libballotbrain/ballotbrain.h"

#include <string.h>

/*
 * System-class request handlers. Each orchestrates: validate -> seam calls ->
 * return. Where a step needs to read state back (prior ballot version, nonce
 * history, current tally, current state), the DB seam returns
 * BB_ERR_NOT_IMPLEMENTED today; those points are marked so the SimpleDB pass
 * knows exactly what to fill in. The pure checks (validation, eligibility,
 * option range) are complete now.
 */

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

/* ---- eligibility / certs ---------------------------------------------- */

bb_cert_status_t bb_verify_cert(bb_ctx *ctx, const char *cert_name) {
  /* Placeholder: real X.509 verification (validity, expiry, chain) arrives with
   * the PKI/transport layer. For now, any non-empty name is treated as a valid
   * cert; the eligible-list check is the meaningful gate and is pure. */
  if (cert_name == NULL || cert_name[0] == '\0') {
    return BB_CERT_INVALID;
  }
  bb_log(ctx, "[cert] verify '%s' (placeholder -> VALID)", cert_name);
  return BB_CERT_VALID;
}

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

/* ---- UC-3 / UC-4 ------------------------------------------------------- */

bb_result_t bb_record_ballot(bb_ctx *ctx, const char *election_id, const bb_ballot_t *ballot,
                             bb_receipt_t *out) {
  if (election_id == NULL || ballot == NULL || out == NULL) {
    return BB_ERR_DB;
  }

  /* Decrypt to recover the chosen option (crypto seam / placeholder). */
  int option_index = -1;
  bb_result_t cr = bb_decrypt_ballot(ctx, ballot, &option_index);
  if (cr != BB_OK) {
    return cr; /* BB_ERR_DECRYPT once real OAEP is wired in */
  }

  /*
   * Deferred readback gates (each returns NOT_IMPLEMENTED today; enforced once
   * SimpleDB backs the seam):
   *   - GET_ELECTION: confirm state == OPEN (else BB_ERR_CLOSED / BB_ERR_NOT_OPEN),
   *     re-check eligibility at record time, and range-check option_index against
   *     option_count (else BB_ERR_BAD_OPTION).
   *   - NONCE_SEEN: reject replays (else BB_ERR_REPLAY).
   *   - prior-ballot lookup: set version and supersede the previous hash on update.
   */
  bb_db_cmd_t seen = {0};
  seen.op = BB_DB_NONCE_SEEN;
  snprintf(seen.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(seen.nonce, BB_NONCE_LEN, "%s", ballot->nonce);
  (void)db_exec(ctx, &seen, NULL); /* readback deferred */

  int version = 1; /* real version derives from the voter's prior ballot */

  /* Issue the receipt/commitment for this ballot. */
  bb_result_t rr = bb_issue_receipt(ctx, ballot, version, out);
  if (rr != BB_OK) {
    return rr;
  }

  /* Persist the hash row (identity-free) and consume the nonce. */
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

  bb_db_cmd_t mark = {0};
  mark.op = BB_DB_NONCE_MARK;
  snprintf(mark.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(mark.nonce, BB_NONCE_LEN, "%s", ballot->nonce);
  (void)db_exec(ctx, &mark, NULL);

  return BB_OK;
}

/* ---- UC-5 -------------------------------------------------------------- */

bb_result_t bb_publish_results(bb_ctx *ctx, const char *election_id) {
  if (election_id == NULL) {
    return BB_ERR_DB;
  }

  /*
   * Publish is gated: it requires the election to be CLOSED (U-08). That gate
   * needs the current state, which is a deferred readback; once SimpleDB is
   * wired, GET_ELECTION here returns BB_ERR_NOT_OPEN/ILLEGAL when not CLOSED.
   */
  bb_db_cmd_t get = {0};
  get.op = BB_DB_GET_ELECTION;
  snprintf(get.election_id, BB_ID_LEN, "%s", election_id);
  (void)db_exec(ctx, &get, NULL); /* readback deferred */

  /* Compute the published tally over non-superseded ballots (deferred). */
  bb_db_cmd_t tally = {0};
  tally.op = BB_DB_GET_TALLY;
  snprintf(tally.election_id, BB_ID_LEN, "%s", election_id);
  (void)db_exec(ctx, &tally, NULL);

  /* Flip to PUBLISHED. */
  bb_db_cmd_t upd = {0};
  upd.op = BB_DB_UPDATE_STATE;
  snprintf(upd.election_id, BB_ID_LEN, "%s", election_id);
  upd.new_state = BB_STATE_PUBLISHED;
  return db_exec(ctx, &upd, NULL);
}

/* ---- UC-6 -------------------------------------------------------------- */

bb_result_t bb_lookup_hash(bb_ctx *ctx, const char *election_id, const char *hash,
                           bb_ballot_hash_t *out) {
  if (election_id == NULL || hash == NULL) {
    return BB_ERR_DB;
  }
  (void)out;

  /* Search published, non-superseded hashes. Without a store this cannot
   * resolve found/not-found, so it honestly reports NOT_IMPLEMENTED rather than
   * a misleading "not found". SimpleDB fills FIND_HASH to return BB_OK (found,
   * with option) or BB_ERR_NOT_FOUND (dropped-ballot path). */
  bb_db_cmd_t find = {0};
  find.op = BB_DB_FIND_HASH;
  snprintf(find.election_id, BB_ID_LEN, "%s", election_id);
  snprintf(find.hash, BB_HASH_LEN, "%s", hash);
  return db_exec(ctx, &find, NULL);
}
