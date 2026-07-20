#ifndef BALLOTCLIENT_ADMIN_H
#define BALLOTCLIENT_ADMIN_H

/*
 * Admin-side logic for ballotctl (UC-1 create/open, close, publish). Assembles
 * requests and pre-validates config client-side before it hits the daemon.
 */

#include "libballotclient/client.h"

/* Pure client-side pre-validation of config, so the admin gets immediate
 * feedback without a round trip. Delegates to the brain's authoritative
 * bb_validate_config, which the daemon also enforces. */
bb_result_t bc_prevalidate_config(const bb_config_t *config);

/* Build a CREATE request from a validated config. Returns the pre-validation
 * result; on BB_OK, `out` is a ready-to-send request. */
bb_result_t bc_build_create(const bb_config_t *config, bcl_request_t *out);

/* Build an OPEN/CLOSE/PUBLISH lifecycle request for an election. `op` must be
 * one of BCL_OPEN, BCL_CLOSE, BCL_PUBLISH. */
bb_result_t bc_build_transition(bcl_op_t op, const char *election_id, bcl_request_t *out);

#endif /* BALLOTCLIENT_ADMIN_H */
