#ifndef LIBTETRISAUTH_PROVISION_H
#define LIBTETRISAUTH_PROVISION_H

/*
 * provision.h - what bin/tetrisdb needs from libtetrisauth before any session
 * exists, plus the one function a session calls to read the result.
 *
 * This is a second header because there are two consumers with different jobs,
 * not one header that anything in the tree may include. tetrisauth.h is "what
 * session.c calls" and stays exactly three functions; this is "what the
 * launcher provisions". bin/tetrisdb pays one #include and one call per item,
 * the same budget standing constraint 2 sets for session.c.
 *
 *
 * THE SCHEMA TEXT LIVES HERE, BESIDE ITS ONLY PARSER
 *
 * tetrisdb start calls tdb_ensure_table(db_dir, TETRISAUTH_DB_TABLE,
 * TETRISAUTH_DB_SCHEMA) before it spawns the JVM, because SimpleDB reads
 * catalog.txt once at runner startup and never again.
 *
 * The launcher is the caller, so the literal ought by ownership to live there.
 * It does not, for one reason: tdb_row_fields() hands out fields
 * POSITIONALLY. If the schema text lived in the launcher and the field indices
 * lived in libtetrisauth, a column reordered in one file would read a salt as
 * a digest with no error anywhere. Co-locating the definition with its only
 * parser makes that a single-file fact. The precedent is LOGD_DB_SCHEMA next
 * to its only writer at src/tetrislogd/sink.c:165.
 *
 * The login read is nevertheless an explicit projection
 * (`select id, salt, digest, iters from user where name = '...'`) rather than
 * `select *`, so the field positions come from the query text two lines above
 * the code that indexes into them. That deliberately weakens the argument
 * above to defence in depth, which is the better trade.
 *
 * HAZARDS THAT BELONG WITH THE SCHEMA, because this is where the next writer
 * of `user` will look:
 *
 *   - UNIQUENESS IS A CONVENTION, NOT A PROPERTY OF THE DATA. SimpleDB's
 *     HeapFile.insertTuple appends a brand new page when every existing page
 *     is full, and a freshly appended page carries no lock anyone conflicts
 *     with - so two concurrent registrations of the same name can both commit,
 *     with no deadlock, no <<END retry>> and no error. What actually prevents
 *     duplicates is the named POSIX semaphore "/tetrish_register", taken
 *     around the whole registration transaction in every session process.
 *     ANYTHING ELSE THAT EVER WRITES THIS TABLE MUST TAKE IT - a tetrisctl
 *     command, a manual insert - or duplicates come back silently. The
 *     three-line fix in db/ was ruled out of scope because db/ is graded lab
 *     code, not because it was the worse answer.
 *   - id is allocated max(id)+1 inside the registration transaction, so it
 *     RECYCLES if the highest-numbered row is ever deleted. Nothing deletes
 *     rows today. If account deletion lands, an old token's `sub` could name a
 *     different person.
 *   - created_at is SimpleDB's int, which is a Java int, so it is 32-bit and a
 *     Unix-seconds value WRAPS IN 2038.
 *   - iters is per row and is not speculative: there is no UPDATE in SimpleDB,
 *     so a compile-time-only iteration count means the day the number is
 *     raised every stored row fails to verify and every account is dead. The
 *     column lets old rows verify at their own count. Do not remove it as dead
 *     weight because it holds one distinct value today.
 *   - The username charset excludes TAB as well as LF and quote, and the
 *     digest encoding is hex, both so a tab-separated select reply cannot
 *     shift a row's fields.
 *
 * Full reasoning: #47 (schema), #44 (the append hole and the semaphore),
 * #52 (the launcher).
 *
 *
 * THE JWT SECRET IS CREATED HERE AND ONLY EVER READ IN A SESSION
 *
 * There is no O_CREAT|O_EXCL retry in the session process, because there is no
 * creation in the session process. The retry was specified and then DELETED
 * RATHER THAN WRITTEN, and the reason is worth carrying: O_EXCL makes creation
 * exclusive but does not make create-and-fill atomic, so a creator that dies
 * between open and write leaves a zero-length file that the >= 32 byte rule
 * then correctly refuses FOREVER. A transient race would have become a
 * self-inflicted brick, on the login path of every session process, guarding
 * an event that happens once in the lifetime of an installation.
 *
 * Provisioning has one writer, no concurrent readers, a flock already held,
 * and an operator standing at a terminal reading a non-zero exit code. The
 * mkstemp / fchmod / fsync / link dance still happens - the hazard is not
 * deleted, it is moved to the only place where it is cheap and where failing
 * is loud. This is the same move #52 made by putting tdb_ensure_table() and
 * the exec eleven lines apart in one process: correct by construction rather
 * than by convention.
 *
 * It costs nothing in availability, which is the argument that decides it:
 * nothing can reach a mint without first reaching the database, so the set of
 * configurations in which login works is identical either way.
 */

#include <stddef.h>

/* The catalog line tetrisdb start creates before spawning the JVM. */
#define TETRISAUTH_DB_TABLE  "user"
#define TETRISAUTH_DB_SCHEMA \
    "id int, name string, salt string, digest string, iters int, created_at int"

/*
 * Create <root>/auth/jwt_secret if absent, validate it if present. Returns 0
 * if a usable secret exists on return, -1 otherwise, with a human-readable
 * reason written into err.
 *
 * PROVISIONING ONLY - never called from a session. tetrisdb start calls it as
 * step 4b, inside the flock, between tdb_ensure_table() and the socket unlink,
 * and REFUSES THE WHOLE START on -1. That is where "fail hard on a loose
 * secret" lives now: in front of an operator, before any session exists, which
 * is the only place where refusing prevents anything.
 *
 * Creates exactly 32 raw bytes from RAND_bytes at mode 0600. Not hex: a
 * 64-character hex file carries 32 bytes of entropy and invites the question
 * of whether the key is the text or the decoded bytes, which is a question
 * with no good answer and no need to exist. `cat auth/jwt_secret` printing
 * garbage is a feature.
 *
 * Idempotent, and it NEVER REPAIRS: it creates at 0600 or refuses, and nothing
 * anywhere calls chmod on this file. Calling it twice does not rotate an
 * existing key.
 *
 * Because start refuses at step 1 when the lock is held, a secret deleted out
 * from under a live runner cannot be re-provisioned in place. Recovery is
 * tetrisdb stop && tetrisdb start, which is already the documented recovery
 * for adding a table.
 */
int tauth_secret_provision(const char *root, char *err, size_t errlen);

/*
 * Read and validate <root>/auth/jwt_secret into out. Returns the byte count,
 * or -1. NEVER CREATES.
 *
 * Called per request at the top of the LOGIN/REGISTER arm - before the
 * semaphore, so a broken secret never stalls every other registration in the
 * system, and before the row is written, so REGISTER cannot create an account
 * and then fail to mint (which would produce a 500 followed by a 409 on the
 * retry, corrupting the meaning 409 was given). Never on the guest path: a
 * guest never needs a secret, and reporting one missing would put noise into
 * the channel operator visibility depends on.
 *
 * NOTHING IS CACHED, INCLUDING THE FAILURE. An operator who runs
 * chmod 0600 auth/jwt_secret fixes every live session on its next attempt with
 * no restart of anything. The cost is at most auth_max_attempts reads of a
 * 64-byte file per process against 65 ms of PBKDF2 per attempt, which is not a
 * measurable cost and which buys the deletion of a cache-invalidation
 * question.
 *
 * Every check is on the OPEN DESCRIPTOR via fstat, never on the path via stat,
 * so there is no check-to-use window:
 *
 *   S_ISREG              - a FIFO here passes a permission check and then
 *                          blocks the session in read() forever, holding a
 *                          client that has already handshaked. One line, one
 *                          hang removed.
 *   st_mode & 077        - group- or other-readable is a compromised key
 *   st_uid == geteuid()  - a secret owned by someone else that happens to be
 *                          readable by us is exactly as compromised
 *   32 <= st_size <= 64  - 32 is RFC 7518 3.2's floor. The upper bound is a
 *                          deliberate stop rather than a security property:
 *                          above the HMAC block size the key would be hashed
 *                          before use, so the file's contents and the
 *                          effective key would differ, and nobody should have
 *                          to know that.
 *
 * An unusable secret DEGRADES rather than refusing to start: LOGIN and
 * REGISTER return 500 with one LOG_ERROR naming the file and the defect, and
 * GUEST, JOIN, START and gameplay are untouched. That is not the runner's
 * reasoning applied by analogy - a loose secret is a security failure, not an
 * availability one - it is that refusing to start buys nothing here. Refusing
 * to mint already stops issuance completely, the exposure predates this
 * process and outlives it, and the only thing refuse-to-start adds is that the
 * player cannot play as a guest on a path that never touches the secret.
 *
 * The bytes belong in ONE stack buffer alive for one request, explicit_bzero'd
 * beside the credentials on every exit path - the same pattern
 * src/tetrisd/session.c:344 already uses for the server private key. There is
 * deliberately no process-lifetime static with an atexit() scrub: atexit does
 * not run on _exit, a signal or a crash, which is the entire case a long-lived
 * copy would need protecting from.
 *
 * bin/tetrisdb check calls THIS function, not an approximation of it, so a
 * validation rule cannot drift between the operator's diagnostic and the
 * runtime path.
 *
 * The path is a compile-time constant resolved against root, not a .tetrishrc
 * key: two readers must agree on it, shared keys are exactly where a typo
 * becomes a silent 500, and a key nobody has a reason to change buys nothing
 * while adding a disagreement mode.
 *
 * Full reasoning: #58.
 */
int tauth_secret_load(const char *root, unsigned char *out, size_t outlen);

#endif /* LIBTETRISAUTH_PROVISION_H */
