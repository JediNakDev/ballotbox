/**
 * @file credential.c
 * @brief The body split, #47's username rules, and PBKDF2.
 *
 * Phase 4e (#55). Contract in tauth_priv.h; the rules themselves are #47's and
 * are tabulated in docs/libtetrisauth.md.
 *
 * Everything here is pure: no socket, no file, no clock, no state. That is
 * what puts every rule in this file inside #54's section A, testable with
 * nothing running, and it is the reason the split between this file and
 * account.c is where it is.
 *
 * NOTHING HERE COPIES THE PASSWORD. cred_t is two pointers into the received
 * frame and PKCS5_PBKDF2_HMAC takes an explicit length, so the plaintext
 * exists in exactly one buffer from the recv that read it to the scrub that
 * clears it. A helper that took a NUL-terminated password would create a
 * second copy with its own scrub obligation, which is the shape #48 decision
 * 13 rejected.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "libcommon/limits.h"
#include "libcommon/playername.h"

#include "tauth_priv.h"

#define CRED_NAME_MAX 15  /* #47 decision 2, and MAX_PLAYER_NAME - 1 */
#define CRED_PASS_MIN 8   /* #47 decision 8, REGISTER only           */
#define CRED_PASS_MAX 128 /* #47 decision 8, REGISTER only           */
#define CRED_SALT_BYTES 16
#define CRED_DIGEST_BYTES 32

_Static_assert(CRED_NAME_MAX + 1 == MAX_PLAYER_NAME,
               "the username cap IS the roster name cap: #46 made the username "
               "the display name, so widening one without the other truncates "
               "people on the roster");

int cred_split(const uint8_t *body, uint32_t body_len, cred_t *out) {
  if (body == NULL || body_len == 0)
    return -1;

  /* The FIRST LF, so everything after it is the password whatever it
   * contains. A password holding an LF is legal and works; a username holding
   * one is impossible by construction rather than by a rule. */
  const uint8_t *lf = memchr(body, '\n', body_len);
  if (lf == NULL)
    return -1;

  out->user = (const char *)body;
  out->user_len = (size_t)(lf - body);
  out->pass = (const char *)(lf + 1);
  out->pass_len = body_len - out->user_len - 1;

  /* An empty field is 400 from #48's malformed-body rule, and it is checked
   * here for both fields so that no arm downstream has to remember to. */
  if (out->user_len == 0 || out->pass_len == 0)
    return -1;
  return 0;
}

int cred_name(const cred_t *c, char *dst, size_t cap) {
  /* The allowlist, the length and the fold are libcommon/playername.h's, so
   * this reader and the client's form validator cannot disagree about what a
   * legal name is. */
  return player_name_fold(dst, cap, c->user, c->user_len);
}

int cred_password_ok_for_register(const cred_t *c) {
  return (c->pass_len >= CRED_PASS_MIN && c->pass_len <= CRED_PASS_MAX) ? 0
                                                                        : -1;
}

static const char HEX[] = "0123456789abcdef";

static void hex_encode(const unsigned char *in, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = HEX[in[i] >> 4];
    out[i * 2 + 1] = HEX[in[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

static int hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static int hex_decode(const char *in, size_t in_len, unsigned char *out,
                      size_t cap) {
  if (in_len % 2 != 0 || in_len / 2 > cap)
    return -1;

  for (size_t i = 0; i < in_len; i += 2) {
    int hi = hex_nibble((unsigned char)in[i]);
    int lo = hex_nibble((unsigned char)in[i + 1]);
    if (hi < 0 || lo < 0)
      return -1;
    out[i / 2] = (unsigned char)((hi << 4) | lo);
  }
  return (int)(in_len / 2);
}

int cred_hash(const cred_t *c, const char *salt_hex, size_t salt_hex_len,
              int iters, char *out_hex, size_t cap) {
  unsigned char salt[CRED_SALT_BYTES];
  unsigned char digest[CRED_DIGEST_BYTES];
  int rc = -1;

  if (out_hex == NULL || cap < CRED_DIGEST_BYTES * 2 + 1 || iters < 1)
    return -1;

  int salt_len = hex_decode(salt_hex, salt_hex_len, salt, sizeof salt);
  if (salt_len <= 0)
    return -1;

  /* c->pass is not NUL-terminated and does not need to be: the length is
   * explicit, which is the whole reason the credential path can stay
   * zero-copy from the recv buffer to here. */
  if (PKCS5_PBKDF2_HMAC(c->pass, (int)c->pass_len, salt, salt_len, iters,
                        EVP_sha256(), (int)sizeof digest, digest) != 1)
    goto done;

  hex_encode(digest, sizeof digest, out_hex);
  rc = 0;

done:
  /* The digest is not the password, but it is the thing an attacker with the
   * stack wants next, and this frame is about to be reused by the reply
   * path. */
  OPENSSL_cleanse(digest, sizeof digest);
  OPENSSL_cleanse(salt, sizeof salt);
  return rc;
}

int cred_new_salt(char *out_hex, size_t cap) {
  unsigned char salt[CRED_SALT_BYTES];

  if (out_hex == NULL || cap < CRED_SALT_BYTES * 2 + 1)
    return -1;
  if (RAND_bytes(salt, sizeof salt) != 1)
    return -1;

  hex_encode(salt, sizeof salt, out_hex);
  return 0;
}
