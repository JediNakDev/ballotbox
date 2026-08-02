#include "libballotbrain/ballotbrain.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>

/*
 * Per-instance context: the operation-log sink, the id allocator, and the write
 * lock that serialises ballot recording (R1). When SimpleDB lands it also owns
 * the store handle. Keeping state here rather than in file-scope globals is
 * what lets each test case run an isolated instance.
 */
struct bb_ctx {
  FILE *log;
  int next_election; /* placeholder id allocator until the DB assigns ids */
  pthread_mutex_t write_lock;
};

bb_ctx *bb_create(void) {
  bb_ctx *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }
  ctx->log = stderr;
  ctx->next_election = 100;
  if (pthread_mutex_init(&ctx->write_lock, NULL) != 0) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

void bb_write_lock(bb_ctx *ctx) {
  if (ctx != NULL) {
    pthread_mutex_lock(&ctx->write_lock);
  }
}

void bb_write_unlock(bb_ctx *ctx) {
  if (ctx != NULL) {
    pthread_mutex_unlock(&ctx->write_lock);
  }
}

/* Internal: allocate the next placeholder election id ("E-100", "E-101", ...).
 * Real ids are assigned by SimpleDB once the store is wired in. */
void bb_alloc_id(bb_ctx *ctx, char out[BB_ID_LEN]) {
  snprintf(out, BB_ID_LEN, "E-%d", ctx->next_election++);
}

void bb_destroy(bb_ctx *ctx) {
  if (ctx == NULL) {
    return;
  }
  pthread_mutex_destroy(&ctx->write_lock);
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
