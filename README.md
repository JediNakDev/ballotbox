# BallotBox

> [!IMPORTANT]
> Security claims involving ballot RSA-OAEP encryption, signed receipt commitments, receipt-key KDFs, and X.509 certificate verification describe the intended production design.
> The current crypto and PKI seams use deterministic placeholders while preserving the interfaces and flows shown below.
> Transport encryption and voter account authentication are implemented through tetriSH and tetrisauth.

BallotBox is a secure e-voting system for small orgs (clubs, coops, unions) solving the core tension: ballots must be secret (untraceable to voters) yet verifiable (tally is auditable). Built on the 50.005 CoreStack/tetriSH libraries, it delivers a CLI-based voter client and admin control tool (ballotctl) over a custom C backend (ballotd), communicating via SSH/shell sessions. Key guarantees: encrypted traffic, no double-votes, no ballot-to-voter linkage in logs, and concurrency-safe tallying.

---

1009098 Pitchayut Ariyachansil (Jedi)
1009164 Phatsakorn Ukanchanakitti (Pop)
1009195 Popsuk Sumetchoengprachya (Kenji)

---

- Admin: a person who runs and manages e-voting.
- Voter: a person who casts a vote.
- Observer: a person who is eligible to observe the voting results, under usual circumstances, a voter and an admin are also an observer.
- System: a backend service that runs and manages ballotbox programmes.
- Client: a frontend service or a GUI that voter or admin interact.
- ballotctl: an admin side client system program, a part of BallotBox.
- ballotu: a voter side client system program, a part of BallotBox.
- ballotd: a system, runs on admin machine, a part of BallotBox.
- SimpleDB: a DBMS, a project lab from 50.043 Database Systems, a part of BallotBox.

---

## Use Case Diagram

```mermaid
flowchart LR
    Admin([Admin])
    Voter([Voter])
    Observer([Observer])

    subgraph BB["BallotBox"]
        UC1((Instantiate<br/>BallotBox))
        UC2((Join<br/>BallotBox))
        UC3((Cast a Vote))
        UC4((Update a Vote))
        UC5((View Results))
        UC6((Check Your Vote))
        UC7((Close Election))
        UC8((Publish Results))
    end

    Admin --- UC1
    Admin --- UC5
    Admin --- UC6
    Admin --- UC7
    Admin --- UC8
    Voter --- UC2
    Voter --- UC3
    Voter --- UC4
    Voter --- UC6
    Observer --- UC5
```

## Use Cases

### UC-1: Instantiate BallotBox

| Field          | Detail                                                             |
| -------------- | ------------------------------------------------------------------ |
| Description    | Admin creates a new election instance and opens it for voting.     |
| Actors         | Admin                                                              |
| Triggers       | Admin runs the create-election command in ballotctl.               |
| Preconditions  | Admin can access ballotd's owner-only local control socket.        |
| Postconditions | Election is Open and accepting voters.                             |

**Flow**

1. Admin fills in the title, options, eligible-voter certs, and open/close times in ballotctl.
2. ballotd validates the configuration (title and options required, close time after open time).
3. ballotd inserts the election into SimpleDB in 'draft'.
4. Admin runs Open Election in ballotctl and selects the 'draft' election.
5. ballotd updates the election status to 'open' in SimpleDB and begins accepting voters; ballotctl confirms the instance is live.

**Alternative Flows**

None.

**Error States**

2a. Invalid config (no title/options, or close time ≤ open time) → rejected with a specific error; stays in 'draft'.

```mermaid
sequenceDiagram
    actor Admin
    participant ballotctl
    participant ballotd
    participant SimpleDB

    note over Admin, SimpleDB: Precondition: ballotd local control socket reachable

    alt invalid config
        Admin->>+ballotctl: fill in configuration
        ballotctl->>+ballotd: submit configuration over local control socket
        ballotd->>ballotd: validate configuration
        ballotd-->>-ballotctl: specific error, stays in 'draft'
        ballotctl-->>-Admin: show error (fix and retry)
    else valid config
        Admin->>+ballotctl: fill in configuration
        ballotctl->>+ballotd: submit configuration over local control socket
        ballotd->>ballotd: validate configuration
        ballotd->>+SimpleDB: insert ballotbox
        SimpleDB-->>-ballotd: insert success
        ballotd-->>-ballotctl: election created in 'draft'
        ballotctl-->>-Admin: show create success
        Admin->>+ballotctl: Open Election (select 'draft' election)
        ballotctl->>+ballotd: request transition to 'open'
        ballotd->>+SimpleDB: update status to 'open'
        SimpleDB-->>-ballotd: update success
        ballotd-->>-ballotctl: instance is live
        ballotctl-->>-Admin: show instance is live
        note over Admin,SimpleDB: Postcondition: election is 'open' and accepting voters
    end
```

### UC-2: Join BallotBox

| Field          | Detail                                                  |
| -------------- | ------------------------------------------------------- |
| Description    | An eligible voter joins an open election instance.      |
| Actors         | Voter                                                   |
| Triggers       | Voter runs the join command.                            |
| Preconditions  | Authenticated voter account over a secure tetriSH session. |
| Postconditions | Voter is admitted to the session and can cast a ballot. |

**Flow**

1. Voter connects to ballotd, logs in or registers, and enters the election ID in ballotu.
2. ballotu connects to ballotd; ballotd fetches the election from SimpleDB.
3. ballotd checks the server-confirmed username against the eligible-voter list.
4. ballotd confirms the election is 'open' and admits the voter to the session.
5. ballotu displays the ballot options and confirms the voter has joined.

**Alternative Flows**

4a. Election not 'open' → cannot join yet; ballotu saves the election for later (UC-5, UC-6).

**Error States**

2a. ballotd unreachable (timeout / no route to host) → join failed.
2b. Election not found → join failed.
3a. Authenticated username not on the eligible list → refused.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: authenticated voter account over tetriSH

    alt admin IP/port not found or refuse to connect
        Voter->>+ballotu: enter election details
        ballotu--xballotd: connect to server
        note over ballotu: timeout / no route to host
        ballotu-->>-Voter: join failed, connection timeout
    else election not found
        Voter->>+ballotu: enter election details
        ballotu->>+ballotd: connect to server
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: election does not exist
        ballotd-->>-ballotu: election not found
        ballotu-->>-Voter: join failed, election not found
    else username not on eligible list
        Voter->>+ballotu: enter election details
        ballotu->>+ballotd: connect to server
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd-->>-ballotu: refused (username not eligible)
        ballotu-->>-Voter: show refusal (not eligible)
    else election not 'open'
        Voter->>+ballotu: enter election details
        ballotu->>+ballotd: connect to server
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd-->>-ballotu: return election
        ballotu->>ballotu: add election to list
        ballotu-->>-Voter: show election not 'open'
    else eligible and Open
        Voter->>+ballotu: enter election details
        ballotu->>+ballotd: connect to server
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd-->>-ballotu: return election config
        ballotu-->>-Voter: display options, confirm joined
        note over Voter,SimpleDB: Postcondition: voter admitted to the session, can cast a ballot
    end
```

### UC-3: Cast a Vote

| Field          | Detail                                                                                        |
| -------------- | --------------------------------------------------------------------------------------------- |
| Description    | Voter submits a secret ballot and receives a verifiable receipt (hash).                       |
| Actors         | Voter                                                                                         |
| Triggers       | Voter runs the vote command before the close time.                                            |
| Preconditions  | Authenticated voter session.                                                                  |
| Postconditions | One authoritative ballot recorded; receipt hash issued; no log links the ballot to the voter. |

**Flow**

1. ballotu checks the voter has joined an 'open' election.
2. ballotu displays the ballot options; Voter selects one and confirms.
3. ballotu encrypts the ballot (RSA-OAEP) with a fresh anti-replay nonce and submits it to ballotd.
4. ballotd verifies the nonce and eligibility and inserts the ballot into SimpleDB.
5. ballotd issues a signed receipt hash; ballotu displays it for the voter to keep (UC-6).

**Alternative Flows**

1b. Voter already has a final ballot → route to UC-4 (Update a Vote).

**Error States**

1a. Voter has not joined the election → must join first (UC-2).
4a. Election closed mid-submission → ballotd rejects it; ballotu shows the rejection.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: authenticated voter session

    alt not joined
        ballotu->>ballotu: check the voter and election
        note over ballotu: voter hasn't joined the election
        ballotu-->>Voter: must join first (UC-2)
    else already has a final ballot
        ballotu->>ballotu: check the voter and election
        note over ballotu: voter has already cast a vote
        ballotu-->>Voter: route to UC-4 (update)
    else election closed mid submission
        ballotu->>ballotu: check the voter and election
        ballotu-->>Voter: display ballot options
        Voter->>+ballotu: select option, confirm
        ballotu->>ballotu: encrypt ballot (RSA-OAEP) + anti-replay nonce
        ballotu->>+ballotd: submit encrypted ballot
        note over ballotd: election has been closed
        ballotd-->>-ballotu: rejected
        ballotu-->>-Voter: show rejection
    else ready to vote
        ballotu->>ballotu: check the voter and election
        ballotu-->>Voter: display ballot options
        Voter->>+ballotu: select option, confirm
        ballotu->>ballotu: encrypt ballot (RSA-OAEP) + anti-replay nonce
        ballotu->>+ballotd: submit encrypted ballot
        ballotd->>ballotd: verify nonce/eligibility
        ballotd->>+SimpleDB: insert ballot
        SimpleDB-->>-ballotd: insert success
        ballotd-->>-ballotu: signed receipt (verification hash)
        ballotu-->>-Voter: display receipt hash, success (keep for UC-6)
        note over Voter,SimpleDB: Postcondition: one authoritative ballot recorded, receipt issued, no ballot-to-voter link in logs
    end
```

### UC-4: Update a Vote

| Field          | Detail                                                                                        |
| -------------- | --------------------------------------------------------------------------------------------- |
| Description    | Voter re-casts to override a prior selection while voting is still open.                      |
| Actors         | Voter                                                                                         |
| Triggers       | Voter runs update vote before the close time.                                                 |
| Preconditions  | Authenticated voter session.                                                                  |
| Postconditions | Tally counts only the latest ballot version; a new receipt hash is issued; secrecy preserved. |

**Flow**

1. ballotu checks the voter has joined an 'open' election and has a prior ballot.
2. ballotu displays the options, noting a prior ballot exists; Voter selects a new option and confirms.
3. ballotu encrypts the new ballot (RSA-OAEP) with a fresh anti-replay nonce and submits it to ballotd.
4. ballotd inserts it into SimpleDB under a higher version and marks the previous receipt superseded (only the latest version counts).
5. ballotd issues a fresh receipt hash; ballotu displays the updated success message.

**Alternative Flows**

1b. Voter has no prior ballot → route to UC-3 (Cast a Vote).

**Error States**

1a. Voter has not joined the election → must join first (UC-2).
4a. Election closed mid-submission → ballotd rejects it; ballotu shows the rejection.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: authenticated voter session

    alt not joined
        ballotu->>ballotu: check the voter and election
        note over ballotu: voter hasn't joined the election
        ballotu-->>Voter: must join first (UC-2)
    else no prior ballot
        ballotu->>ballotu: check the voter and election
        note over ballotu: voter has no prior ballot
        ballotu-->>Voter: route to UC-3 (cast a vote)
    else election closed mid submission
        ballotu->>ballotu: check the voter and election
        ballotu-->>Voter: display options, note prior ballot exists
        Voter->>+ballotu: select new option, confirm
        ballotu->>ballotu: encrypt new ballot (RSA-OAEP) + anti-replay nonce
        ballotu->>+ballotd: submit new encrypted ballot
        note over ballotd: election has been closed
        ballotd-->>-ballotu: rejected
        ballotu-->>-Voter: show rejection
    else prior ballot exists
        ballotu->>ballotu: check the voter and election
        ballotu-->>Voter: display options, note prior ballot exists
        Voter->>+ballotu: select new option, confirm
        ballotu->>ballotu: encrypt new ballot (RSA-OAEP) + anti-replay nonce
        ballotu->>+ballotd: submit new encrypted ballot
        ballotd->>ballotd: verify nonce/eligibility
        ballotd->>+SimpleDB: insert ballot (higher version), mark previous superseded
        SimpleDB-->>-ballotd: update success
        ballotd-->>-ballotu: signed fresh receipt (verification hash)
        ballotu-->>-Voter: display new receipt hash, success (keep for UC-6)
        note over Voter,SimpleDB: Postcondition: only the latest ballot version counts, fresh receipt issued, secrecy preserved
    end
```

### UC-5: View Result

| Field          | Detail                                                                         |
| -------------- | ------------------------------------------------------------------------------ |
| Description    | An eligible observer or the local admin views the published tally and ballot hashes. |
| Actors         | Observer, Admin                                                                |
| Triggers       | Observer or Admin runs the results command.                                    |
| Preconditions  | Authenticated voter account, or access to ballotd's local admin socket.        |
| Postconditions | The final tally and the full list of ballot verification hashes are displayed. |

**Flow**

1. Observer selects an election in ballotu, or Admin enters an election ID in ballotctl.
2. ballotd fetches the election from SimpleDB and checks observer eligibility. The local admin path bypasses the eligible-voter check.
3. ballotd confirms the election is 'published' and returns the tally and the counted ballot hashes from SimpleDB.
4. The selected client displays the tally and hash list, grouped by option.

**Alternative Flows**

None.

**Error States**

2a. Observer not eligible → refused.
3a. Election not 'published' → results not available.

```mermaid
sequenceDiagram
    actor Observer
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Observer,ballotd: Precondition: authenticated voter account; admin uses the local control socket

    alt Observer not eligible
        Observer->>+ballotu: select election
        ballotu->>+ballotd: request results
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd->>ballotd: check observer eligibility
        note over ballotd: observer is not eligible
        ballotd-->>-ballotu: refused (not eligible)
        ballotu-->>-Observer: show refusal (not eligible)
    else election not 'published'
        Observer->>+ballotu: select election
        ballotu->>+ballotd: request results
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd->>ballotd: check observer eligibility
        note over ballotd: election is not 'published'
        ballotd-->>-ballotu: results not available
        ballotu-->>-Observer: show "results not available"
    else 'published'
        Observer->>+ballotu: select election
        ballotu->>+ballotd: request results
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd->>ballotd: check observer eligibility
        ballotd->>+SimpleDB: fetch tally and counted ballot hashes
        SimpleDB-->>-ballotd: return tally + hashes
        ballotd-->>-ballotu: tally + counted ballot hashes (grouped by option)
        ballotu-->>-Observer: display tally and hash list
        note over Observer,SimpleDB: Postcondition: final tally and full ballot hash list displayed
    end
```

### UC-6: Check Your Vote

| Field          | Detail                                                                                  |
| -------------- | --------------------------------------------------------------------------------------- |
| Description    | A voter confirms their own ballot was counted, using their secret ballot key.           |
| Actors         | Voter                                                                                   |
| Triggers       | Voter runs the check command.                                                           |
| Preconditions  | Caller holds a secret ballot key from UC-3/UC-4.                                        |
| Postconditions | Voter confirms inclusion of their ballot without revealing their choice to anyone else. |

**Flow**

1. Voter enters their secret ballot key in ballotu.
2. ballotu derives the receipt hash from the key (hash function / KDF).
3. ballotu sends the derived hash to ballotd to look up in the live ballot-hash set.
4. ballotd searches the non-superseded hashes in SimpleDB. This lookup works before and after publication.
5. ballotu reports the voter's ballot was included and shows their recorded choice.

**Alternative Flows**

None.

**Error States**

4a. Derived hash not found in the live ballot-hash set → verification failed; ballotu flags it as a dropped ballot for the voter to raise with the Admin.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: holds a secret ballot key (UC-3/UC-4)

    alt hash not found
        Voter->>+ballotu: enter secret ballot key
        ballotu->>ballotu: derive receipt hash from key (hash function / KDF)
        ballotu->>+ballotd: look up derived hash
        ballotd->>+SimpleDB: search live, non-superseded hashes
        SimpleDB-->>-ballotd: hash not found
        ballotd-->>-ballotu: not found
        ballotu-->>-Voter: verification failed (dropped ballot), raise with Admin
    else hash found
        Voter->>+ballotu: enter secret ballot key
        ballotu->>ballotu: derive receipt hash from key (hash function / KDF)
        ballotu->>+ballotd: look up derived hash
        ballotd->>+SimpleDB: search live, non-superseded hashes
        SimpleDB-->>-ballotd: hash found
        ballotd-->>-ballotu: found, counted in tally (with choice)
        ballotu-->>-Voter: ballot included and counted
        note over Voter,SimpleDB: Postcondition: inclusion confirmed without revealing the choice to anyone else
    end
```

---

### UC-7: Close Election

The local administrator closes an `OPEN` election through `ballotctl` and ballotd's owner-only control socket.
The successful transition persists `CLOSED`, after which both casts and updates are rejected without changing the live ballot set.
Closing from `DRAFT`, `CLOSED`, or `PUBLISHED` is an illegal transition and leaves the stored state unchanged.

### UC-8: Publish Results

The local administrator publishes a `CLOSED` election through the same control socket.
The successful transition persists terminal state `PUBLISHED` and exposes the final tally and live receipt hashes through UC-5.
Publishing from `DRAFT`, `OPEN`, or `PUBLISHED` is an illegal transition and leaves the stored state unchanged.

The legal lifecycle is `DRAFT -> OPEN -> CLOSED -> PUBLISHED`.
Every other ordered transition, including a self-transition, is rejected.

---

## Class Diagram

BallotBox is implemented in C, which has no classes, so every class below maps to a struct, an opaque context handle, or a module (a header plus its translation units).

### Domain Class Diagram

```mermaid
classDiagram
    class Actor {
        -certName
    }
    class Admin
    class Voter
    class Observer

    class Certificate {
        -name
        -status
    }

    class Election {
        -id
        -title
        -state
        -options
        -eligibleVoters
        -openTime
        -closeTime
        -tally
    }

    class Ballot {
        -certName
        -nonce
        -encryptedPayload
        -payloadLen
    }

    class BallotHash {
        -hash
        -optionIndex
        -version
        -superseded
    }

    class Receipt {
        -hash
        -issuedAt
    }

    class VoterSession {
        -certName
        -joined
        -hasBallot
        -ballotVersion
        -myHash
        -title
        -options
        -optionCount
    }

    class PublishedResults {
        -title
        -tally
        -options
        -ballotHashes
    }

    class BallotOwner {
        -electionId
        -certName
        -currentHash
        -version
    }

    class ElectionState {
        <<enumeration>>
        DRAFT
        OPEN
        CLOSED
        PUBLISHED
    }

    class CertStatus {
        <<enumeration>>
        INVALID
        EXPIRED
        NOT_ELIGIBLE
        VALID
    }

    Admin --|> Actor
    Voter --|> Actor
    Observer --|> Actor

    Actor "1" -- "1" Certificate : identified by
    Admin "1" -- "*" Election : manages
    Voter "1" -- "1" VoterSession : has
    VoterSession "*" -- "1" Election : joins
    Observer "*" -- "*" Election : observes
    Election "1" *-- "*" Ballot : accepts
    Election "1" *-- "*" BallotHash : records
    Election "1" -- "0..1" PublishedResults : publishes
    Election "1" -- "*" BallotOwner : tracks privately
    Ballot ..> BallotHash : produces
    BallotHash ..> Receipt : returned as
    Election "1" -- "1" ElectionState : state
    Certificate "1" -- "1" CertStatus : status
```

`BallotHash` is the only per-ballot entity published in results, and it holds no voter identity.
The store keeps the voter-to-current-hash association in the separate, private `BallotOwner` mapping used for updates.

### Solution Class Diagram: voter client (ballotu)

```mermaid
classDiagram
    class VoterUI {
        -formData: VoterFormData
        -session: VoterSession
        +show(msg: String)
    }

    class VoterFormData {
        -host: String
        -port: int
        -electionId: String
        -optionIndex: int
        -secretKey: String
    }

    class VoterSession {
        -certName: String
        -joined: boolean
        -electionId: String
        -hasBallot: boolean
        -ballotVersion: int
        -myHash: String
        -title: String
        -options: List~String~
        -optionCount: int
    }

    class VoterController {
        +join(session: VoterSession, electionId: String, username: String) JoinOutcome
        +routeVote(s: VoterSession) VoteAction
        +submitVote(s: VoterSession, optionIndex: int, nonce: String) Receipt
        +deriveReceipt(secretKey: String) String
    }

    class ResultsController {
        +buildResultsRequest(id: String, username: String) BallotRequest
        +buildCheckRequest(id: String, hash: String) BallotRequest
    }

    class ClientCrypto {
        +encryptBallot(optionIndex: int, nonce: String) Ballot
        +deriveReceiptHash(secretKey: String) String
    }

    class SecureSession {
        +connect(host: String, port: int, caPath: String) ResultStatus
        +authenticate(method: String, username: String, password: String) int
        +disconnect()
        +send(req: BallotRequest) BallotResponse
    }

    class BallotRequest {
        -op: RequestOp
        -certName: String
        -electionId: String
        -ballot: Ballot
        -hash: String
        -config: ElectionConfig
    }

    class BallotResponse {
        -status: ResultStatus
        -election: Election
        -receipt: Receipt
        -hasPriorBallot: boolean
        -priorBallotVersion: int
        -tally: List~int~
        -optionCount: int
        -options: List~String~
        -hashes: List~BallotHash~
        -hashCount: int
        -found: boolean
        -foundOption: int
        -foundOptionName: String
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
    }

    class CheckOutcome {
        <<enumeration>>
        COUNTED
        DROPPED
        UNAVAILABLE
    }

    class VoteAction {
        <<enumeration>>
        MUST_JOIN
        CAST
        UPDATE
    }

    class JoinOutcome {
        <<enumeration>>
        TIMEOUT
        NOT_FOUND
        NOT_ELIGIBLE
        NOT_OPEN
        ADMITTED
    }

    VoterUI "1" -- "1" VoterFormData : holds
    VoterUI "1" -- "1" VoterSession : keeps

    VoterUI ..> VoterController : uses
    VoterUI ..> ResultsController : uses

    VoterController ..> ClientCrypto : uses
    VoterController ..> SecureSession : uses
    VoterController ..> VoteAction : returns
    VoterController ..> JoinOutcome : returns
    VoterController ..> CheckOutcome : returns
    ResultsController ..> SecureSession : uses
    ResultsController ..> ResultView : creates

    SecureSession ..> BallotRequest : uses
    SecureSession ..> BallotResponse : creates
```

An Observer views results through this same client, so `ResultsController` serves UC-5 for both voters and observers.
`VoterController` represents the `bu_*` functions in `libballotclient`; it is not a stored C object.
The executable keeps one `bcl_ctx` transport context and one `bu_session_t` voter session.
Voter identity comes from the username confirmed by the server's LOGIN or REGISTER exchange.

### Solution Class Diagram: admin client (ballotctl)

```mermaid
classDiagram
    class AdminUI {
        -formData: ElectionFormData
        +show(msg: String)
    }

    class ElectionFormData {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        -electionId: String
    }

    class AdminController {
        +prevalidateConfig(cfg: ElectionConfig) ResultStatus
        +buildCreate(cfg: ElectionConfig) BallotRequest
        +buildTransition(op: RequestOp, id: String) BallotRequest
        +foldEligible(names: List~String~) ResultStatus
    }

    class ResultsController {
        +viewResults(id: String) ResultView
        +checkHash(id: String, hash: String) CheckResult
    }

    class ElectionConfig {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        +isValid() ResultStatus
    }

    class AdminChannel {
        +setControlPath(path: String)
        +send(req: BallotRequest) BallotResponse
    }

    class BallotRequest {
        -op: RequestOp
        -certName: String
        -electionId: String
        -ballot: Ballot
        -hash: String
        -config: ElectionConfig
    }

    class BallotResponse {
        -status: ResultStatus
        -election: Election
        -receipt: Receipt
        -tally: List~int~
        -optionCount: int
        -options: List~String~
        -hashes: List~BallotHash~
        -hashCount: int
        -found: boolean
        -foundOption: int
        -foundOptionName: String
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
    }

    class CheckResult {
        -found: boolean
        -optionIndex: int
        -optionName: String
    }

    class RequestOp {
        <<enumeration>>
        JOIN
        CAST
        UPDATE
        RESULTS
        CHECK
        CREATE
        OPEN
        CLOSE
        PUBLISH
        ADMIN_RESULTS
        ADMIN_CHECK
        ADMIN_NEXT_ID
    }

    AdminUI "1" -- "1" ElectionFormData : holds

    AdminUI ..> AdminController : uses
    AdminUI ..> ResultsController : uses

    AdminController ..> ElectionConfig : creates
    AdminController ..> AdminChannel : uses
    ResultsController ..> AdminChannel : uses
    ResultsController ..> ResultView : creates

    AdminChannel ..> BallotRequest : uses
    AdminChannel ..> BallotResponse : creates
    BallotRequest ..> RequestOp : uses
```

`AdminController` holds no election of its own: it represents the `bc_*` request-building functions, not a stored C object.
`prevalidateConfig` calls the daemon library's authoritative validator, so the config rules exist in one place and the admin sees errors before a round trip.

`BallotRequest` and `BallotResponse` are shared protocol types from `libballotclient.a`.
Unlike ballotu, ballotctl sends every request through a one-shot, owner-only AF_UNIX control socket and never opens a TCP/tetriSH voter session.

### Solution Class Diagram: daemon tier

`ballotd` runs on the admin machine and is the only tier that touches the store.
ballotu reaches `BallotdService` through TCP/tetriSH after LOGIN or REGISTER.
ballotctl reaches it through ballotd's local AF_UNIX control plane.

```mermaid
classDiagram
    class BallotdService {
        +verifyCert(certName: String) CertStatus
        +checkEligibility(e: Election, certName: String) boolean
        +validateConfig(cfg: ElectionConfig) ResultStatus
        +createElection(cfg: ElectionConfig, desiredId: String) String
        +transitionState(id: String, to: ElectionState) ResultStatus
        +recordBallot(id: String, b: Ballot) Receipt
        +publishResults(id: String) ResultStatus
        +getResults(id: String, username: String) ResultView
        +getResultsAdmin(id: String) ResultView
        +lookupHash(id: String, hash: String) BallotHash
    }

    class ElectionConfig {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        +isValid() ResultStatus
    }

    class Election {
        -id: String
        -title: String
        -state: ElectionState
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        -tally: List~int~
        +isOpen() boolean
        +isEligible(certName: String) boolean
        +canTransitionTo(to: ElectionState) boolean
        +getTally() List~int~
    }

    class Ballot {
        -certName: String
        -nonce: String
        -payload: byte[]
        -payloadLen: int
    }

    class BallotHash {
        -hash: String
        -optionIndex: int
        -version: int
        -superseded: boolean
        +supersede()
    }

    class Receipt {
        -hash: String
        -issuedAt: String
        +getHash() String
    }

    class ServerCrypto {
        +decryptBallot(b: Ballot) int
        +issueReceipt(b: Ballot, version: int) Receipt
    }

    class BallotStore {
        +exec(cmd: DbCommand) DbResult
    }

    class DbCommand {
        -op: DbOperation
        -electionId: String
        -newState: ElectionState
        -hashRow: BallotHash
        -hash: String
        -nonce: String
        -certName: String
        -config: ElectionConfig
    }

    class DbResult {
        -status: ResultStatus
        -election: Election
        -tally: List~int~
        -hashes: List~BallotHash~
        -found: boolean
    }

    class DbOperation {
        <<enumeration>>
        INSERT_ELECTION
        UPDATE_STATE
        APPEND_BALLOT
        MARK_SUPERSEDED
        NONCE_MARK
        GET_ELECTION
        GET_TALLY
        GET_HASHES
        FIND_HASH
        NONCE_SEEN
        GET_PRIOR_BALLOT
        SET_OWNER
    }

    class ElectionState {
        <<enumeration>>
        DRAFT
        OPEN
        CLOSED
        PUBLISHED
    }

    class CertStatus {
        <<enumeration>>
        INVALID
        EXPIRED
        NOT_ELIGIBLE
        VALID
    }

    class ResultStatus {
        <<enumeration>>
        OK
        ERR_CONFIG_TITLE
        ERR_CONFIG_OPTIONS
        ERR_CONFIG_TIME
        ERR_CONFIG_ID_TAKEN
        ERR_ILLEGAL_TRANSITION
        ERR_NOT_OPEN
        ERR_CLOSED
        ERR_NOT_PUBLISHED
        ERR_NOT_ELIGIBLE
        ERR_CERT_INVALID
        ERR_CERT_EXPIRED
        ERR_BAD_OPTION
        ERR_REPLAY
        ERR_DECRYPT
        ERR_NOT_FOUND
        ERR_NOT_JOINED
        ERR_DB
        ERR_NOT_IMPLEMENTED
        ERR_RETRY
    }

    Election "1" *-- "*" BallotHash : records
    Election "1" -- "1" ElectionState : state

    BallotdService ..> ElectionConfig : uses
    BallotdService ..> Election : uses
    BallotdService ..> Ballot : uses
    BallotdService ..> BallotHash : creates
    BallotdService ..> Receipt : creates
    BallotdService ..> ServerCrypto : uses
    BallotdService ..> BallotStore : uses
    BallotdService ..> CertStatus : returns
    BallotdService ..> ResultStatus : returns

    ServerCrypto ..> Ballot : uses
    ServerCrypto ..> Receipt : creates
    BallotStore ..> DbCommand : uses
    BallotStore ..> DbResult : creates
    DbCommand ..> DbOperation : uses
```
