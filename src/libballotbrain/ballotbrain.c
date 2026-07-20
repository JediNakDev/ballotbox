#include "libballotbrain/ballotbrain.h"

#include <stdarg.h>
#include <stdlib.h>

/*
 * Per-instance context. Deliberately tiny today: it owns the operation-log
 * sink. When SimpleDB lands it also owns the store handle; when concurrency
 * (R1) is implemented it owns the write mutex. Keeping state here (rather than
 * in file-scope globals) is what lets the later test agent run isolated cases.
 */
struct bb_ctx {
  FILE *log;
  int next_election; /* placeholder id allocator until the DB assigns ids */
};

bb_ctx *bb_create(void) {
  bb_ctx *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }
  ctx->log = stderr;
  ctx->next_election = 100;
  return ctx;
}

/* Internal: allocate the next placeholder election id ("E-100", "E-101", ...).
 * Real ids are assigned by SimpleDB once the store is wired in. */
void bb_alloc_id(bb_ctx *ctx, char out[BB_ID_LEN]) {
  snprintf(out, BB_ID_LEN, "E-%d", ctx->next_election++);
}

void bb_destroy(bb_ctx *ctx) {
  free(ctx);
}

void bb_set_log(bb_ctx *ctx, FILE *sink) {
  if (ctx != NULL) {
    ctx->log = sink;
  }
}

void bb_log(bb_ctx *ctx, const char *fmt, ...) {
  if (ctx == NULL || ctx->log == NULL) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(ctx->log, fmt, ap);
  va_end(ap);
  fputc('\n', ctx->log);
}

const char *bb_state_str(bb_state_t s) {
  switch (s) {
    case BB_STATE_DRAFT: return "DRAFT";
    case BB_STATE_OPEN: return "OPEN";
    case BB_STATE_CLOSED: return "CLOSED";
    case BB_STATE_PUBLISHED: return "PUBLISHED";
  }
  return "?";
}
