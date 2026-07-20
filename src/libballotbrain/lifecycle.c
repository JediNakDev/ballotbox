#include "libballotbrain/ballotbrain.h"

/*
 * Election lifecycle. The legal chain is strictly:
 *   DRAFT -> OPEN -> CLOSED -> PUBLISHED
 * No skips, no reversals. bb_is_legal_transition is a pure table used both to
 * guard transitions here and (later) by tests directly.
 */
int bb_is_legal_transition(bb_state_t from, bb_state_t to) {
  switch (from) {
    case BB_STATE_DRAFT: return to == BB_STATE_OPEN;
    case BB_STATE_OPEN: return to == BB_STATE_CLOSED;
    case BB_STATE_CLOSED: return to == BB_STATE_PUBLISHED;
    case BB_STATE_PUBLISHED: return 0;
  }
  return 0;
}

bb_result_t bb_transition_state(bb_ctx *ctx, const char *election_id, bb_state_t from,
                                bb_state_t to) {
  /* Legality is enforced before any store access. */
  if (!bb_is_legal_transition(from, to)) {
    bb_log(ctx, "[lifecycle] reject %s -> %s on '%s'", bb_state_str(from), bb_state_str(to),
           election_id ? election_id : "?");
    return BB_ERR_ILLEGAL_TRANSITION;
  }

  /*
   * `from` is caller-supplied today. Once the DB seam can read state back, this
   * fetches the election and verifies its actual current state matches `from`
   * before writing, closing the check-then-act gap.
   */
  bb_db_cmd_t cmd = {0};
  cmd.op = BB_DB_UPDATE_STATE;
  if (election_id != NULL) {
    snprintf(cmd.election_id, BB_ID_LEN, "%s", election_id);
  }
  cmd.new_state = to;
  return db_exec(ctx, &cmd, NULL);
}
