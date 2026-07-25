# BallotBox

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
        UC5((View Result))
        UC6((Check Your Vote))
    end

    Admin --- UC1
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
| Preconditions  | Authenticated admin session (valid admin cert); ballotd reachable. |
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

    note over Admin, SimpleDB: Precondition: authenticated admin session, ballotd reachable

    alt invalid config
        Admin->>+ballotctl: fill in configuration
        ballotctl->>+ballotd: submit configuration
        ballotd->>ballotd: validate configuration
        ballotd-->>-ballotctl: specific error, stays in 'draft'
        ballotctl-->>-Admin: show error (fix and retry)
    else valid config
        Admin->>+ballotctl: fill in configuration
        ballotctl->>+ballotd: submit configuration
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
| Preconditions  | Authenticated voter session (valid client cert).        |
| Postconditions | Voter is admitted to the session and can cast a ballot. |

**Flow**

1. Voter enters the election details (admin IP, port, and election ID) in ballotu.
2. ballotu connects to ballotd; ballotd fetches the election from SimpleDB.
3. ballotd checks the voter's cert against the eligible-voter list.
4. ballotd confirms the election is 'open' and admits the voter to the session.
5. ballotu displays the ballot options and confirms the voter has joined.

**Alternative Flows**

4a. Election not 'open' → cannot join yet; ballotu saves the election for later (UC-5, UC-6).

**Error States**

2a. ballotd unreachable (timeout / no route to host) → join failed.
2b. Election not found → join failed.
3a. Cert not on the eligible list → refused.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: authenticated voter session (valid client cert)

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
    else cert not on eligible list
        Voter->>+ballotu: enter election details
        ballotu->>+ballotd: connect to server
        ballotd->>+SimpleDB: fetch election
        SimpleDB-->>-ballotd: return election
        ballotd-->>-ballotu: refused (not eligible)
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
| Description    | Anyone views the published tally together with the list of ballot hashes.      |
| Actors         | Observer                                                                       |
| Triggers       | Observer runs the results command.                                             |
| Preconditions  | Authenticated observer session (valid client cert).                            |
| Postconditions | The final tally and the full list of ballot verification hashes are displayed. |

**Flow**

1. Observer selects an election to view in ballotu.
2. ballotd fetches the election from SimpleDB and checks the observer's eligibility to observe it.
3. ballotd confirms the election is 'published' and returns the tally and the counted ballot hashes from SimpleDB.
4. ballotu displays the tally and the hash list (grouped by option) so a voter can locate their own (UC-6).

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

    note over Observer,ballotd: Precondition: authenticated observer session (valid client cert)

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
| Preconditions  | Voter holds a secret ballot key from UC-3/UC-4; results published.                      |
| Postconditions | Voter confirms inclusion of their ballot without revealing their choice to anyone else. |

**Flow**

1. Voter enters their secret ballot key in ballotu.
2. ballotu derives the receipt hash from the key (hash function / KDF).
3. ballotu sends the derived hash to ballotd to look up in the published result view.
4. ballotd searches the published, non-superseded hashes in SimpleDB and confirms the hash is counted in the tally.
5. ballotu reports the voter's ballot was included and shows their recorded choice.

**Alternative Flows**

None.

**Error States**

4a. Derived hash not found in the published results → verification failed; ballotu flags it as a dropped ballot for the voter to raise with the Admin.

```mermaid
sequenceDiagram
    actor Voter
    participant ballotu
    participant ballotd
    participant SimpleDB

    note over Voter, SimpleDB: Precondition: holds a secret ballot key (UC-3/UC-4), results published

    alt hash not found
        Voter->>+ballotu: enter secret ballot key
        ballotu->>ballotu: derive receipt hash from key (hash function / KDF)
        ballotu->>+ballotd: look up derived hash in published result view
        ballotd->>+SimpleDB: search published, non-superseded hashes
        SimpleDB-->>-ballotd: hash not found
        ballotd-->>-ballotu: not found
        ballotu-->>-Voter: verification failed (dropped ballot), raise with Admin
    else hash found
        Voter->>+ballotu: enter secret ballot key
        ballotu->>ballotu: derive receipt hash from key (hash function / KDF)
        ballotu->>+ballotd: look up derived hash in published result view
        ballotd->>+SimpleDB: search published, non-superseded hashes
        SimpleDB-->>-ballotd: hash found
        ballotd-->>-ballotu: found, counted in tally (with choice)
        ballotu-->>-Voter: ballot included and counted
        note over Voter,SimpleDB: Postcondition: inclusion confirmed without revealing the choice to anyone else
    end
```

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
        -optionIndex
        -nonce
        -encryptedPayload
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
    Ballot "1" -- "1" BallotHash : recorded as
    BallotHash "1" -- "1" Receipt : issues
    Election "1" -- "1" ElectionState : state
    Certificate "1" -- "1" CertStatus : status
```

`BallotHash` is the only entity published in results, and it holds no voter identity: that is what makes the tally verifiable without making it traceable.

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
    }

    class VoterController {
        +join(fd: VoterFormData) JoinOutcome
        +routeVote(s: VoterSession) VoteAction
        +castVote(s: VoterSession, fd: VoterFormData) Receipt
        +updateVote(s: VoterSession, fd: VoterFormData) Receipt
        +checkVote(fd: VoterFormData) CheckResult
    }

    class ResultsController {
        +viewResults(id: String, certName: String) ResultView
    }

    class ClientCrypto {
        +encryptBallot(optionIndex: int, nonce: String) Ballot
        +deriveReceiptHash(secretKey: String) String
    }

    class SecureSession {
        +connect(host: String, port: int, cert: Certificate) ResultStatus
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
        -hashes: List~BallotHash~
        -found: boolean
        -foundOption: int
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
    }

    class CheckResult {
        -found: boolean
        -optionIndex: int
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
    VoterController ..> CheckResult : creates
    ResultsController ..> SecureSession : uses
    ResultsController ..> ResultView : creates

    SecureSession ..> BallotRequest : uses
    SecureSession ..> BallotResponse : creates
```

An Observer views results through this same client, so `ResultsController` serves UC-5 for both voters and observers.
`VoterController` and `ResultsController` are stateless: the session and the form data are passed in as arguments, never stored on the controller.

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
        +validateConfig(cfg: ElectionConfig) ResultStatus
        +createElection(cfg: ElectionConfig) String
        +openElection(id: String) ResultStatus
        +closeElection(id: String) ResultStatus
        +publishResults(id: String) ResultStatus
    }

    class ResultsController {
        +viewResults(id: String, certName: String) ResultView
    }

    class ElectionConfig {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        +isValid() ResultStatus
    }

    class SecureSession {
        +connect(host: String, port: int, cert: Certificate) ResultStatus
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
        -hashes: List~BallotHash~
        -found: boolean
        -foundOption: int
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
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
    }

    AdminUI "1" -- "1" ElectionFormData : holds

    AdminUI ..> AdminController : uses
    AdminUI ..> ResultsController : uses

    AdminController ..> ElectionConfig : creates
    AdminController ..> SecureSession : uses
    ResultsController ..> SecureSession : uses
    ResultsController ..> ResultView : creates

    SecureSession ..> BallotRequest : uses
    SecureSession ..> BallotResponse : creates
    BallotRequest ..> RequestOp : uses
```

`AdminController` holds no election of its own: the form data arrives as an `ElectionConfig` argument, is validated, and is passed straight to the request.
`validateConfig` calls the daemon's own validator, so the config rules exist in exactly one place and the admin still sees the error before a round trip.

`SecureSession`, `BallotRequest`, `BallotResponse`, `ResultsController`, and `ResultView` are one shared implementation (`libballotclient.a`) used by both clients; they appear in both diagrams so each stands on its own.

### Solution Class Diagram: daemon tier

`ballotd` runs on the admin machine and is the only tier that touches the store.
`SecureSession` in either client calls `BallotdService` across the network.

```mermaid
classDiagram
    class BallotdService {
        +verifyCert(certName: String) CertStatus
        +checkEligibility(e: Election, certName: String) boolean
        +validateConfig(cfg: ElectionConfig) ResultStatus
        +createElection(cfg: ElectionConfig) String
        +transitionState(id: String, from: ElectionState, to: ElectionState) ResultStatus
        +recordBallot(id: String, b: Ballot) Receipt
        +publishResults(id: String) ResultStatus
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
        ERR_DB
        ERR_NOT_IMPLEMENTED
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
