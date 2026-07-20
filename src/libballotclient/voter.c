#include "libballotclient/voter.h"

#include <string.h>

/* ---- decision logic (pure, complete) ---------------------------------- */

bu_vote_action_t bu_route_vote(const bu_session_t *session) {
  if (session == NULL || !session->joined) {
    return BU_MUST_JOIN; /* rule 1 */
  }
  if (!session->has_ballot) {
    return BU_CAST; /* rule 3 */
  }
  return BU_UPDATE; /* rule 5 */
}

bu_join_outcome_t bu_classify_join(bb_result_t status, const bb_election_t *election) {
  switch (status) {
    case BB_ERR_NOT_FOUND:
      return BU_JOIN_NOT_FOUND;
    case BB_ERR_NOT_ELIGIBLE:
    case BB_ERR_CERT_INVALID:
    case BB_ERR_CERT_EXPIRED:
      return BU_JOIN_NOT_ELIGIBLE;
    case BB_ERR_NOT_OPEN:
      return BU_JOIN_NOT_OPEN;
    case BB_OK:
      /* Admitted only if the daemon returned an OPEN election; any other state
       * means "saved for later" per UC-2 alternative flow 4a. */
      if (election != NULL && election->state == BB_STATE_OPEN) {
        return BU_JOIN_ADMITTED;
      }
      return BU_JOIN_NOT_OPEN;
    default:
      /* Transport-level failure (unreachable host, not-implemented stub). */
      return BU_JOIN_TIMEOUT;
  }
}

/* ---- crypto seam (placeholder) ---------------------------------------- */

static void hash_from_seed(char *out, unsigned int seed) {
  static const char hex[] = "0123456789abcdef";
  unsigned int state = seed * 2654435761u + 1u;
  for (int i = 0; i < BB_HASH_LEN - 1; i++) {
    state = state * 1103515245u + 12345u;
    out[i] = hex[(state >> 16) & 0xF];
  }
  out[BB_HASH_LEN - 1] = '\0';
}

bb_result_t bu_encrypt_ballot(bcl_ctx *ctx, int option_index, const char *nonce, bb_ballot_t *out) {
  if (out == NULL || nonce == NULL || option_index < 0 || option_index > 0xFF) {
    return BB_ERR_BAD_OPTION;
  }
  memset(out, 0, sizeof(*out));
  /* Placeholder: option index carried in payload[0], matching the daemon's
   * bb_decrypt_ballot stub. Real RSA-OAEP over the election pubkey lands with
   * keys - plaintext must never leave this buffer then (test U-32). */
  out->payload[0] = (uint8_t)option_index;
  out->payload_len = 1;
  snprintf(out->nonce, BB_NONCE_LEN, "%s", nonce);
  bcl_log(ctx, "[crypto] encrypt ballot option=%d nonce=%s (placeholder)", option_index, nonce);
  return BB_OK;
}

bb_result_t bu_derive_receipt(bcl_ctx *ctx, const char *secret_key, char out[BB_HASH_LEN]) {
  if (secret_key == NULL || out == NULL) {
    return BB_ERR_DB;
  }
  unsigned int h = 5381u;
  for (const char *p = secret_key; *p; p++) {
    h = h * 33u + (unsigned char)*p;
  }
  hash_from_seed(out, h);
  bcl_log(ctx, "[crypto] derive receipt from key (placeholder) -> %s", out);
  return BB_OK;
}
