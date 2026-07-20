#ifndef BALLOTBRAIN_H
#define BALLOTBRAIN_H

/*
 * libballotbrain - the daemon-side authoritative logic for BallotBox (the
 * `System` class). This is the umbrella header: consumers include it to get
 * the whole API.
 *
 * Scope note: this library holds the *logic*. Storage (SimpleDB), transport
 * (libtetrissh/libhtttp) and cryptography sit behind stubbed seams (db.h,
 * crypto.h) and are wired in later. Functions whose behaviour depends on
 * reading state back are therefore structurally complete but behaviourally
 * partial until the DB seam is implemented.
 */

#include "libballotbrain/types.h"
#include "libballotbrain/db.h"
#include "libballotbrain/crypto.h"

#include <stdio.h>

/* Opaque per-instance context (holds config + the log sink; later the store
 * handle and the write mutex). No hidden file-scope state, so isolated
 * instances - and isolated tests - are possible. */
typedef struct bb_ctx bb_ctx;

bb_ctx *bb_create(void);
void bb_destroy(bb_ctx *ctx);

/* Redirect the seam/operation log (defaults to stderr). Pass NULL to silence. */
void bb_set_log(bb_ctx *ctx, FILE *sink);

/* Internal: emit one operation-log line. Exposed for the seam sources. */
void bb_log(bb_ctx *ctx, const char *fmt, ...);

/* Internal: allocate the next placeholder election id. */
void bb_alloc_id(bb_ctx *ctx, char out[BB_ID_LEN]);

/* Human-readable state name (for logs and error messages). */
const char *bb_state_str(bb_state_t s);

/* ---- UC-1: instantiate ------------------------------------------------- */

/* Pure predicate: validate an election config. Returns BB_OK or a specific
 * BB_ERR_CONFIG_* code. No side effects. */
bb_result_t bb_validate_config(const bb_config_t *config);

/* Validate, then persist a new election in DRAFT. Fills out_id. Persistence
 * goes through the DB seam (logged today). */
bb_result_t bb_create_election(bb_ctx *ctx, const bb_config_t *config, char out_id[BB_ID_LEN]);

/* ---- lifecycle --------------------------------------------------------- */

/* Pure: is `to` a legal successor of `from`? (DRAFT->OPEN->CLOSED->PUBLISHED) */
int bb_is_legal_transition(bb_state_t from, bb_state_t to);

/* Transition an election's state. Rejects illegal edges with
 * BB_ERR_ILLEGAL_TRANSITION before touching the store. */
bb_result_t bb_transition_state(bb_ctx *ctx, const char *election_id, bb_state_t from,
                                bb_state_t to);

/* ---- eligibility / certs ---------------------------------------------- */

/* Verify a certificate (placeholder until PKI arrives). */
bb_cert_status_t bb_verify_cert(bb_ctx *ctx, const char *cert_name);

/* Pure: is cert_name on the election's eligible list? */
int bb_check_eligibility(const bb_election_t *election, const char *cert_name);

/* ---- UC-3 / UC-4: cast & update --------------------------------------- */

/* Record a ballot (cast or update). Verifies nonce/eligibility/option range,
 * decrypts via the crypto seam, appends through the DB seam, and issues a
 * receipt. Behaviourally partial until the DB seam can report prior versions
 * and nonce state. */
bb_result_t bb_record_ballot(bb_ctx *ctx, const char *election_id, const bb_ballot_t *ballot,
                             bb_receipt_t *out);

/* ---- UC-5: results ----------------------------------------------------- */

/* Publish a CLOSED election's results (gated: requires CLOSED). */
bb_result_t bb_publish_results(bb_ctx *ctx, const char *election_id);

/* ---- UC-6: check your vote -------------------------------------------- */

/* Look up a receipt hash in a published election's non-superseded set. */
bb_result_t bb_lookup_hash(bb_ctx *ctx, const char *election_id, const char *hash,
                           bb_ballot_hash_t *out);

#endif /* BALLOTBRAIN_H */
