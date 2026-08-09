/**
 * @file tauth.c
 * @brief The entry point session.c calls.
 *
 * Phase 4e (#55). The contract, the invariants and the rejected alternatives
 * are in include/libtetrisauth/tetrisauth.h. Read it before changing a line
 * here; it is the deliverable this file implements, not a summary of it.
 *
 * What is in this file is the seam and nothing else: the file-static block,
 * the recv/reply loop, the method arms, both scrubs and the attempt counter.
 * The .tetrishrc read is authconf.c's, the username rules and PBKDF2 are
 * credential.c's, and the two database exchanges are account.c's. See
 * tauth_priv.h for why the line falls there.
 *
 * THE ONE THING THAT CAN BREAK THE DESIGN IS MOVING THE CALL. Being inside
 * session_main()'s poll loop is the proof of authentication, so there is no
 * flag anywhere to get out of step - and equally, nothing catches a
 * tauth_login() that gets moved below the loop or deleted. That is stated
 * here, in the header, and at the call site, because it cannot be asserted.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>

#include "libtetrisutil/limits.h"
#include "libtetrisutil/logmsg.h"
#include "libtetrisauth/jwt.h"
#include "libtetrisauth/provision.h"
#include "libtetrisauth/tetrisauth.h"

#include "libtetrisutil/authbudget.h"
#include "tauth_priv.h"

/* provision.h's upper bound on a usable secret. A local constant rather than
 * an exported one: this is the size of a buffer, and the rule it comes from is
 * stated where the rule lives. */
#define SECRET_MAX 64

/* A status line, a Date, a Content-Length and at most a JWT_MAX_LEN body. The
 * serializer refuses rather than truncates, and a refusal is logged, so an
 * undersized buffer here could never become a half-written frame. */
#define REPLY_MAX 1024

/*
 * === The file-static block ===
 *
 * THE SESSION BINARY IS ONE PROCESS SERVING EXACTLY ONE CLIENT FOR ITS ENTIRE
 * LIFE: tetrisd forks and execs it per accept, and session.c's own header
 * comment says so. One process, one identity. So there is no second instance
 * for a global to collide with, and the alternative that avoids file-statics -
 * an opaque auth_t the session author declares and threads through - costs a
 * member on his struct, a line in session_reset() and an extra parameter, all
 * to model a multiplicity that cannot exist.
 *
 * NOTHING HERE EVER RESETS. The counter is cumulative for the life of the
 * connection, and a new connection is a new process. There is deliberately no
 * tauth_reset_for_test(): a test that wants a fresh counter forks, because
 * that is the shape that ships.
 *
 * AND NOTHING HERE DESCRIBES WHETHER A DEPENDENCY IS REACHABLE. The
 * one-process-one-client argument makes such a field look harmless, which is
 * exactly why it needs saying: a cached "the database is down" would still be
 * wrong within one connection, and it would be a degradation that never
 * recovers. Reachability is discovered by the login that needs it, every time.
 */

static session_t *g_sh; /**< The session tauth_login() was handed; retained
                          because tauth_offer() takes no session_t and still
                          has to answer 409 on the wire. */

static char g_name[MAX_PLAYER_NAME]; /**< Display name AND the guest flag: a
                                       username is 1..15 characters, so empty
                                       means guest and the two cannot
                                       disagree. Read by tauth_name(). */

/* Failed credential attempts on this connection: 401 and 404 only, never
 * reset (#56). Only tauth_login() can move it, because identity is fixed by
 * the exchange it runs and no credential can be offered afterwards. */
static auth_budget_t g_budget;

/** Answer on the wire. body is NUL-terminated or NULL.
 * The serialize buffer is scrubbed because the 200 carries a JWT in its body,
 * and a token left on the stack of a process that goes on to run a game is the
 * same class of residue #59 removed from session_recv(). It costs a memset of
 * one kilobyte per response. */
static void reply(int status, const char *body) {
  htttp_response_t res;
  uint8_t out[REPLY_MAX];
  uint32_t out_len = sizeof out;

  memset(&res, 0, sizeof res);
  res.status = status;
  if (body != NULL) {
    res.body = (const uint8_t *)body;
    res.body_len = (uint32_t)strlen(body);
  }

  if (htttp_serialize_response(&res, out, &out_len) == HTTTP_OK)
    session_send(g_sh, out, out_len);
  else
    /* Only reachable if a status reaches here that htttp_reason() has no
     * phrase for, which would be a bug in this file rather than in the
     * request: the serializer refuses those silently, so it is logged here or
     * it is invisible. */
    log_send(LOG_ERROR, "auth: could not serialize a %d response", status);

  OPENSSL_cleanse(out, sizeof out);
}

/**
 * The one place the counter moves. Returns its argument so it can wrap a call.
 *
 * Called only on responses to LOGIN and REGISTER, which is what keeps the rule
 * honest: a 401 refusing a JOIN that arrived before the client authenticated
 * is a desync, not a wrong password, and spending an attempt on it would also
 * put this count out of step with the client's, which runs the same
 * libtetrisutil/authbudget.h and reports the number to the user.
 */
static int counted(int status) {
  (void)auth_budget_reply(&g_budget, status);
  return status;
}

/** Has this connection spent its auth_max_attempts. */
static bool attempts_exhausted(void) {
  return auth_budget_exhausted(&g_budget, auth_conf()->max_attempts);
}

/** Runs a LOGIN or REGISTER to completion and returns the status sent. Called
 * by the two method arms of tauth_login()'s loop.
 *
 * ONE FUNCTION, ONE EXIT, because the secret and the token are scrubbed on
 * every path including the failures - which is what makes the scrub correct
 * and also untestable, so it is closed by review of this shape.
 *
 * The order of the first four steps is #58's and #60's: config and secret are
 * read BEFORE the semaphore, so a broken one never stalls every other
 * registration, and BEFORE any row is written, so REGISTER cannot create an
 * account and then fail to mint - which would answer 500 and then 409 on the
 * retry, corrupting the meaning 409 was given. */
static int credential_flow(const htttp_request_t *req, bool is_register) {
  unsigned char secret[SECRET_MAX];
  char token[JWT_MAX_LEN];
  char name[MAX_PLAYER_NAME];
  cred_t c;
  jwt_claims_t claims;
  long long id = 0;
  int secret_len;
  int status;

  memset(token, 0, sizeof token);
  memset(secret, 0, sizeof secret);

  if (auth_conf_load() != 0) {
    status = 500; /* authconf.c already named the file and the defect */
    goto done;
  }
  if (cred_split(req->body, req->body_len, &c) != 0 ||
      cred_name(&c, name, sizeof name) != 0) {
    status = 400;
    goto done;
  }
  /* The password bounds are REGISTER's only. Enforcing a minimum at LOGIN
   * would make every existing account permanently unloggable the day the
   * minimum is raised (#47 decision 8). */
  if (is_register && cred_password_ok_for_register(&c) != 0) {
    status = 400;
    goto done;
  }

  secret_len = tauth_secret_load(".", secret, sizeof secret);
  if (secret_len < 0) {
    status = 500; /* secret.c already named the file and the defect */
    goto done;
  }

  switch (is_register ? account_register(name, &c, &id)
                      : account_login(name, &c, &id)) {
  case ACCT_OK:
    break;
  case ACCT_NO_USER:
    status = 404;
    goto done;
  case ACCT_BAD_PASSWORD:
    status = 401;
    goto done;
  case ACCT_TAKEN:
    status = 409;
    goto done;
  case ACCT_UNAVAILABLE:
  default:
    status = 500;
    goto done;
  }

  memset(&claims, 0, sizeof claims);
  claims.sub = id;
  snprintf(claims.name, sizeof claims.name, "%s", name);
  claims.iat = (long long)time(NULL);
  claims.exp = claims.iat + auth_conf()->token_ttl;

  if (jwt_mint(token, sizeof token, &claims, secret, (size_t)secret_len) != 0) {
    log_send(LOG_ERROR, "auth: could not mint a token for '%s'", name);
    status = 500;
    goto done;
  }

  /* The account is this client's identity from here to process exit. Written
   * only on the path where the row is known to exist and the token is known to
   * have been produced. */
  snprintf(g_name, sizeof g_name, "%s", name);
  status = 200;

done:
  OPENSSL_cleanse(secret, sizeof secret);
  /* The token goes out in the 200's body and the client discards it (#46
   * decision 5). Nothing in tetriSH ever verifies one. */
  reply(status, status == 200 ? token : NULL);
  OPENSSL_cleanse(token, sizeof token);
  return status;
}

/* GUEST. Touches no database, no socket under db_ipc and no file under db_dir,
 * which is Invariant A (#52 decision 6) and is what makes "the database is
 * down but the game still works" true rather than hoped for. It does not even
 * read .tetrishrc. A future change that consults anything here breaks it, and
 * the test that guards it runs with the runner provably absent. */
static void accept_guest(void) {
  g_name[0] = '\0';
  reply(200, NULL);
}

/*
 * Which of the three this request is, or AUTH_NONE.
 *
 * One classifier rather than a strcmp cascade in each of the two callers, so
 * that a fourth method - or a renamed one - is one edit. It is also the only
 * place that decides what "an auth method" means, which is what
 * tauth_offer()'s "returns false if the request is none of its business" and
 * tauth_login()'s pre-auth gate have to agree on.
 *
 * The path is not inspected, matching session_dispatch(), which routes on the
 * method alone. #48 decision 5 names /auth/login and friends, and refusing a
 * LOGIN that arrived on a different path would be a fourth failure mode for
 * the client to render while buying nothing: the method already says what the
 * request is, and the credentials are in the body either way.
 */
typedef enum {
  AUTH_NONE = 0,
  AUTH_GUEST,
  AUTH_LOGIN,
  AUTH_REGISTER
} auth_method_t;

static auth_method_t auth_method_of(const htttp_request_t *req) {
  if (strcmp(req->method, "GUEST") == 0)
    return AUTH_GUEST;
  if (strcmp(req->method, "LOGIN") == 0)
    return AUTH_LOGIN;
  if (strcmp(req->method, "REGISTER") == 0)
    return AUTH_REGISTER;
  return AUTH_NONE;
}

/* One frame during the pre-auth exchange. True once the client is an account
 * or an accepted guest. */
static bool pre_auth_frame(const htttp_request_t *req) {
  switch (auth_method_of(req)) {
  case AUTH_GUEST:
    accept_guest();
    return true;
  case AUTH_LOGIN:
    return counted(credential_flow(req, false)) == 200;
  case AUTH_REGISTER:
    return counted(credential_flow(req, true)) == 200;
  case AUTH_NONE:
  default:
    reply(401, NULL);
    return false;
  }
}

int tauth_login(session_t *sh) {
  uint8_t buf[SESSION_MAX_FRAME];
  int verdict;

  g_sh = sh;

  for (;;) {
    uint32_t len = sizeof buf;
    int rc = session_recv(sh, buf, &len);

    if (rc == SESSION_ERR_IO || rc == SESSION_ERR_TOOBIG) {
      verdict = TAUTH_DROP;
      break;
    }
    if (rc != SESSION_OK) {
      OPENSSL_cleanse(buf, sizeof buf);
      continue;
    }

    htttp_request_t req;
    bool resolved = false;

    if (htttp_parse_request(buf, len, &req) != HTTTP_OK)
      reply(400, NULL);
    else
      resolved = pre_auth_frame(&req);

    OPENSSL_cleanse(buf, len);

    if (resolved) {
      verdict = TAUTH_OK;
      break;
    }
    if (attempts_exhausted()) {
      /* The final 401 or 404 has already gone out. */
      verdict = TAUTH_DROP;
      break;
    }
  }

  OPENSSL_cleanse(buf, sizeof buf);
  return verdict;
}

bool tauth_offer(const htttp_request_t *req, const SessionState *st) {
  (void)st;

  if (auth_method_of(req) == AUTH_NONE)
    return false;

  if (g_sh == NULL)
    return false;

  reply(409, NULL);

  if (req->body != NULL && req->body_len > 0)
    OPENSSL_cleanse((void *)(uintptr_t)req->body, req->body_len);

  return true;
}

void tauth_name(char *dst, size_t cap) {
  if (dst == NULL || cap == 0)
    return;

  snprintf(dst, cap, "%s", g_name);
}
