#include "libballotbrain/db.h"
#include "libballotbrain/ballotbrain.h"

/*
 * DB seam - stub implementation.
 *
 * Every op is rendered as a SQL-ish log line so the intended statement is
 * visible (and, later, is the reference for the real SimpleDB translation).
 * Writes return BB_OK; reads return BB_ERR_NOT_IMPLEMENTED because there is no
 * backing store to read from yet.
 *
 * Secrecy: the APPEND_BALLOT rendering logs the hash and option only - never
 * the submitting cert - so no log line links a voter to a ballot (R2, U-21).
 */

bb_result_t db_exec(bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out) {
  if (cmd == NULL) {
    return BB_ERR_DB;
  }

  switch (cmd->op) {
    case BB_DB_INSERT_ELECTION:
      bb_log(ctx, "[db] INSERT INTO elections(id, title, state) VALUES('%s', ?, 'DRAFT')",
             cmd->election_id);
      return BB_OK;

    case BB_DB_UPDATE_STATE:
      bb_log(ctx, "[db] UPDATE elections SET state='%s' WHERE id='%s'",
             bb_state_str(cmd->new_state), cmd->election_id);
      return BB_OK;

    case BB_DB_APPEND_BALLOT:
      /* hash + option only; no cert - see secrecy note above. */
      bb_log(ctx,
             "[db] INSERT INTO ballots(election_id, hash, option_index, version, superseded) "
             "VALUES('%s', '%s', %d, %d, %d)",
             cmd->election_id, cmd->hash_row ? cmd->hash_row->hash : "?",
             cmd->hash_row ? cmd->hash_row->option_index : -1,
             cmd->hash_row ? cmd->hash_row->version : -1,
             cmd->hash_row ? cmd->hash_row->superseded : 0);
      return BB_OK;

    case BB_DB_MARK_SUPERSEDED:
      bb_log(ctx,
             "[db] UPDATE ballots SET superseded=1 WHERE election_id='%s' AND hash<>'%s'",
             cmd->election_id, cmd->hash);
      return BB_OK;

    case BB_DB_NONCE_MARK:
      bb_log(ctx, "[db] INSERT INTO nonces(election_id, nonce) VALUES('%s', '%s')",
             cmd->election_id, cmd->nonce);
      return BB_OK;

    case BB_DB_GET_ELECTION:
      bb_log(ctx, "[db] SELECT * FROM elections WHERE id='%s' -- not implemented",
             cmd->election_id);
      (void)out;
      return BB_ERR_NOT_IMPLEMENTED;

    case BB_DB_GET_TALLY:
      bb_log(ctx,
             "[db] SELECT option_index, COUNT(*) FROM ballots WHERE election_id='%s' "
             "AND superseded=0 GROUP BY option_index -- not implemented",
             cmd->election_id);
      return BB_ERR_NOT_IMPLEMENTED;

    case BB_DB_GET_HASHES:
      bb_log(ctx,
             "[db] SELECT hash, option_index FROM ballots WHERE election_id='%s' "
             "AND superseded=0 -- not implemented",
             cmd->election_id);
      return BB_ERR_NOT_IMPLEMENTED;

    case BB_DB_FIND_HASH:
      bb_log(ctx,
             "[db] SELECT 1 FROM ballots WHERE election_id='%s' AND hash='%s' "
             "AND superseded=0 -- not implemented",
             cmd->election_id, cmd->hash);
      return BB_ERR_NOT_IMPLEMENTED;

    case BB_DB_NONCE_SEEN:
      bb_log(ctx, "[db] SELECT 1 FROM nonces WHERE election_id='%s' AND nonce='%s' -- not implemented",
             cmd->election_id, cmd->nonce);
      return BB_ERR_NOT_IMPLEMENTED;
  }

  return BB_ERR_DB;
}
