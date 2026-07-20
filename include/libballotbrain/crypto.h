#ifndef BALLOTBRAIN_CRYPTO_H
#define BALLOTBRAIN_CRYPTO_H

/*
 * Crypto seam (daemon side).
 *
 * Real RSA-OAEP / receipt signing depends on keys and PKI that arrive with the
 * teammate's libtetrissh / libhtttp transport layer. Until then these are
 * deterministic placeholders (same spirit as the demo mock_generate_hash): they
 * produce stable, well-formed hex so the logic above them can be exercised, but
 * they are NOT cryptographically meaningful yet.
 *
 * When keys land, only this seam changes to call the OpenSSL helpers already
 * available in external/2026-pa2-.../source/libs/common.c.
 */

#include "libballotbrain/types.h"

struct bb_ctx;

/*
 * Decrypt and validate a submitted ballot payload, yielding the chosen option
 * index. Placeholder today: derives a deterministic index from the payload and
 * never reports BB_ERR_DECRYPT (real OAEP failure detection lands with keys).
 */
bb_result_t bb_decrypt_ballot(struct bb_ctx *ctx, const bb_ballot_t *ballot, int *option_index);

/*
 * Issue the receipt/commitment hash for a recorded ballot. Placeholder derives
 * a deterministic hash from (nonce, option, version); the real version is a
 * signed commitment the voter can later verify (UC-6).
 */
bb_result_t bb_issue_receipt(struct bb_ctx *ctx, const bb_ballot_t *ballot, int version,
                             bb_receipt_t *out);

#endif /* BALLOTBRAIN_CRYPTO_H */
