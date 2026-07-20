# BallotBox Test Plan

## Unit Test Cases

### ballotd unit tests

| ID   | UC     | Technique     | Purpose                               | Input / Setup                                                                              | Expected Output                                   | Postcondition                                    | Status   | Reason (partial / deferred)                                                  |
| ---- | ------ | ------------- | ------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------------- | ------------------------------------------------ | -------- | ---------------------------------------------------------------------------- |
| U-01 | UC-1   | BVT           | Valid config accepted                 | Title, 2 options (min valid), close = open + 1h                                            | `validateConfig` true                             | Election exists in `DRAFT`                       | Partial  | Validator asserted; DRAFT-existence postcondition needs DB readback          |
| U-02 | UC-1   | BVT           | Option count below boundary           | Config with 1 option, then 0 options                                                       | Rejected with specific error                      | No election created                              | Done     | -                                                                            |
| U-03 | UC-1   | BVT           | Empty title rejected                  | Config with empty title                                                                    | Rejected with specific error                      | No election created                              | Done     | -                                                                            |
| U-04 | UC-1   | BVT           | Time window boundary                  | close = open, then close = open - 1s                                                       | Rejected with specific error                      | No election created                              | Done     | -                                                                            |
| U-05 | UC-1   | BVT           | Minimum valid time window             | close = open + 1s                                                                          | Accepted                                          | Election exists in `DRAFT`                       | Partial  | Validator asserted; DRAFT-existence postcondition needs DB readback          |
| U-06 | UC-1   | -             | Legal transition chain                | `DRAFT` election; transition OPEN, then CLOSED, then PUBLISHED                             | Each transition succeeds                          | Final state `PUBLISHED`                          | Partial  | Legality asserted; "final state PUBLISHED" needs DB readback                 |
| U-07 | -      | ECT           | Illegal transitions rejected          | One representative per illegal pair: PUBLISHED→OPEN, DRAFT→CLOSED, OPEN→DRAFT, CLOSED→OPEN | Each rejected                                     | State unchanged in every case                    | Done     | -                                                                            |
| U-08 | UC-5   | -             | Publish requires CLOSED               | `OPEN` election, `publishResults` called                                                   | Rejected                                          | State stays `OPEN`, no result view created       | Deferred | DB readback (current-state gate)                                             |
| U-09 | UC-2   | ECT           | Join: election not found              | Join request with unknown election ID                                                      | "election not found" error                        | No session created                               | Deferred | DB readback (election existence)                                             |
| U-10 | UC-2   | ECT           | Join: election not open               | Join request for election in `DRAFT`, `CLOSED`, `PUBLISHED` (one case each)                | Refused, "not open" with current state            | No session created                               | Deferred | DB readback (election state)                                                 |
| U-11 | UC-2   | ECT           | Join: unlisted cert refused           | Valid cert not on eligible list, election `OPEN`                                           | Refused, reason "not eligible"                    | No session created                               | Partial  | `bb_check_eligibility` asserted; full join-refusal path needs DB readback    |
| U-12 | UC-2   | ECT           | Join: invalid or expired cert refused | Cert with `EXPIRED` status, then `INVALID`                                                 | `verifyCert` returns the status, session refused  | No session created                               | Deferred | Crypto/PKI (real X.509 verify; placeholder returns VALID)                    |
| U-13 | UC-2   | ECT           | Join: eligible voter admitted         | Cert on eligible list, election `OPEN`                                                     | `checkEligibility` true, election config returned | Voter admitted, can cast                         | Partial  | `bb_check_eligibility` asserted; admission + config return needs DB readback |
| U-14 | UC-3   | -             | Fresh nonce accepted                  | Ballot with unused nonce from admitted voter                                               | Ballot accepted                                   | Nonce marked used                                | Deferred | DB nonce store (`NONCE_SEEN`)                                                |
| U-15 | UC-3   | -             | Replayed nonce rejected               | Same encrypted ballot submitted twice                                                      | Second submission rejected                        | Exactly one ballot recorded                      | Deferred | DB nonce store (`NONCE_SEEN`)                                                |
| U-16 | UC-3   | BVT           | Option index boundaries               | n options; index -1, 0, n-1, n                                                             | -1 and n rejected; 0 and n-1 accepted             | Only valid ballots recorded                      | Deferred | DB readback (`option_count`); optional pure-helper not extracted             |
| U-17 | UC-3   | -             | Malformed ballot rejected             | Payload that fails RSA-OAEP decryption (garbage bytes)                                     | Rejected with decryption error                    | Store unchanged, nonce not consumed              | Deferred | Crypto (RSA-OAEP decrypt failure; placeholder never fails)                   |
| U-18 | UC-3   | -             | Ineligible ballot rejected at record  | Well-formed ballot whose cert is not on the eligible list                                  | Rejected (eligibility re-checked at record time)  | Store unchanged                                  | Deferred | DB readback (stored election at record time)                                 |
| U-19 | UC-3/4 | DT rules 2, 4 | Submit after close rejected           | Ballot arrives when state is `CLOSED`; once with no prior ballot, once with prior ballot   | Rejected in both cases                            | Store unchanged, prior ballot still counted      | Deferred | DB readback (current state + prior ballot)                                   |
| U-20 | UC-3   | -             | No double vote                        | Two distinct first-time ballots from the same cert                                         | Second stored as version 2                        | Tally counts exactly one ballot for that cert    | Deferred | DB readback (ballot versioning)                                              |
| U-21 | UC-3   | -             | No ballot-to-voter link in logs       | Record a ballot, inspect log output                                                        | No (cert, ballot/hash) pairing in any log line    | Secrecy preserved                                | Done     | -                                                                            |
| U-22 | UC-3   | -             | Concurrency-safe recording            | N threads submit ballots for distinct voters simultaneously                                | All N accepted                                    | Exactly N ballots, tally = N, no corruption      | Deferred | DB store + write mutex (concurrency unimplemented)                           |
| U-23 | UC-4   | -             | Supersede on update                   | Voter with ballot v1 submits v2                                                            | Fresh receipt hash issued                         | v1 `superseded`, only v2 counted                 | Deferred | DB readback (ballot versioning)                                              |
| U-24 | UC-4   | -             | Repeated updates, latest counts       | Voter submits v1, v2, v3                                                                   | Fresh receipt each time                           | v1 and v2 `superseded`, tally counts only v3     | Deferred | DB readback (ballot versioning)                                              |
| U-25 | UC-5   | -             | Tally counts latest only              | 3 voters, one updated once, then close + publish                                           | Tally = 3 ballots                                 | Superseded version excluded from result view     | Deferred | DB readback (tally)                                                          |
| U-26 | UC-5   | -             | Results gated before publish          | Results request while election is `OPEN`, then `CLOSED`                                    | "results not available" in both cases             | No tally or hash data returned                   | Deferred | DB readback (current-state gate)                                             |
| U-27 | UC-5   | -             | Ineligible observer refused           | Results request from cert outside the observer set                                         | Refused, reason "not eligible"                    | No tally or hash data returned                   | Deferred | DB readback (observer eligibility on stored election)                        |
| U-28 | UC-5   | BVT           | Zero-ballot publish                   | Election with 0 ballots, close + publish                                                   | Publish succeeds                                  | All-zero tally, empty hash list displayed safely | Deferred | DB readback (tally)                                                          |
| U-29 | UC-6   | ECT           | Lookup: counted hash found            | Hash of a live (non-superseded) published ballot                                           | Found, counted, choice returned                   | -                                                | Deferred | DB readback (`FIND_HASH`)                                                    |
| U-30 | UC-6   | ECT           | Lookup: superseded hash excluded      | Hash of a superseded ballot version                                                        | Not found                                         | Dropped-ballot path triggered client side        | Deferred | DB readback (`FIND_HASH`)                                                    |
| U-31 | UC-6   | ECT           | Lookup: unknown hash not found        | Random hash never issued                                                                   | Not found                                         | No information leaked about other ballots        | Deferred | DB readback (`FIND_HASH`)                                                    |

### ballotu unit tests

| ID   | UC   | Technique | Purpose                          | Input / Setup                                            | Expected Output                                          | Postcondition                                 | Status   | Reason (partial / deferred)                                            |
| ---- | ---- | --------- | -------------------------------- | -------------------------------------------------------- | -------------------------------------------------------- | --------------------------------------------- | -------- | ---------------------------------------------------------------------- |
| U-32 | UC-3 | -         | RSA-OAEP round trip              | Known selection and election public key                  | Ciphertext decrypts to the selection                     | Plaintext never leaves client buffer          | Deferred | Crypto (real RSA-OAEP; only a payload[0] placeholder exists)           |
| U-33 | UC-6 | -         | Deterministic receipt KDF        | Same secret ballot key twice                             | Same receipt hash both times                             | -                                             | Done     | Placeholder-grade (djb2 KDF; no crypto collision guarantee)            |
| U-34 | UC-6 | -         | Distinct keys, distinct hashes   | Two different secret keys                                | Different hashes, no collision on test corpus            | -                                             | Done     | Placeholder-grade (djb2 KDF; no crypto collision guarantee)            |
| U-35 | UC-2 | ECT       | Join: connection timeout handled | Stub simulates unreachable host (timeout / no route)     | "join failed, connection timeout" shown                  | No session state created, client still usable | Done     | -                                                                      |
| U-36 | UC-2 | ECT       | Join: not-open election handled  | Stub returns election with non-open state                | "not open" shown                                         | Election added to local list (per UC-2 flow)  | Partial  | `bu_classify_join` asserted; "added to local list" is session/UI state |
| U-37 | UC-3 | DT rule 1 | Vote before join blocked         | `vote` with no joined election in `VoterSession`         | "must join first" shown                                  | Nothing sent to server                        | Done     | -                                                                      |
| U-38 | UC-3 | DT rule 3 | Cast flow selected               | Joined, `hasBallot` false                                | Cast flow (UC-3), receipt displayed                      | `hasBallot` true, `myHash` stored             | Done     | -                                                                      |
| U-39 | UC-4 | DT rule 5 | Update flow selected             | Joined, `hasBallot` true                                 | Update flow (UC-4), fresh receipt displayed              | `ballotVersion` incremented                   | Done     | -                                                                      |
| U-40 | UC-6 | -         | Dropped ballot flagged           | Valid key; stub returns "not found" for the derived hash | "verification failed (dropped ballot), raise with Admin" | Voter directed to the admin escalation path   | Deferred | Needs client check-mapping helper (`bu_classify_check`), not yet added |

### Traceability

Every error state and alternative flow is claimed by at least one test case.

| UC   | Error state / alternative flow (from README)            | Covered by             |
| ---- | ------------------------------------------------------- | ---------------------- |
| UC-1 | Invalid config: no title / no options / bad time window | U-02, U-03, U-04, I-08 |
| UC-2 | Admin IP/port unreachable (timeout)                     | U-35, I-10             |
| UC-2 | Election ID not found                                   | U-09, I-10             |
| UC-2 | Cert not on eligible list                               | U-11, U-18, I-10       |
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
Voter (remote):  ballotu ──libtetrissh──▶ ballotd ─▶ …
```

- Backend, **bottom-up**: `ballotd → SimpleDB`, storage first then handlers, reusing the deferred DB unit cases (U-08..U-31) as drivers against the real store.
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

### Unit testse

| Milestone                                                                   | Unit cases                                                                                                 | Notes                                                                                                                                                       |
| --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Available today** - logic libraries, seams stubbed (green in `make test`) | U-02, U-03, U-04, U-07, U-21, U-33, U-34, U-35, U-37, U-38, U-39                                           | Pure logic + placeholder crypto. U-33 / U-34 are placeholder-grade (no crypto guarantee).                                                                   |
| **Available today** - partial (pure part green; postcondition deferred)     | U-01, U-05, U-06, U-11, U-13, U-36                                                                         | Decision / predicate asserted now; each row's Reason column names the seam its postcondition waits on.                                                      |
| **After SimpleDB** - DB readback + store                                    | U-08, U-09, U-10, U-14, U-15, U-16, U-18, U-19, U-20, U-23, U-24, U-25, U-26, U-27, U-28, U-29, U-30, U-31 | Also completes the deferred postconditions of the six partial cases above.                                                                                  |
| **After SimpleDB + write mutex** - concurrency                              | U-22                                                                                                       | Needs the store _and_ the R1 write lock.                                                                                                                    |
| **After crypto / PKI**                                                      | U-12, U-17, U-32                                                                                           | X.509 verify (U-12), RSA-OAEP decrypt failure (U-17), real OAEP round-trip (U-32).                                                                          |
| **Deferred by design**                                                      | U-40                                                                                                       | Needs the client check-mapping helper (`bu_classify_check`), not yet added; U-16's optional early-unblock helper likewise, so it stays in the SimpleDB row. |

### Integration tests

| Prerequisite feature                         | Integration cases | Direction                                                     | Gate                                           |
| -------------------------------------------- | ----------------- | ------------------------------------------------------------- | ---------------------------------------------- |
| SimpleDB backing the `db_exec` seam          | I-01..I-06        | Backend, bottom-up (deferred DB unit cases reused as drivers) | Persistence and concurrency cases green        |
| `ballotd` assembled + local admin channel    | I-07, I-08        | Frontend, admin path (local, no `libtetrissh`)                | Invalid config surfaced; election opens        |
| `libtetrissh` session wired (voter ↔ daemon) | I-09..I-15        | Frontend, voter path (remote, encrypted)                      | UC-2 refusal partitions + ciphertext-only wire |
