#ifndef LIBTETRISAUTH_H
#define LIBTETRISAUTH_H

/**
 * @file tetrisauth.h
 * @brief The pre-auth exchange, owned end to end.
 *
 * The whole surface src/tetrisd/session.c sees. libtetrisauth owns the client
 * socket from session_accept() until the client is an account or an accepted
 * guest; only then does session_main() enter its poll loop. Below that line it
 * owns its recv/reply loop, the three methods, the credential split, the
 * database query and its deadline, PBKDF2, the token in the 200 body, every
 * status code, the attempt counter and both scrubs. session.c never formats an
 * auth response and never learns that 404 and 409 exist.
 *
 * Status table, attempt rule, `user` schema, startup ordering and rc keys are
 * in docs/libtetrisauth.md.
 *
 * Three things a reader of session.c needs:
 *
 * BEING IN THE POLL LOOP IS THE PROOF OF AUTHENTICATION. There is no pre-auth
 * flag on Session, nothing in SessionState, no session_is_authed(). The loop
 * cannot start until tauth_login() returns TAUTH_OK, so the illegal state is
 * unrepresentable rather than guarded. Moving the call is the one thing that
 * breaks it, and nothing catches that.
 *
 * IT BLOCKS, AND THAT IS THE FEATURE. The session binary serves exactly one
 * client, which is sitting on a login screen; the gravity tick is already a
 * no-op pre-play, and every client is its own process. The ~2s database
 * deadline lives inside this library as a private poll(), so session.c never
 * sees an fd or a third timeout.
 *
 * STATE IS FILE-STATIC, and that is not a smell here: tetrisd forks and execs
 * this binary per accept, so one process serves one identity for its whole
 * life and there is no second instance to collide with. Nothing resets, and
 * there is deliberately no tauth_reset_for_test() - a test that wants a fresh
 * counter forks, because that is the shape that ships. Nothing caches whether
 * the database or the JWT secret is reachable: a cached outage would be a
 * degradation that never recovers, so reachability is rediscovered by every
 * login that needs it.
 *
 * What this library does NOT do: it never verifies a token (the client
 * discards the minted JWT, #46 decision 5), and it gates no capability - a
 * guest and an account can do the same things, and logging in only changes the
 * roster name. A GUEST TOUCHES NO DATABASE, NO SOCKET UNDER db_ipc AND NO FILE
 * UNDER db_dir; that is #52's Invariant A, guarded by the one test in the
 * suite that requires the runner's absence.
 *
 * Issue #43; seam decided in #55.
 */

#include <stdbool.h>
#include <stddef.h>

#include "libhtttp/htttp.h"
#include "libtetrissh/tetrissh.h"
#include "libcommon/sessionstate.h"

/** tauth_login()'s two outcomes. An anonymous enum rather than a typedef: this
 * header exports three functions and no types, so nothing of ours can end up
 * as a member of one of session.c's structs. */
enum {
    TAUTH_OK   = 0, /**< The client is an account or an accepted guest. */
    TAUTH_DROP = 1  /**< Hung up, or burned auth_max_attempts. */
};

/**
 * Runs the pre-auth exchange to completion.
 *
 * Called once by session.c, after session_accept() succeeds and before the
 * poll loop, which must not start until this returns TAUTH_OK.
 *
 * Blocks. It owns its recv/reply loop on sh and answers on the wire itself.
 * "Still pre-auth" never escapes - that is a loop iteration in here, not a
 * return value. Reads <root>/.tetrishrc on its first call rather than in the
 * caller's startup, because the session binary has no startup this library is
 * entitled to add code to. Credentials land in this library's own stack buffer
 * and are explicit_bzero'd in one function with one exit, so session.c has no
 * relationship to the plaintext.
 *
 * @param sh  The established session. Untouched apart from the frames that
 *            cross it; nothing here holds a resource outliving the call.
 * @returns TAUTH_OK, or TAUTH_DROP for the caller to close as it already does
 *          for a client that hung up.
 */
int tauth_login(session_t *sh);

/**
 * One dispatch arm: a LOGIN, REGISTER or GUEST arriving after tauth_login().
 *
 * Called as a guard at the top of session_dispatch()'s existing switch.
 * Deleting the call is not the same change: without it a post-login LOGIN
 * falls through to the bottom, where an unknown method is ignored, and the
 * client waits for a response that never comes.
 *
 * IT ALWAYS REFUSES, WITH 409, IN EVERY STATE. Identity is fixed by the
 * pre-auth exchange and never changes for the life of the connection, so a
 * player dropped to guest by a 500 is a guest until they reconnect. #48
 * decision 3 kept guest non-terminal and this overturns it; the cost is real,
 * since tetrisu's reconnect (#61) is the only way back. What it buys is a
 * function with no exchange, no path that can consume an attempt, and
 * therefore no way to drop the connection - which is why the old final-401 and
 * shutdown(SHUT_RDWR) mechanism is deleted rather than documented.
 *
 * Scrubs req->body in place, casting away the const: refusing the request does
 * not unread it, and the plaintext genuinely landed in the caller's buffer, so
 * the scrub cannot be moved, only made invisible.
 *
 * The session it replies on is the one tauth_login() was handed, retained in
 * the same file-static block as everything else here. Do not widen this call
 * site to pass one.
 *
 * @param req  The parsed request; its body is scrubbed before returning.
 * @param st   Not read. Kept because removing it churns session.c's call site
 *             to delete one argument, and it costs nothing to start reading
 *             again if some future state needs an answer other than 409.
 * @returns true if it consumed the method, false if the request is none of its
 *          business.
 */
bool tauth_offer(const htttp_request_t *req, const SessionState *st);

/**
 * This client's roster display name, or "" for a guest.
 *
 * Called from session.c's JOIN arm to stamp ADMIN_JOIN. The username is known
 * only in the session process and room.c runs in tetrisd, so AdminMsg is the
 * only channel between them. The stamp is memoryless: room.c falls back to
 * "Player %d" on an empty name, so a forgotten call degrades to today's
 * behaviour rather than shipping an uninitialised roster.
 *
 * Never fails, never blocks, never touches a socket.
 *
 * @param dst  Receives the name.
 * @param cap  Must be MAX_PLAYER_NAME. That is where the 1..15 character cap
 *             comes from, and it is enforced at REGISTER with a 400 rather
 *             than by truncating here: two players could truncate to the same
 *             string, and a roster that silently renames people is worse than
 *             a registration that refuses a long name up front.
 */
void tauth_name(char *dst, size_t cap);

#endif /* LIBTETRISAUTH_H */
