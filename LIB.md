# BallotBox Libraries

This document is the reference for the two logic libraries that hold the real BallotBox behaviour.
It is written for two audiences: the agent that will author the deferred unit tests, and the teammates wiring in SimpleDB and the transport layer.
Terminology, use cases (UC-1 to UC-6), and the class diagram are defined in [README](README.md).

## Scope and status

These libraries contain the **logic only**.
The three things the logic depends on from the outside world - persistence, transport, and cryptography - each sit behind a stubbed seam and are implemented later.
Nothing here is integrated yet: `ballotd` does not call `libballotbrain`, and the `ballotu`/`ballotctl` TUIs still run on their own `mock.c` demo state.
The libraries build (`make`) and are archived to `lib/libballotbrain.a` and `lib/libballotclient.a`, ready to be linked and tested in isolation.

Because the DB seam has no readback yet (see below), functions whose behaviour needs to read state back are **structurally complete but behaviourally partial**: they perform the pure checks they can and call the seams in the right order, but they cannot yet enforce anything that requires querying stored state.
Those are the functions whose tests are deferred to the SimpleDB milestone.

## `libballotbrain` - daemon-side authoritative logic

This is the `System` class from the README.
Public umbrella header: `include/libballotbrain/ballotbrain.h`.
Prefix: `bb_*`.
All fallible functions return a `bb_result_t` code; data comes back through out-parameters.

### Canonical model (`include/libballotbrain/types.h`)

`libballotbrain` owns the single source of truth for the domain model, and `libballotclient` reuses it.
The demo `mock.h` copies in `ballotu`/`ballotctl` are intentionally left independent until UI integration.

- `bb_state_t` - `DRAFT`, `OPEN`, `CLOSED`, `PUBLISHED`.
- `bb_cert_status_t` - `INVALID`, `EXPIRED`, `NOT_ELIGIBLE`, `VALID`.
- `bb_result_t` - `BB_OK` plus specific error codes (e.g. `BB_ERR_CONFIG_TIME`, `BB_ERR_NOT_ELIGIBLE`, `BB_ERR_REPLAY`, `BB_ERR_NOT_IMPLEMENTED`).
- `bb_config_t`, `bb_election_t`, `bb_ballot_t`, `bb_ballot_hash_t`, `bb_receipt_t` - the config, election, ballot, published hash row, and receipt shapes.

### API

| Function | UC | Status | Notes |
| --- | --- | --- | --- |
| `bb_validate_config` | UC-1 | Complete (pure) | Title, option-count, and time-window checks; returns a specific `BB_ERR_CONFIG_*`. |
| `bb_create_election` | UC-1 | Partial | Validates, allocates a placeholder id, inserts via the DB seam. |
| `bb_is_legal_transition` | lifecycle | Complete (pure) | The `DRAFT->OPEN->CLOSED->PUBLISHED` table. |
| `bb_transition_state` | UC-1 | Partial | Enforces legality before writing; verifying the *current* state needs readback. |
| `bb_verify_cert` | UC-2 | Placeholder | Real X.509 verification arrives with PKI. |
| `bb_check_eligibility` | UC-2 | Complete (pure) | Scans the election's eligible list. |
| `bb_record_ballot` | UC-3/4 | Partial | Decrypts, issues a receipt, appends the hash row and consumes the nonce; replay/version/range gates need readback. |
| `bb_publish_results` | UC-5 | Partial | Flips to `PUBLISHED`; the CLOSED gate and tally need readback. |
| `bb_lookup_hash` | UC-6 | Partial | Reports `NOT_IMPLEMENTED` until the store can resolve found / not-found. |

## `libballotclient` - client-side logic

Shared by `ballotu` (voter) and `ballotctl` (admin), one artifact.
It reuses the `libballotbrain` model rather than redefining it.
Headers: `include/libballotclient/{client,voter,admin}.h`.
Prefixes: `bcl_*` shared core, `bu_*` voter, `bc_*` admin.

### Core (`client.h`)

- `bcl_ctx` - per-client context (log sink today, the session handle later).
- `bcl_request_t` / `bcl_response_t` - the request/response shapes for every daemon operation (`bcl_op_t`: JOIN, CAST, UPDATE, RESULTS, CHECK, CREATE, OPEN, CLOSE, PUBLISH).
- `bcl_send` - the transport seam (see below).

### Voter (`voter.h`)

- `bu_session_t` - client-local voter session (mirrors `VoterSession`).
- `bu_route_vote` - **complete, pure**: the vote decision-table routing (rules 1/3/5 -> `MUST_JOIN` / `CAST` / `UPDATE`).
- `bu_classify_join` - **complete, pure**: maps a join response to a UC-2 outcome (timeout / not-found / not-eligible / not-open / admitted).
- `bu_encrypt_ballot`, `bu_derive_receipt` - the client crypto seam (placeholder, see below).

### Admin (`admin.h`)

- `bc_prevalidate_config` - **complete**: client-side pre-validation that delegates to the authoritative `bb_validate_config` (no rule duplication).
- `bc_build_create`, `bc_build_transition` - assemble CREATE and OPEN/CLOSE/PUBLISH requests.

## The three seams

Each seam is one narrow function.
When its backing implementation lands, only that function changes; the logic above it is untouched.

### 1. DB seam (`include/libballotbrain/db.h`)

`db_exec(ctx, cmd, out)` takes a **typed, parameterized** `bb_db_cmd_t` - never a raw SQL string, which keeps it type-safe and injection-safe by construction - and today only logs the operation it would run.

- Write ops (`INSERT_ELECTION`, `UPDATE_STATE`, `APPEND_BALLOT`, `MARK_SUPERSEDED`, `NONCE_MARK`) log a SQL-ish line and return `BB_OK`.
- Read ops (`GET_ELECTION`, `GET_TALLY`, `GET_HASHES`, `FIND_HASH`, `NONCE_SEEN`) log and return `BB_ERR_NOT_IMPLEMENTED`.

To implement SimpleDB: translate each `bb_db_cmd_t` into a parameterized SQL statement and fill `bb_db_result_t` for the reads.
Secrecy invariant (R2, test U-21): the ballot-append log line carries the hash and option only, never the submitting cert, so no log line links a voter to a ballot.

### 2. Transport seam (`include/libballotclient/client.h`)

`bcl_send(ctx, req, resp)` sends a request over the secure session.
Stub today: it logs the intended request and returns `BB_ERR_NOT_IMPLEMENTED`.
To implement: wire it to the teammate's `libtetrissh` / `libhtttp` session layer.

### 3. Crypto seam (`include/libballotbrain/crypto.h`, voter half in `voter.h`)

Deterministic, well-formed hex placeholders so the logic can run; not cryptographically meaningful yet.

- Daemon: `bb_decrypt_ballot`, `bb_issue_receipt`.
- Voter: `bu_encrypt_ballot`, `bu_derive_receipt`.

Placeholder wire convention: `payload[0]` carries the chosen option index, so the voter's encrypt stub and the daemon's decrypt stub round-trip an option end to end.
To implement: call the OpenSSL helpers already present in `external/2026-pa2-50005-6767/source/libs/common.c` (RSA-OAEP, AES/HMAC, X.509, SHA-256) once keys are distributed with the transport layer.

## Testing

No tests ship with the libraries; they are authored separately after the libraries stabilise, to keep the test design independent of the implementation.
The libraries are written to be testable at that point: context handles (`bb_ctx`, `bcl_ctx`) give each case an isolated instance with no hidden file-scope state.
The subset that needs none of the three seams is testable immediately (the config validator, the transition table, the vote routing, the join classifier); everything that needs readback waits for the SimpleDB milestone.
See [TEST](TEST.md) for the full plan and the deferral note.
