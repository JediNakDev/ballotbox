#include "libballotclient/client.h"

/*
 * Transport seam - stub implementation, and the only thing in this translation
 * unit. Keeping it alone here means a caller's tests can substitute bcl_send
 * without dragging in (or colliding with) the rest of the client core.
 *
 * When libtetrissh / libhtttp land, only this file changes: it opens the secure
 * session, serialises the request, and fills `resp` from the reply. No
 * cert/ballot pairing is logged, preserving wire secrecy.
 */

static const char *op_str(bcl_op_t op) {
  switch (op) {
    case BCL_JOIN: return "JOIN";
    case BCL_CAST: return "CAST";
    case BCL_UPDATE: return "UPDATE";
    case BCL_RESULTS: return "RESULTS";
    case BCL_CHECK: return "CHECK";
    case BCL_CREATE: return "CREATE";
    case BCL_OPEN: return "OPEN";
    case BCL_CLOSE: return "CLOSE";
    case BCL_PUBLISH: return "PUBLISH";
  }
  return "?";
}

bb_result_t bcl_send(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp) {
  if (req == NULL) {
    return BB_ERR_DB;
  }
  bcl_log(ctx, "[transport] send %s election='%s' (placeholder, not sent)", op_str(req->op),
          req->election_id);
  if (resp != NULL) {
    resp->status = BB_ERR_NOT_IMPLEMENTED;
  }
  return BB_ERR_NOT_IMPLEMENTED;
}
