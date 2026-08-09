#ifndef LIBTETRISUTIL_AUTHBUDGET_H
#define LIBTETRISUTIL_AUTHBUDGET_H

/**
 * @file authbudget.h
 * @brief How many login attempts are left, for both sides of the wire.
 *
 * Called by libtetrisauth's login path, which enforces the cap, and by
 * src/tetrisu/net.c, which mirrors it so the client can tell the user. The
 * rule is #56's and the client's half is #57.
 *
 * A response counts if and only if it means the credentials were wrong.
 * Nothing resets the counter for the life of a connection: "reset on any
 * success" reads to a script as four wrong logins, one GUEST, four more,
 * forever. The cap is not a brute-force defence - reconnecting starts a fresh
 * counter and PBKDF2 is the real control - it stops a stuck client hammering
 * one socket.
 *
 * This replaced authstatus.h, which exported the predicate as a macro so both
 * sides could share one copy of `401 || 404`. They then went on to keep their
 * own counters and their own derived flags either side of it, which is the
 * part that actually drifted: the client held five raw fields and its tests
 * asserted on all five.
 */

#include <stdbool.h>

/**
 * What one auth response means.
 *
 * The order matters to nothing; branch on the value, never on its number.
 */
typedef enum {
    AUTH_VERDICT_OK,      /**< 2xx: the exchange resolved, identity is fixed. */
    AUTH_VERDICT_REFUSED, /**< Refused, and it did not count. */
    AUTH_VERDICT_COUNTED  /**< Refused because the credentials were wrong. */
} auth_verdict_t;

/**
 * One connection's attempt budget. Zero-initialise it, or call
 * auth_budget_reset(); every field is derived and none is a caller's to read.
 */
typedef struct {
    int  failures;   /**< Counted refusals so far on this connection. */
    bool armed;      /**< The last verdict was a counted 401, so a hangup now
                          would be cap exhaustion. Consumed by
                          auth_budget_hangup_is_cap(). */
} auth_budget_t;

/** Clears the budget. Called when a connection opens; a new connection is a
 * new budget, which is the whole reason the cap is not a security control. */
void auth_budget_reset(auth_budget_t *b);

/**
 * Records one auth response and says what it meant.
 *
 * @param b       Budget to move.
 * @param status  The response status. 400, 409 and 500 do not count: a 400 is
 *                a bug in our own client, which knows the allowlist too, and
 *                REGISTER can return nothing else, so it is uncapped by
 *                construction rather than by an exemption.
 * @returns The verdict; AUTH_VERDICT_COUNTED also means failures went up.
 */
auth_verdict_t auth_budget_reply(auth_budget_t *b, int status);

/** Counted refusals so far. The client reports this; it holds no copy of the
 * cap, so there is no number an operator's rc edit can make false. */
int auth_budget_failures(const auth_budget_t *b);

/** Has the budget reached cap. Called by the server, which is the only side
 * that knows auth_max_attempts. */
bool auth_budget_exhausted(const auth_budget_t *b, int cap);

/**
 * Is a hangup arriving now this connection's cap being spent?
 *
 * Only a 401 - never a 404 - is followed by the server's shutdown(): a wrong
 * password burns the connection, a nonexistent user does not. The bit that
 * would otherwise prove it is already cleared by the time the hangup arrives,
 * so this is armed by the reply and consumed by the next question.
 *
 * @param b  Budget; the arming is one-shot and is cleared here.
 * @returns true if the immediately preceding verdict was a counted 401.
 */
bool auth_budget_hangup_is_cap(auth_budget_t *b);

#endif /* LIBTETRISUTIL_AUTHBUDGET_H */
