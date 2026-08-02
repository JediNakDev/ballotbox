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
