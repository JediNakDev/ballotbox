#include "libballotclient/client.h"

#include <stdarg.h>
#include <stdlib.h>

/* Per-client context. Tiny today (log sink); later it owns the libtetrissh
 * session handle. Instance-scoped, no file-scope state. */
struct bcl_ctx {
  FILE *log;
};

bcl_ctx *bcl_create(void) {
  bcl_ctx *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }
  ctx->log = stderr;
  return ctx;
}

void bcl_destroy(bcl_ctx *ctx) {
  free(ctx);
}

void bcl_set_log(bcl_ctx *ctx, FILE *sink) {
  if (ctx != NULL) {
    ctx->log = sink;
  }
}

void bcl_log(bcl_ctx *ctx, const char *fmt, ...) {
  if (ctx == NULL || ctx->log == NULL) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(ctx->log, fmt, ap);
  va_end(ap);
  fputc('\n', ctx->log);
}

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

/*
 * Transport seam - stub. Logs the request that would go over the secure
 * session and reports NOT_IMPLEMENTED. No cert/ballot pairing is logged, to
 * preserve the wire-secrecy property once this is real.
 */
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
