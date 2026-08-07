/**
 * @file authbudget.c
 * @brief The attempt rule. Contract in libcommon/authbudget.h.
 *
 * The whole of #56 is the one predicate in counts(). Everything else here is
 * bookkeeping around it, and it is bookkeeping that used to live in five raw
 * struct fields on the client and two file-statics on the server.
 */

#include "libcommon/authbudget.h"

#include <string.h>

/** Does this status mean the credentials were wrong. Called by
 * auth_budget_reply(), and the only place the rule is written. */
static bool counts(int status)
{
    return status == 401 || status == 404;
}

void auth_budget_reset(auth_budget_t *b)
{
    if (b != NULL)
        memset(b, 0, sizeof *b);
}

auth_verdict_t auth_budget_reply(auth_budget_t *b, int status)
{
    /* Armed by this reply or cleared by it, never left over from an older
     * one: the question it answers is about the immediately preceding
     * response, so a stale arming would report an ordinary disconnect as cap
     * exhaustion. */
    b->armed = false;

    if (status >= 200 && status < 300)
        return AUTH_VERDICT_OK;

    if (!counts(status))
        return AUTH_VERDICT_REFUSED;

    b->failures++;
    if (status == 401)
        b->armed = true;
    return AUTH_VERDICT_COUNTED;
}

int auth_budget_failures(const auth_budget_t *b) { return b->failures; }

bool auth_budget_exhausted(const auth_budget_t *b, int cap)
{
    return b->failures >= cap;
}

bool auth_budget_hangup_is_cap(auth_budget_t *b)
{
    bool armed = b->armed;
    b->armed = false;
    return armed;
}
