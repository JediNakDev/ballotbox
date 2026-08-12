# BallotBox

1009098 Pitchayut Ariyachansil (Jedi)
1009164 Phatsakorn Ukanchanakitti (Pop)
1009195 Popsuk Sumetchoengprachya (Kenji)

---

BallotBox is a secure e-voting system for small orgs (clubs, coops, unions) solving the core tension: ballots must be secret (untraceable to voters) yet verifiable (tally is auditable).
Built on the 50.005 CoreStack/tetriSH libraries, it delivers a CLI-based voter client and admin control tool (ballotctl) over a custom C backend (ballotd), communicating via SSH/shell sessions.

---

| Shared library with tetriSH |                           |
| --------------------------- | ------------------------- |
| tetrish                     | a shell                   |
| tetrislogd                  | a logger daemon           |
| libhtttp                    | a http protocol           |
| libtetrissh                 | a secure shell            |
| libtetrisui                 | a ui library              |
| libtetrisauth               | an authentication library |
| libtetrisdb and tetrisdb    | a connector to SimpleDB   |
| libtetrisutil               | a utility library         |

---

**Glossary**

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

## Requirement

See [Requirement](REQUIREMENTS.md)

## Design

See [Design](DESIGN.md)

## Implementation Challenges

See [Implementation Challenges](CHALLENGES.md)

## Testing

See [Testing](TEST.md)

## Feature Progress Records

See [Features](FEATURE.md)

## Final Note

The team confirms that this is an academic project with no industry partner, partner code, proprietary data sample, or partner intellectual property.

BallotBox contributes most directly to UN SDG 16, particularly Target 16.6 on accountable institutions and Target 16.7 on inclusive, participatory decision-making.
Receipt-based verification lets each voter confirm that the published result includes their latest ballot, while authenticated election management, replay protection, and durable records strengthen accountability.
Secret ballots and logs without voter-choice links protect participation from disclosure, while eligibility rules let small organisations define their electorate consistently.

The project also contributes to UN SDG 12 on responsible consumption by reducing paper ballots, printing, physical storage, and travel for small elections.
Its terminal-based C services can run on existing modest hardware instead of requiring specialised voting machines.
These benefits depend on proportionate deployment: organisers should retain election data only as long as required, reuse existing equipment, and account for the electricity and network resources consumed by digital voting.
## Issue #5 strict acceptance run

The final issue #5 run used macOS in the Asia/Singapore environment with Apple Clang, OpenSSL 3 from Homebrew, and the installed Java runtime.
The observed command was `/usr/bin/time -p make final-test`.
The target cleaned all build outputs, ran the complete existing suite with `TETRISH_REQUIRE_RUNNER=1`, and ran all strict system cases.
It completed in 59.57 seconds with all 44 independently isolated system cases passing and zero skips.

The initial strict foundation passed only eight shared-fixture groups.
It also hid failing runner-backed regressions because `final-test` called `test-ci` instead of the complete suite.
Direct runs exposed seven `test_tetrisdb` launcher failures and five authenticated integration failures.
The fixes restored launcher preflight, semaphore recovery, stale-socket cleanup, diagnostic checks, safe lock handling, failed-child cleanup, and per-session configuration reload.
The final direct results were 18 of 18 `test_tetrisdb` cases and 26 of 26 authenticated cases passing.

Every system case now owns its database, account store, socket, TCP port, process tree, secret, log, and filesystem root.
Cast and update race cases pause after encryption and before transport, close the election, release the request, and verify unchanged persisted ballots.
Lifecycle cases restart the daemon and recreate the client after legal transitions.
The logging case finds the successful receipt and finds no voter-to-choice link.
