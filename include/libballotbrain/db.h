#ifndef BALLOTBRAIN_DB_H
#define BALLOTBRAIN_DB_H

/*
 * DB seam.
 *
 * The authoritative store is SimpleDB (50.043), which does not exist in this
 * repo yet. Until it lands, every storage access goes through a single narrow
 * function, db_exec(), which takes a *typed, parameterized* command struct
 * (never a raw SQL string - that keeps us type-safe and injection-safe by
 * construction) and, for now, only logs the operation it would run.
 *
 * Contract today:
 *   - Write ops (INSERT/UPDATE/APPEND/MARK/NONCE_MARK) are logged and return
 *     BB_OK. Nothing is persisted.
 *   - Read ops (GET/FIND/TALLY/NONCE_SEEN) are logged and return
 *     BB_ERR_NOT_IMPLEMENTED. There is no readback until SimpleDB is wired in.
 *
 * When SimpleDB is implemented, only db_exec() changes: it translates each
 * bb_db_cmd into a real parameterized SQL statement and fills bb_db_result.
 *
 * Log secrecy (R2 / test U-21): the rendered log line for a ballot append
 * carries the hash and option, never the submitting cert. No log line pairs a
 * voter identity with a ballot or hash.
 */

#include "libballotbrain/types.h"

struct bb_ctx;

typedef enum {
  BB_DB_INSERT_ELECTION,  /* write: persist a new election in DRAFT */
  BB_DB_UPDATE_STATE,     /* write: set election.state */
  BB_DB_APPEND_BALLOT,    /* write: add a ballot hash row */
  BB_DB_MARK_SUPERSEDED,  /* write: flag prior versions of a voter's ballot */
  BB_DB_NONCE_MARK,       /* write: record a consumed nonce */
  BB_DB_GET_ELECTION,     /* read: fetch an election by id */
  BB_DB_GET_TALLY,        /* read: fetch tally of non-superseded ballots */
  BB_DB_GET_HASHES,       /* read: fetch published hash list */
  BB_DB_FIND_HASH,        /* read: look up one published, non-superseded hash */
  BB_DB_NONCE_SEEN        /* read: has this nonce been consumed? */
} bb_db_op_t;

/*
 * One command. Only the fields relevant to `op` are read; the rest are
 * ignored. Kept as a flat struct (not a union) for simple, readable call
 * sites - the deferred SQL translation reads exactly the fields it needs.
 */
typedef struct {
  bb_db_op_t op;
  char election_id[BB_ID_LEN];
  bb_state_t new_state;               /* UPDATE_STATE */
  const bb_ballot_hash_t *hash_row;   /* APPEND_BALLOT */
  char hash[BB_HASH_LEN];             /* FIND_HASH */
  char nonce[BB_NONCE_LEN];           /* NONCE_MARK / NONCE_SEEN */
  const bb_config_t *config;          /* INSERT_ELECTION */
} bb_db_cmd_t;

/*
 * Read-op output. Populated only once SimpleDB backs the seam; today reads
 * return BB_ERR_NOT_IMPLEMENTED and leave this untouched.
 */
typedef struct {
  bb_election_t election;          /* GET_ELECTION */
  int tally[BB_MAX_OPTIONS];       /* GET_TALLY */
  bb_ballot_hash_t hashes[BB_MAX_VOTERS]; /* GET_HASHES */
  int hash_count;
  int found;                       /* FIND_HASH / NONCE_SEEN: 1 if present */
} bb_db_result_t;

/*
 * Execute one command. `out` may be NULL for write ops. Logs a SQL-ish line
 * to the context's log sink. Returns BB_OK for writes, BB_ERR_NOT_IMPLEMENTED
 * for reads (until SimpleDB is wired in).
 */
bb_result_t db_exec(struct bb_ctx *ctx, const bb_db_cmd_t *cmd, bb_db_result_t *out);

#endif /* BALLOTBRAIN_DB_H */
