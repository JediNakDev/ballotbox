/**
 * @file authconf.c
 * @brief The five .tetrishrc values bin/session reads on the auth path.
 *
 * Phase 4e (#55), key set and behaviour from #60. Contract in tauth_priv.h,
 * tables in docs/libtetrisauth.md.
 *
 * The keys are declared as an rc_key_t table rather than parsed by hand:
 * libtetrisutil/rc.h owns the whole-string strtol, the range check, the fixed-size
 * copy and the "first bad value wins" report, which this file, tetrislogd and
 * libtetrisdb's runner each used to carry their own copy of.
 *
 * The session reader validates only the values it consumes. It does not reject
 * unknown keys because it cannot know the whole db_ namespace, and a warning
 * from a process already serving a client is noise nobody sees.
 * tauth_rc_validate() is the separate operator-facing adapter used by
 * bin/tetrisdb start: it owns the complete auth_ namespace and rejects an
 * unknown key while a human is watching (#60 decision 6). That is the only
 * difference between the two, and it is now one argument.
 *
 * The one thing worth reading twice is the freeze rule, because it looks like
 * caching and is its opposite. A SUCCESS is frozen, so a running session never
 * sees its attempt cap change underneath it. A FAILURE is not remembered at
 * all, so an operator who creates the file repairs every live session on its
 * next LOGIN with no restart of anything - the same rule #58 gives the JWT
 * secret and #52 gives runner reachability, and for the same reason: a
 * remembered failure is a degradation that never recovers.
 */

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "libtetrisutil/logmsg.h"
#include "libtetrisutil/rc.h"
#include "libtetrisauth/config.h"
#include "libtetrisdb/socket/conf.h"

#include "tauth_priv.h"

/* The two db_ values are libtetrisdb/socket/conf.h's. bin/tetrisdb owns that
 * namespace and validates it; this reader is the consumer, and taking the
 * default and the bounds from the owner is what stops the launcher and the
 * login path disagreeing about them when an operator omits a key. */

static auth_conf_t g_conf = {
    .max_attempts = TAUTH_DEFAULT_MAX_ATTEMPTS,
    .token_ttl = TAUTH_DEFAULT_TOKEN_TTL,
    .pbkdf2_iters = TAUTH_DEFAULT_PBKDF2_ITERS,
    .db_timeout_ms = TDB_DEFAULT_TIMEOUT_MS,
    .db_sock = TDB_DEFAULT_IPC,
};

/* Set by the first load that produced usable values, and never cleared. */
static int g_frozen;

const char *const tauth_rc_keys[] = {
    "auth_max_attempts", "auth_token_ttl", "auth_pbkdf2_iters", NULL,
};

/*
 * libtetrisauth's own three keys, and the ranges #60 settled.
 *
 * auth_pbkdf2_iters has no floor beyond 1, deliberately: any floor low enough
 * for the test suite's four-process sweep to run in under a second stops
 * nobody in production (#60 decision 8).
 */
static const rc_key_t AUTH_KEYS[] = {
    {.key = "auth_max_attempts",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, max_attempts),
     .lo = 1,
     .hi = 100},
    {.key = "auth_token_ttl",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, token_ttl),
     .lo = 60,
     .hi = 31536000},
    {.key = "auth_pbkdf2_iters",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, pbkdf2_iters),
     .lo = 1,
     .hi = 10000000},
};

/* The three above plus the two launcher keys the login path is the other end
 * of. bin/tetrisdb owns the db_ namespace and validates it; this reader only
 * consumes what it needs, which is why it passes no owned_prefix. */
static const rc_key_t SESSION_KEYS[] = {
    {.key = "auth_max_attempts",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, max_attempts),
     .lo = 1,
     .hi = 100},
    {.key = "auth_token_ttl",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, token_ttl),
     .lo = 60,
     .hi = 31536000},
    {.key = "auth_pbkdf2_iters",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, pbkdf2_iters),
     .lo = 1,
     .hi = 10000000},
    {.key = "db_timeout",
     .type = RC_INT,
     .off = offsetof(auth_conf_t, db_timeout_ms),
     .lo = TDB_TIMEOUT_MIN_MS,
     .hi = TDB_TIMEOUT_MAX_MS},
    {.key = "db_ipc",
     .type = RC_STR,
     .off = offsetof(auth_conf_t, db_sock),
     .cap = sizeof(((auth_conf_t *)0)->db_sock),
     .max_len = TDB_IPC_MAX},
};

int tauth_rc_validate(const char *rc_path) {
  if (rc_path == NULL)
    return -1;

  auth_conf_t scratch = g_conf;
  rc_defect_t defect;

  int applied = rc_bind(rc_path, AUTH_KEYS,
                        sizeof AUTH_KEYS / sizeof AUTH_KEYS[0], &scratch,
                        "auth_", &defect);
  if (applied == RC_E_OPEN) {
    fprintf(stderr, "tetrisdb: cannot read %s\n", rc_path);
    return -1;
  }
  if (applied < 0) {
    fprintf(stderr, "tetrisdb: %s: invalid directive (%s = %s)\n", rc_path,
            defect.key, defect.value);
    return -1;
  }
  return applied;
}

const auth_conf_t *auth_conf(void) { return &g_conf; }

int auth_conf_load(void) {
  if (g_frozen)
    return 0;

  auth_conf_t scratch = g_conf;
  rc_defect_t defect;

  int applied =
      rc_bind(RC_PATH, SESSION_KEYS,
              sizeof SESSION_KEYS / sizeof SESSION_KEYS[0], &scratch, NULL,
              &defect);
  if (applied == RC_E_OPEN) {
    log_send(LOG_ERROR,
             "auth: %s could not be read; LOGIN and REGISTER will answer 500 "
             "and the attempt counter runs on %d. GUEST is unaffected",
             RC_PATH, g_conf.max_attempts);
    return -1;
  }
  if (applied < 0) {
    log_send(LOG_ERROR,
             "auth: %s: unusable value (%s = %s); LOGIN and REGISTER answer "
             "500 until it is corrected. GUEST is unaffected",
             RC_PATH, defect.key, defect.value);
    return -1;
  }

  g_conf = scratch;
  g_frozen = 1;

  /* The answer to "was the file actually found, and what is this box running
   * on" - one line, greppable, naming every resolved value rather than only
   * the ones that differ from a default (#60 decision 9). */
  log_send(LOG_INFO,
           "auth: %s (%d directives): max_attempts=%d token_ttl=%d "
           "pbkdf2_iters=%d db_ipc=%s db_timeout=%d",
           RC_PATH, applied, g_conf.max_attempts, g_conf.token_ttl,
           g_conf.pbkdf2_iters, g_conf.db_sock, g_conf.db_timeout_ms);
  return 0;
}
