#ifndef BALLOTCLIENT_H
#define BALLOTCLIENT_H

/*
 * libballotclient - client-side logic shared by ballotu (voter) and ballotctl
 * (admin). It reuses the canonical domain model from libballotbrain rather than
 * redefining it.
 *
 * Like the daemon library, this holds the *logic*. The wire transport
 * (libtetrissh / libhtttp) and the ballot cryptography sit behind stubbed seams
 * and are wired in once the teammate's transport layer and PKI land. The pure
 * decision logic (vote routing, join-outcome classification, config assembly)
 * is complete now.
 *
 * Naming: bcl_* shared core, bu_* voter entry points, bc_* admin entry points.
 */

#include "libballotbrain/types.h"

#include <stdio.h>

/* Opaque per-client context (log sink today; the session handle later). */
typedef struct bcl_ctx bcl_ctx;

bcl_ctx *bcl_create(void);
void bcl_destroy(bcl_ctx *ctx);
void bcl_set_log(bcl_ctx *ctx, FILE *sink);
void bcl_log(bcl_ctx *ctx, const char *fmt, ...);

/* Every request the clients can make of the daemon. */
typedef enum {
  BCL_JOIN,     /* voter: UC-2 */
  BCL_CAST,     /* voter: UC-3 */
  BCL_UPDATE,   /* voter: UC-4 */
  BCL_RESULTS,  /* observer: UC-5 */
  BCL_CHECK,    /* voter: UC-6 */
  BCL_CREATE,   /* admin: UC-1 create */
  BCL_OPEN,     /* admin: UC-1 open */
  BCL_CLOSE,    /* admin: close */
  BCL_PUBLISH   /* admin: publish */
} bcl_op_t;

/* One request. Only the fields relevant to `op` are populated. */
typedef struct {
  bcl_op_t op;
  char cert_name[BB_CERT_LEN];
  char election_id[BB_ID_LEN];
  bb_ballot_t ballot;      /* CAST / UPDATE */
  char hash[BB_HASH_LEN];  /* CHECK: derived receipt hash */
  bb_config_t config;      /* CREATE */
} bcl_request_t;

/* One response. Fields are filled per the request op once transport is real. */
typedef struct {
  bb_result_t status;
  bb_election_t election;               /* JOIN / RESULTS */
  bb_receipt_t receipt;                 /* CAST / UPDATE */
  int tally[BB_MAX_OPTIONS];            /* RESULTS */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS]; /* RESULTS */
  int hash_count;
  int found;                            /* CHECK: 1 if hash counted */
  int found_option;                     /* CHECK: revealed only to the key holder */
} bcl_response_t;

/*
 * Transport seam. Sends a request over the secure session and fills `resp`.
 * Stub today: logs the intended request and returns BB_ERR_NOT_IMPLEMENTED.
 * When libtetrissh/libhtttp land, only this function changes.
 */
bb_result_t bcl_send(bcl_ctx *ctx, const bcl_request_t *req, bcl_response_t *resp);

#endif /* BALLOTCLIENT_H */
