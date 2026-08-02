# BallotBox Test Plan

## Unit Test Cases

### Approach

A unit test here exercises exactly one function.
Everything that function calls out to - the store, the crypto and PKI seams, the network transport - is replaced by a programmable substitute, so the case states the answers those collaborators give and then asserts the function's return value and the commands it issued.
No unit test needs SimpleDB, keys, or a socket, and none of them is scheduled behind those milestones.

The substitutes live in `tests/unit/support/fake_brain_seams.h` (store + daemon crypto/PKI) and `tests/unit/support/fake_client_seams.h` (transport + ballot crypto).
Because the libraries are static archives, a test that defines a seam symbol keeps the real implementation out of its binary, so no production code carries test hooks.
Each test file is its own binary; `make test` builds and runs them all.

Postconditions such as "no election created" or "store unchanged" are asserted as the exact commands that reached the store, since those commands are what a real store would act on.

### Decision table: vote command (UC-3 / UC-4)

| ~                                        | 1   | 2   | 3   | 4   | 5   |
| ---------------------------------------- | --- | --- | --- | --- | --- |
| joined                                   | N   | Y   | Y   | Y   | Y   |
| has prior ballot                         | -   | N   | N   | Y   | Y   |
| election open                            | -   | N   | Y   | N   | Y   |
| **actions**                              |     |     |     |     |     |
| must join first                          | X   |     |     |     |     |
| rejected (closed)                        |     | X   |     | X   |     |
| cast (UC-3), receipt                     |     |     | X   |     |     |
| update (UC-4), supersede + fresh receipt |     |     |     |     | X   |

### ballotd unit tests

| ID   | UC     | Technique     | Unit under test       | Purpose                               | Input / Setup                                                                              | Expected Output                                   | Postcondition                                    | Status | Test file                  |
| ---- | ------ | ------------- | --------------------- | ------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------------- | ------------------------------------------------ | ------ | -------------------------- |
| U-01 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election` | Valid config accepted   | Title, 2 options (min valid), close = open + 1h                                            | `BB_OK`                                           | One `INSERT_ELECTION`, id returned               | Done   | `test_brain_config`, `test_brain_create` |
| U-02 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election` | Option count below boundary | Config with 1 option, then 0 options                                                   | `BB_ERR_CONFIG_OPTIONS`                           | No write reaches the store                       | Done   | `test_brain_config`, `test_brain_create` |
| U-03 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election` | Empty title rejected    | Config with empty title                                                                    | `BB_ERR_CONFIG_TITLE`                             | No write reaches the store                       | Done   | `test_brain_config`, `test_brain_create` |
| U-04 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election` | Time window boundary    | close = open, then close = open - 1s                                                       | `BB_ERR_CONFIG_TIME`                              | No write reaches the store                       | Done   | `test_brain_config`, `test_brain_create` |
| U-05 | UC-1   | BVT           | `bb_validate_config` / `bb_create_election` | Minimum valid time window | close = open + 1s                                                                        | `BB_OK`                                           | One `INSERT_ELECTION`                            | Done   | `test_brain_config`, `test_brain_create` |
| U-06 | UC-1   | -             | `bb_transition_state` | Legal transition chain                | Stored election in DRAFT, then OPEN, then CLOSED; transition to the next state each time   | Each transition succeeds                          | One `UPDATE_STATE` per step, carrying the target | Done   | `test_brain_lifecycle`     |
| U-07 | -      | ECT           | `bb_is_legal_transition` / `bb_transition_state` | Illegal transitions rejected | One representative per illegal pair: PUBLISHED→OPEN, DRAFT→CLOSED, OPEN→DRAFT, CLOSED→OPEN | `BB_ERR_ILLEGAL_TRANSITION`      | No write in any case                             | Done   | `test_brain_lifecycle`     |
| U-08 | UC-5   | -             | `bb_publish_results`  | Publish requires CLOSED               | Stored election OPEN, DRAFT, PUBLISHED (one case each)                                     | Rejected                                          | No write; from CLOSED it writes `PUBLISHED`      | Done   | `test_brain_results`       |
| U-09 | UC-2   | ECT           | `bb_join`             | Join: election not found              | Store reports no such election                                                             | `BB_ERR_NOT_FOUND`                                | No session created (no write)                    | Done   | `test_brain_join`          |
| U-10 | UC-2   | ECT           | `bb_join`             | Join: election not open               | Eligible cert, election in DRAFT, CLOSED, PUBLISHED (one case each)                        | `BB_ERR_NOT_OPEN`                                 | No session created                               | Done   | `test_brain_join`          |
| U-11 | UC-2   | ECT           | `bb_join`             | Join: unlisted cert refused           | Valid cert not on eligible list, election OPEN                                             | `BB_ERR_NOT_ELIGIBLE`                             | No session created, no config returned           | Done   | `test_brain_join`          |
| U-12 | UC-2   | ECT           | `bb_join`             | Join: invalid or expired cert refused | Cert seam returns `EXPIRED`, then `INVALID`, then `NOT_ELIGIBLE`                           | The matching refusal code                         | No session created                               | Done   | `test_brain_join`          |
| U-13 | UC-2   | ECT           | `bb_join`             | Join: eligible voter admitted         | Cert on eligible list, election OPEN                                                       | `BB_OK`, election config returned                 | Voter admitted, can cast                         | Done   | `test_brain_join`          |
| U-14 | UC-3   | -             | `bb_record_ballot`    | Fresh nonce accepted                  | Ballot with unused nonce from an eligible voter                                            | `BB_OK`, receipt issued                           | Hash row appended v1, nonce marked used          | Done   | `test_brain_record`        |
| U-15 | UC-3   | -             | `bb_record_ballot`    | Replayed nonce rejected               | Same ballot submitted twice, store reports the nonce as seen                               | `BB_ERR_REPLAY`                                   | Exactly one ballot appended                      | Done   | `test_brain_record`        |
| U-16 | UC-3   | BVT           | `bb_record_ballot`    | Option index boundaries               | 3 options; decrypted index -1, 0, 2, 3                                                     | -1 and 3 rejected; 0 and 2 accepted               | Only valid ballots appended                      | Done   | `test_brain_record`        |
| U-17 | UC-3   | -             | `bb_record_ballot`    | Malformed ballot rejected             | Decrypt seam reports `BB_ERR_DECRYPT`                                                      | `BB_ERR_DECRYPT`                                  | Store unchanged, nonce not consumed, no receipt  | Done   | `test_brain_record`        |
| U-18 | UC-3   | -             | `bb_record_ballot`    | Ineligible ballot rejected at record  | Well-formed ballot whose cert is not on the eligible list                                  | `BB_ERR_NOT_ELIGIBLE`                             | Store unchanged, ciphertext never opened         | Done   | `test_brain_record`        |
| U-19 | UC-3/4 | DT rules 2, 4 | `bb_record_ballot`    | Submit after close rejected           | Election CLOSED; once with no prior ballot, once with prior ballot                         | `BB_ERR_CLOSED` in both cases                     | Store unchanged, prior ballot not superseded     | Done   | `test_brain_record`        |
| U-20 | UC-3   | -             | `bb_record_ballot`    | No double vote                        | Two distinct ballots from the same cert                                                    | Second stored as version 2                        | v1 superseded, one live ballot for that cert     | Done   | `test_brain_record`        |
| U-21 | UC-3   | -             | `db_exec`             | No ballot-to-voter link in logs       | Every op rendered with a distinctive cert in the command                                   | Cert appears in no log line; hash does            | Secrecy preserved, prior-ballot query binds cert | Done   | `test_brain_secrecy`       |
| U-22 | UC-3   | -             | `bb_record_ballot`    | Concurrency-safe recording            | 16 threads submit ballots for 16 distinct voters simultaneously                            | All 16 accepted                                   | 16 appends, 16 distinct hashes, all v1           | Done   | `test_brain_concurrency`   |
| U-23 | UC-4   | -             | `bb_record_ballot`    | Supersede on update                   | Store reports a v1 ballot for this voter                                                   | Fresh receipt hash issued, appended as v2         | v1 marked superseded                             | Done   | `test_brain_record`        |
| U-24 | UC-4   | -             | `bb_record_ballot`    | Repeated updates, latest counts       | Voter submits v1, v2, v3                                                                   | Fresh distinct receipt each time                  | v1 and v2 superseded in turn, v3 live            | Done   | `test_brain_record`        |
| U-25 | UC-5   | -             | `bb_get_results`      | Published view counts live only       | Published election, store's live set is 3 rows                                             | Tally sums to 3                                   | No superseded row in the view                    | Done   | `test_brain_results`       |
| U-26 | UC-5   | -             | `bb_get_results`      | Results gated before publish          | Results request while OPEN, then CLOSED, then DRAFT                                        | `BB_ERR_NOT_PUBLISHED` in every case              | Tally and hashes never read from the store       | Done   | `test_brain_results`       |
| U-27 | UC-5   | -             | `bb_get_results`      | Ineligible observer refused           | Results request from a cert outside the observer set                                       | `BB_ERR_NOT_ELIGIBLE`                             | No tally or hash data returned                   | Done   | `test_brain_results`       |
| U-28 | UC-5   | BVT           | `bb_publish_results` / `bb_get_results` | Zero-ballot publish | Election with 0 ballots, close + publish                                                   | Publish succeeds                                  | All-zero tally, empty hash list                  | Done   | `test_brain_results`       |
| U-29 | UC-6   | ECT           | `bb_lookup_hash`      | Lookup: counted hash found            | Hash of a live published ballot                                                            | `BB_OK`, row with the choice returned             | Query carried the voter's hash                   | Done   | `test_brain_lookup`        |
| U-30 | UC-6   | ECT           | `bb_lookup_hash`      | Lookup: superseded hash excluded      | Hash of a superseded ballot version (not in the live set)                                  | `BB_ERR_NOT_FOUND`                                | Dropped-ballot path triggered client side        | Done   | `test_brain_lookup`        |
| U-31 | UC-6   | ECT           | `bb_lookup_hash`      | Lookup: unknown hash not found        | Random hash never issued                                                                   | `BB_ERR_NOT_FOUND`, identical to U-30's answer    | Caller's buffer untouched, nothing leaked        | Done   | `test_brain_lookup`        |

### ballotu unit tests

| ID   | UC   | Technique | Unit under test    | Purpose                          | Input / Setup                                            | Expected Output                                          | Postcondition                                 | Status   | Test file          |
| ---- | ---- | --------- | ------------------ | -------------------------------- | -------------------------------------------------------- | -------------------------------------------------------- | --------------------------------------------- | -------- | ------------------ |
| U-32 | UC-3 | -         | `bu_encrypt_ballot` | RSA-OAEP round trip             | Known selection and election public key                  | Ciphertext decrypts to the selection                     | Plaintext never leaves client buffer          | Deferred | Crypto/PKI         |
| U-33 | UC-6 | -         | `bu_derive_receipt` | Deterministic receipt KDF       | Same secret ballot key twice                             | Same receipt hash both times                             | -                                             | Done     | `test_voter`       |
| U-34 | UC-6 | -         | `bu_derive_receipt` | Distinct keys, distinct hashes  | Two different secret keys                                | Different hashes, no collision on test corpus            | -                                             | Done     | `test_voter`       |
| U-35 | UC-2 | ECT       | `bu_join`           | Join: connection timeout handled | Transport seam reports a transport-level failure         | `BU_JOIN_TIMEOUT`                                        | No session state created, client still usable | Done     | `test_voter_flow`  |
| U-36 | UC-2 | ECT       | `bu_join`           | Join: not-open election handled | Daemon returns an election with a non-open state         | `BU_JOIN_NOT_OPEN`                                       | Election recorded locally, voter not joined   | Done     | `test_voter_flow`  |
| U-37 | UC-3 | DT rule 1 | `bu_route_vote` / `bu_submit_vote` | Vote before join blocked | `vote` with no joined election in the session          | `BU_MUST_JOIN` / `BB_ERR_NOT_JOINED`                     | Nothing encrypted, nothing sent               | Done     | `test_voter`, `test_voter_flow` |
| U-38 | UC-3 | DT rule 3 | `bu_route_vote` / `bu_submit_vote` | Cast flow selected      | Joined, `has_ballot` false                               | `CAST` request sent, receipt returned                    | `has_ballot` true, `my_hash` stored           | Done     | `test_voter`, `test_voter_flow` |
| U-39 | UC-4 | DT rule 5 | `bu_route_vote` / `bu_submit_vote` | Update flow selected    | Joined, `has_ballot` true                                | `UPDATE` request sent, fresh receipt                     | `ballot_version` incremented                  | Done     | `test_voter`, `test_voter_flow` |
| U-40 | UC-6 | ECT       | `bu_classify_check` | Dropped ballot flagged          | Daemon reports the derived hash as not found             | `BU_CHECK_DROPPED`                                       | Voter directed to the admin escalation path   | Done     | `test_voter`       |

Guard cases beyond the numbered rows are in the same files: validation ordering, store-failure propagation (a failed lookup is never reported to a voter as a dropped ballot), session reset on re-join, and the client pre-validator agreeing with the daemon's validator (`test_admin`).

### Traceability

Every error state and alternative flow is claimed by at least one test case.

| UC   | Error state / alternative flow (from README)            | Covered by             |
| ---- | ------------------------------------------------------- | ---------------------- |
| UC-1 | Invalid config: no title / no options / bad time window | U-02, U-03, U-04, I-08 |
| UC-2 | Admin IP/port unreachable (timeout)                     | U-35, I-10             |
| UC-2 | Election ID not found                                   | U-09, I-10             |
| UC-2 | Cert not on eligible list                               | U-11, U-18, I-10       |
| UC-2 | Cert invalid or expired                                 | U-12                   |
| UC-2 | Election not `OPEN` (added to list, shown not open)     | U-10, U-36, I-10       |
| UC-3 | Not joined → must join first                            | U-37                   |
| UC-3 | Alt 1a: already has final ballot → route to UC-4        | U-39                   |
| UC-3 | Election closed mid-submit → rejected                   | U-19, I-12             |
| UC-4 | Not joined → must join first                            | U-37                   |
| UC-4 | Alt 1a: no prior ballot → route to UC-3                 | U-38                   |
| UC-4 | Election closed mid-submit → rejected                   | U-19, I-12             |
| UC-5 | Observer not eligible → refused                         | U-27                   |
| UC-5 | Election not `PUBLISHED` → results not available        | U-26, I-14             |
| UC-6 | Alt 4a: hash not found → dropped ballot flagged         | U-30, U-31, U-40, I-15 |

---

## Integration Test Cases

### Strategy

```
Admin machine:   ballotctl ─▶ ballotd ─▶ SimpleDB
Voter (remote):  ballotu ──libtetrissh──▶ ballotd ─▶ SimpleDB
```

- Backend, **bottom-up**: `ballotd → SimpleDB`, storage first then handlers.
  The unit cases already pin the logic against the seam contract, so these cases check that the real store honours that contract.
- Frontend, **top-down**: clients against the `mock.c` daemon first, then the real `ballotd`, so a failure after unmocking isolates to the newly integrated component.
- Network: `ballotu → libtetrissh → ballotd` is the only network/encrypted edge, where wire-secrecy (I-11) is checked.

### Backend integration (ballotd + SimpleDB, bottom-up)

Precondition: clean SimpleDB seeded per case, wiped after.

| ID   | Call-graph edge    | UC   | Purpose                           | Input / Setup                        | Expected Output             | Postcondition                                          |
| ---- | ------------------ | ---- | --------------------------------- | ------------------------------------ | --------------------------- | ------------------------------------------------------ |
| I-01 | ballotd → SimpleDB | UC-1 | Election survives restart         | Create election, restart `ballotd`   | Election reloaded           | Config and state identical to before restart           |
| I-02 | ballotd → SimpleDB | UC-1 | Draft to Open persisted           | Create then open an election         | Transition succeeds         | DB row shows `OPEN`, subsequent fetch returns `OPEN`   |
| I-03 | ballotd → SimpleDB | UC-3 | Ballot and hash stored atomically | Record one ballot                    | Receipt matches stored hash | One ballot row, one hash row                           |
| I-04 | ballotd → SimpleDB | UC-4 | Version chain in store            | Cast then update                     | Update succeeds             | v1 `superseded=true`, v2 counted, only v2 in tally     |
| I-05 | ballotd → SimpleDB | UC-3 | Parallel inserts under load       | 50 concurrent casts across 50 voters | All accepted                | 50 ballots stored, no lost writes, no duplicate hashes |
| I-06 | ballotd → SimpleDB | UC-5 | Published view is consistent      | Close then publish                   | Publish succeeds            | DB tally equals count of non-superseded ballots        |

### Frontend integration (clients + ballotd, top-down)

Precondition: real `ballotd` with seeded elections.

| ID   | Call-graph edge                 | UC     | Purpose                     | Input / Setup                                           | Expected Output                                              | Postcondition                          |
| ---- | ------------------------------- | ------ | --------------------------- | ------------------------------------------------------- | ------------------------------------------------------------ | -------------------------------------- |
| I-07 | ballotctl → ballotd (local)     | UC-1   | Admin creates and opens     | Valid config via ballotctl                              | "instance is live" shown                                     | Daemon state `OPEN`                    |
| I-08 | ballotctl → ballotd (local)     | UC-1   | Invalid config surfaced     | Config with close time before open time                 | Specific error shown                                         | Election remains in `DRAFT` flow       |
| I-09 | ballotu → libtetrissh → ballotd | UC-2   | Eligible voter joins        | Correct IP/port/ID, eligible cert                       | Options displayed, joined confirmed                          | Voter admitted to session              |
| I-10 | ballotu → libtetrissh → ballotd | UC-2   | All four refusal partitions | Wrong host; unknown ID; ineligible cert; non-open state | Timeout / not found / not eligible / not open, each distinct | No session created in any branch       |
| I-11 | ballotu → libtetrissh → ballotd | UC-3   | Encrypted cast over session | Joined voter casts a vote                               | Receipt hash displayed                                       | Wire traffic is ciphertext only        |
| I-12 | ballotu → libtetrissh → ballotd | UC-3/4 | Close race (DT rules 2, 4)  | Voter submits as ballotctl closes the election          | Rejection shown to voter                                     | No partial ballot stored               |
| I-13 | ballotu → libtetrissh → ballotd | UC-5   | Published tally displayed   | Observer requests published election                    | Tally plus hash list grouped by option                       | -                                      |
| I-14 | ballotu → libtetrissh → ballotd | UC-5   | Results gated               | Observer requests `CLOSED` election                     | "results not available"                                      | No tally data leaves the daemon        |
| I-15 | ballotu → libtetrissh → ballotd | UC-6   | Hash lookup both branches   | Key of counted ballot; key of superseded ballot         | Found and counted; not found flagged as dropped              | Choice revealed only to the key holder |

---

## Timeline and milestones

### Unit tests

All unit cases run today except U-32, and they stay green as the seams are implemented: substituting a seam is how they are written, so a real store or real keys behind it changes nothing about the case.

| Milestone                                                       | Unit cases                     | What is missing                                                                                                                                                        |
| ----------------------------------------------------------------- | -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Available today** - all logic, seams substituted (`make test`) | U-01 .. U-31, U-33 .. U-40     | Nothing.                                                                                                                                                               |
| **Crypto/PKI** - real keys behind the crypto seams              | U-32, plus a re-run of U-33/U-34 | U-32 tests the RSA-OAEP round trip itself, which is the one thing a substitute cannot stand in for. U-33/U-34 pass against the placeholder KDF and are re-run for real. |

The seam implementations themselves (SimpleDB behind `db_exec`, libtetrissh behind `bcl_send`, OpenSSL behind the crypto seams) are covered by the integration cases below, not by unit cases.

### Integration tests

| Prerequisite feature                                              | Integration cases | Direction                        | Gate                                           |
| ----------------------------------------------------------------- | ----------------- | -------------------------------- | ---------------------------------------------- |
| SimpleDB behind `BallotStore.exec` (`db_exec`)                    | I-01..I-06        | Backend, bottom-up               | Persistence and concurrency cases green        |
| `ballotd` assembled + local admin channel                         | I-07, I-08        | Frontend, admin path (local)     | Invalid config surfaced; election opens        |
| Transport: `libtetrissh` behind `SecureSession.send` (`bcl_send`) | I-09..I-15        | Frontend, voter path (encrypted) | UC-2 refusal partitions + ciphertext-only wire |
