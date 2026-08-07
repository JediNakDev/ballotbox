/* Tests for the JWT layer (#46, #53, #54).
 *
 * Needs no runner, no socket and no skip path: jwt.c takes its secret as
 * (pointer, length) and its clock as an explicit time_t, which was chosen for
 * exactly this.
 *
 * THIS TARGET IS ALSO AN ARCHITECTURAL ASSERTION. It compiles against
 * src/libtetrisauth/jwt.c and links -lcrypto only, so an
 * #include "libtetrissh/..." in it stops it linking. The portability claim
 * that file rests on is a build failure rather than a review comment.
 *
 * Every case here is black box through jwt_mint() and jwt_verify(), including
 * the base64url and JSON ones. Both are reachable directly now that they are
 * their own files, and these stay at the boundary on purpose: a helper that is
 * correct in isolation and wired up wrong is the failure this suite has to
 * catch. The padding residues are
 * covered by walking #47's 1..15 character name range, which moves the payload
 * length through every residue mod 3; '-' and '_' in the output are pinned by
 * the known-answer vector, whose signature segment contains both.
 *
 * The tokens that must not be mintable - a `none` alg, a third header
 * parameter, a reordered payload - are built here by build_signed(), which
 * base64url-encodes and HMACs whatever JSON it is handed. Without it the
 * important cases would die at the MAC check and stop testing what they were
 * written to test.
 *
 * Run from the repo root: make test */
#include <stdio.h>
#include <string.h>

#include <openssl/hmac.h>

#include "libtetrisauth/jwt.h"

/* The secret every case signs and verifies under: 32 bytes, the RFC 7518 3.2
 * minimum. Fixed rather than random, because the known-answer vector below
 * depends on its exact bytes. */
#define SECRET "0123456789abcdef0123456789abcdef"
#define SECRET_LEN (sizeof(SECRET) - 1)

/* The clock, injected rather than read: iat, an exp one hour later, and a now
 * that sits before it. Every case that is not about expiry uses NOW, so a
 * failure there is never an expiry failure in disguise. */
#define IAT 1700000000LL
#define EXP 1700003600LL
#define NOW ((time_t)1700000000)

static const unsigned char *secret = (const unsigned char *)SECRET;

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static void run(const char *name, int (*fn)(void)) {
  printf("  %s\n", name);
  tests_run++;
  if (fn() != 0)
    tests_failed++;
}

/* === Building tokens jwt_mint() would refuse to produce === */

/* The base64url alphabet again, on this side of the boundary. */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Bytes to unpadded base64url, for building token segments by hand.
 *
 * Deliberately a second, independent implementation rather than a call into
 * b64url.c: if this one and the library's ever disagree, the known-answer
 * vector is the third opinion that says which is wrong. */
static size_t t_b64(char *out, const void *vin, size_t n) {
  const unsigned char *in = vin;
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
    out[o++] = B64[v >> 18 & 63];
    out[o++] = B64[v >> 12 & 63];
    out[o++] = B64[v >> 6 & 63];
    out[o++] = B64[v & 63];
  }
  if (n - i > 0) {
    unsigned v = (unsigned)in[i] << 16;
    if (n - i == 2)
      v |= (unsigned)in[i + 1] << 8;
    out[o++] = B64[v >> 18 & 63];
    out[o++] = B64[v >> 12 & 63];
    if (n - i == 2)
      out[o++] = B64[v >> 6 & 63];
  }
  out[o] = '\0';
  return o;
}

/* Build a fully valid token out of whatever header and payload JSON it is
 * handed: encode both as the first two segments, then append the HMAC of those
 * bytes under sec.
 *
 * This is what lets the alg and header-strictness cases reach the code they
 * were written for. Without it they would die at the MAC check, pass, and test
 * nothing. */
static void build_signed(char *out, const char *hdr, const char *pay,
                         const unsigned char *sec, size_t sec_len) {
  size_t n = t_b64(out, hdr, strlen(hdr));
  out[n++] = '.';
  n += t_b64(out + n, pay, strlen(pay));

  unsigned char mac[32];
  unsigned int mac_len = 0;
  HMAC(EVP_sha256(), sec, (int)sec_len, (const unsigned char *)out, n, mac,
       &mac_len);

  out[n++] = '.';
  t_b64(out + n, mac, sizeof mac);
}

/* Same, but with the signature segment supplied verbatim, so one token's
 * signature can be pasted onto another token's bytes. Only the reordered-keys
 * case needs this. */
static void build_with_sig(char *out, const char *hdr, const char *pay,
                           const char *sig_seg) {
  size_t n = t_b64(out, hdr, strlen(hdr));
  out[n++] = '.';
  n += t_b64(out + n, pay, strlen(pay));
  out[n++] = '.';
  strcpy(out + n, sig_seg);
}

/* Locate segment 0, 1 or 2 of a token: returns a pointer to its first
 * character and writes its length. */
static const char *segment(const char *token, int which, size_t *len) {
  const char *p = token;
  for (int i = 0; i < which; i++)
    p = strchr(p, '.') + 1;
  const char *dot = strchr(p, '.');
  *len = dot != NULL ? (size_t)(dot - p) : strlen(p);
  return p;
}

/* Corrupt one character in the middle of a segment, replacing it with a
 * different ALPHABET character so the token stays well-formed base64url and
 * only the bytes it encodes change. That is what makes the expected answer
 * JWT_E_SIGNATURE rather than JWT_E_MALFORMED. */
static void flip_in_segment(char *token, int which) {
  size_t len;
  char *p = (char *)segment(token, which, &len);
  size_t at = len / 2;
  p[at] = p[at] == 'A' ? 'B' : 'A';
}

/* The exact header and payload jwt_mint() emits for the claims below, spelled
 * out so build_signed() can produce near-misses of them. */
static const char CANONICAL_HEADER[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
static const char CANONICAL_PAYLOAD[] =
    "{\"sub\":42,\"name\":\"alice\",\"iat\":1700000000,\"exp\":1700003600}";

/* The one good token the tampering cases start from. */
static int mint_canonical(char *tok) {
  jwt_claims_t c = {0};
  c.sub = 42;
  strcpy(c.name, "alice");
  c.iat = IAT;
  c.exp = EXP;
  return jwt_mint(tok, JWT_MAX_LEN, &c, secret, SECRET_LEN);
}

/* === Round trip and encoding === */

/* Mint a token, verify it, and check all four claims come back unchanged. */
static int test_round_trip(void) {
  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_OK,
        "verify rejected our own token");
  CHECK(got.sub == 42, "sub did not survive");
  CHECK(strcmp(got.name, "alice") == 0, "name did not survive");
  CHECK(got.iat == IAT, "iat did not survive");
  CHECK(got.exp == EXP, "exp did not survive");
  return 0;
}

/*
 * One hardcoded secret/claims/expected-token vector, computed outside this
 * tree, so a refactor that silently changes the encoding fails loudly. Its
 * signature segment contains both '-' and '_', which is the only place the
 * two base64url-specific characters are pinned.
 */
static int test_known_answer(void) {
  static const char EXPECTED[] =
      "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
      "eyJzdWIiOjQyLCJuYW1lIjoiYWxpY2UiLCJpYXQiOjE3MDAwMDAwMDAsImV4cCI6MTcwMDAw"
      "MzYwMH0."
      "MBM2Wa0pNdy0aWHWYuNacf8Jw-n1Gcfl1uuYAN_icQI";

  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");
  CHECK(strcmp(tok, EXPECTED) == 0, "minted token differs from the vector");
  CHECK(strchr(tok, '-') != NULL && strchr(tok, '_') != NULL,
        "vector no longer exercises the base64url alphabet");
  return 0;
}

/* Names of 1 to 15 characters, which is #47's whole range. It walks the
 * payload length through every residue mod 3, and so through every base64url
 * tail length - the padding cases, reached from outside. */
static int test_every_name_length_round_trips(void) {
  for (size_t n = 1; n <= 15; n++) {
    jwt_claims_t c = {0};
    c.sub = 7;
    memset(c.name, 'a', n);
    c.name[n] = '\0';
    c.iat = IAT;
    c.exp = EXP;

    char tok[JWT_MAX_LEN];
    CHECK(jwt_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == 0,
          "mint failed for some name length");

    jwt_claims_t got;
    CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_OK,
          "verify failed for some name length");
    CHECK(strcmp(got.name, c.name) == 0, "name did not survive a length");
  }
  return 0;
}

/* === Tampering === */

/* One character changed in the header, then the payload, then the signature.
 * All three must come back JWT_E_SIGNATURE. */
static int test_flipped_byte_in_each_segment(void) {
  for (int seg = 0; seg < 3; seg++) {
    char tok[JWT_MAX_LEN];
    CHECK(mint_canonical(tok) == 0, "mint failed");
    flip_in_segment(tok, seg);

    jwt_claims_t got;
    /* The exact code matters: if a flipped signature came back malformed the
     * case would pass without ever reaching the MAC compare. */
    CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_SIGNATURE,
          "a flipped byte was not reported as a signature failure");
  }
  return 0;
}

/* A good token presented under a different key. */
static int test_wrong_secret(void) {
  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");

  const unsigned char other[] = "0123456789abcdef0123456789abcdeF";
  jwt_claims_t got;
  CHECK(jwt_verify(tok, other, sizeof other - 1, NOW, &got) == JWT_E_SIGNATURE,
        "a token verified under the wrong secret");
  return 0;
}

/* A signature segment one character short, so it decodes to 31 bytes and must
 * be refused on length before any comparison happens. */
static int test_signature_of_the_wrong_length(void) {
  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");
  tok[strlen(tok) - 1] = '\0'; /* 42 characters decode to 31 bytes */

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_SIGNATURE,
        "a short signature was not rejected on length");
  return 0;
}

/* Tokens that are not three non-empty segments of base64url: empty, two
 * segments, four segments, an empty first segment, and a '+' (base64 but not
 * base64url). */
static int test_malformed_shapes(void) {
  jwt_claims_t got;
  char tok[JWT_MAX_LEN];

  CHECK(jwt_verify("", secret, SECRET_LEN, NOW, &got) == JWT_E_MALFORMED,
        "empty token accepted");
  CHECK(jwt_verify("a.b", secret, SECRET_LEN, NOW, &got) == JWT_E_MALFORMED,
        "two segments accepted");
  CHECK(jwt_verify("a.b.c.d", secret, SECRET_LEN, NOW, &got) == JWT_E_MALFORMED,
        "four segments accepted");
  CHECK(jwt_verify(".b.c", secret, SECRET_LEN, NOW, &got) == JWT_E_MALFORMED,
        "empty first segment accepted");

  /* '+' is base64 but not base64url, and it sits in the signature segment
   * whose decode runs before the compare. */
  CHECK(mint_canonical(tok) == 0, "mint failed");
  size_t len;
  char *sig = (char *)segment(tok, 2, &len);
  sig[len / 2] = '+';
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_MALFORMED,
        "'+' accepted in a base64url segment");
  return 0;
}

/* === alg === */

/*
 * The single most important case in this file: a `none` token carrying a
 * genuinely valid HS256 signature over the none header. Built rather than
 * hand-written, so it dies at the alg compare and not at the MAC check.
 */
static int test_alg_none_with_a_valid_signature(void) {
  char tok[JWT_MAX_LEN];
  build_signed(tok, "{\"alg\":\"none\",\"typ\":\"JWT\"}", CANONICAL_PAYLOAD,
               secret, SECRET_LEN);

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_ALG,
        "an alg:none token with a valid MAC was not rejected on alg");
  return 0;
}

/* A properly signed HS512 header, and a header carrying no alg at all. */
static int test_other_algs_rejected(void) {
  char tok[JWT_MAX_LEN];
  jwt_claims_t got;

  build_signed(tok, "{\"alg\":\"HS512\",\"typ\":\"JWT\"}", CANONICAL_PAYLOAD,
               secret, SECRET_LEN);
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_ALG,
        "HS512 accepted");

  build_signed(tok, "{\"typ\":\"JWT\"}", CANONICAL_PAYLOAD, secret, SECRET_LEN);
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_ALG,
        "a header with no alg accepted");
  return 0;
}

/*
 * The two halves of the RFC-mandated asymmetry, one case each, because either
 * one alone reads as a bug: unknown header parameters MUST be rejected (7519
 * 7.2 step 5) and unknown claims MUST be ignored (7519 4).
 */
/* A third header parameter (kid) alongside alg and typ, properly signed. */
static int test_third_header_parameter_rejected(void) {
  char tok[JWT_MAX_LEN];
  build_signed(tok, "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"kid\":\"k1\"}",
               CANONICAL_PAYLOAD, secret, SECRET_LEN);

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_ALG,
        "an unknown header parameter was accepted");
  return 0;
}

/* A fifth claim in the payload, properly signed: it must verify, and the four
 * known claims must still read correctly. */
static int test_fifth_claim_ignored(void) {
  char tok[JWT_MAX_LEN];
  build_signed(tok, CANONICAL_HEADER,
               "{\"sub\":42,\"name\":\"alice\",\"iat\":1700000000,"
               "\"exp\":1700003600,\"role\":\"admin\"}",
               secret, SECRET_LEN);

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_OK,
        "an unknown claim was not ignored");
  CHECK(got.sub == 42 && strcmp(got.name, "alice") == 0,
        "the known claims were misread");
  return 0;
}

/* === Claims === */

/* The three points around exp: one second before passes, now == exp fails,
 * one second after fails. */
static int test_expiry_boundary(void) {
  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, (time_t)(EXP - 1), &got) == JWT_OK,
        "one second before exp was rejected");
  /* Leeway is zero and 4.1.4's wording is "on or after", so this is the edge
   * that must not drift. */
  CHECK(jwt_verify(tok, secret, SECRET_LEN, (time_t)EXP, &got) == JWT_E_EXPIRED,
        "now == exp was accepted");
  CHECK(jwt_verify(tok, secret, SECRET_LEN, (time_t)(EXP + 1), &got) ==
            JWT_E_EXPIRED,
        "an expired token was accepted");
  return 0;
}

/* A properly signed token with no exp claim at all. */
static int test_missing_exp_is_not_eternal(void) {
  char tok[JWT_MAX_LEN];
  build_signed(tok, CANONICAL_HEADER,
               "{\"sub\":42,\"name\":\"alice\",\"iat\":1700000000}", secret,
               SECRET_LEN);

  jwt_claims_t got;
  CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == JWT_E_CLAIMS,
        "a token with no exp was accepted");
  return 0;
}

/*
 * The regression guard for someone restructuring jwt_verify(): the signature
 * covers the received bytes, so the same claims re-serialized in another key
 * order must fail. This passes on day one - #53's shape makes it structurally
 * unavailable rather than merely avoided.
 */
static int test_reordered_payload_fails(void) {
  char tok[JWT_MAX_LEN];
  CHECK(mint_canonical(tok) == 0, "mint failed");

  size_t len;
  const char *sig = segment(tok, 2, &len);
  char sig_seg[64];
  memcpy(sig_seg, sig, len);
  sig_seg[len] = '\0';

  char reordered[JWT_MAX_LEN];
  build_with_sig(reordered, CANONICAL_HEADER,
                 "{\"name\":\"alice\",\"sub\":42,\"exp\":1700003600,"
                 "\"iat\":1700000000}",
                 sig_seg);

  jwt_claims_t got;
  CHECK(jwt_verify(reordered, secret, SECRET_LEN, NOW, &got) == JWT_E_SIGNATURE,
        "a re-serialized payload verified under the original signature");
  return 0;
}

/* Payloads that are signed correctly and still wrong: a duplicate claim, a
 * nested value, exp as a string, exp as 1.5, an escaped name, a 16 character
 * name, and a missing sub. */
static int test_bad_payload_shapes(void) {
  struct {
    const char *pay;
    jwt_result_t want;
    const char *msg;
  } cases[] = {
      {"{\"sub\":42,\"sub\":7,\"name\":\"alice\",\"iat\":1700000000,"
       "\"exp\":1700003600}",
       JWT_E_MALFORMED, "a duplicate claim was accepted"},
      {"{\"sub\":{\"a\":1},\"name\":\"alice\",\"iat\":1700000000,"
       "\"exp\":1700003600}",
       JWT_E_MALFORMED, "a nested value was accepted"},
      {"{\"sub\":42,\"name\":\"alice\",\"iat\":1700000000,"
       "\"exp\":\"1700003600\"}",
       JWT_E_CLAIMS, "exp as a string was accepted"},
      {"{\"sub\":42,\"name\":\"alice\",\"iat\":1700000000,\"exp\":1.5}",
       JWT_E_CLAIMS, "a non-integer exp was accepted"},
      {"{\"sub\":42,\"name\":\"\\u0061lice\",\"iat\":1700000000,"
       "\"exp\":1700003600}",
       JWT_E_CLAIMS, "an escaped name was decoded rather than rejected"},
      {"{\"sub\":42,\"name\":\"abcdefghijklmnop\",\"iat\":1700000000,"
       "\"exp\":1700003600}",
       JWT_E_CLAIMS, "a 16 character name was accepted"},
      {"{\"name\":\"alice\",\"iat\":1700000000,\"exp\":1700003600}",
       JWT_E_CLAIMS, "a token with no sub was accepted"},
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    char tok[JWT_MAX_LEN];
    build_signed(tok, CANONICAL_HEADER, cases[i].pay, secret, SECRET_LEN);
    jwt_claims_t got;
    CHECK(jwt_verify(tok, secret, SECRET_LEN, NOW, &got) == cases[i].want,
          cases[i].msg);
  }
  return 0;
}

/* === jwt_mint's own refusals === */

/* The emitter has no escaper and is safe only by virtue of #47's allowlist,
 * so it re-validates rather than trusting its caller. */
static int test_mint_rejects_names_outside_the_allowlist(void) {
  static const char *bad[] = {"ali\"ce", "ali\\ce", "ali ce", "ali.ce", ""};

  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    jwt_claims_t c = {0};
    c.sub = 1;
    strcpy(c.name, bad[i]);
    c.iat = IAT;
    c.exp = EXP;

    char tok[JWT_MAX_LEN];
    CHECK(jwt_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
          "a name outside the allowlist was minted");
    CHECK(tok[0] == '\0', "a rejected mint left something in the buffer");
  }
  return 0;
}

/* A name filling all 16 bytes of the field with no NUL. */
static int test_mint_rejects_an_unterminated_name(void) {
  jwt_claims_t c = {0};
  c.sub = 1;
  memset(c.name, 'a', sizeof c.name); /* all 16 bytes, no NUL */
  c.iat = IAT;
  c.exp = EXP;

  char tok[JWT_MAX_LEN];
  CHECK(jwt_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
        "an unterminated name was minted");
  return 0;
}

/* A 31 byte secret, on both sides: mint must refuse it and verify must not
 * return JWT_OK under it. */
static int test_short_secret(void) {
  const unsigned char short_secret[] = "0123456789abcdef0123456789abcde"; /*31*/
  jwt_claims_t c = {0};
  c.sub = 1;
  strcpy(c.name, "alice");
  c.iat = IAT;
  c.exp = EXP;

  char tok[JWT_MAX_LEN];
  CHECK(jwt_mint(tok, sizeof tok, &c, short_secret, sizeof short_secret - 1) ==
            -1,
        "minted under a secret shorter than the hash output");

  CHECK(mint_canonical(tok) == 0, "mint failed");
  jwt_claims_t got;
  CHECK(jwt_verify(tok, short_secret, sizeof short_secret - 1, NOW, &got) !=
            JWT_OK,
        "verified under a secret shorter than the hash output");
  return 0;
}

/* An output buffer too small for the token: mint refuses rather than
 * truncating, and leaves nothing behind in it. */
static int test_mint_refuses_a_small_buffer(void) {
  jwt_claims_t c = {0};
  c.sub = 42;
  strcpy(c.name, "alice");
  c.iat = IAT;
  c.exp = EXP;

  char tok[64];
  CHECK(jwt_mint(tok, sizeof tok, &c, secret, SECRET_LEN) == -1,
        "a token was written into a buffer too small for it");
  CHECK(tok[0] == '\0', "a rejected mint left something in the buffer");
  return 0;
}

int main(void) {
  printf("test_jwt\n");

  run("mints and verifies its own token", test_round_trip);
  run("matches the known-answer vector", test_known_answer);
  run("round trips every allowed name length",
      test_every_name_length_round_trips);

  run("rejects a flipped byte in each segment",
      test_flipped_byte_in_each_segment);
  run("rejects a token signed under another secret", test_wrong_secret);
  run("rejects a signature that is not 32 bytes",
      test_signature_of_the_wrong_length);
  run("rejects malformed segment shapes", test_malformed_shapes);

  run("rejects alg:none carrying a valid HS256 signature",
      test_alg_none_with_a_valid_signature);
  run("rejects other algs and a missing alg", test_other_algs_rejected);
  run("rejects an unknown header parameter",
      test_third_header_parameter_rejected);
  run("ignores an unknown claim", test_fifth_claim_ignored);

  run("expires on or after exp, with no leeway", test_expiry_boundary);
  run("treats a missing exp as a failure, not as eternal",
      test_missing_exp_is_not_eternal);
  run("rejects a payload re-serialized with reordered keys",
      test_reordered_payload_fails);
  run("rejects malformed and out-of-range claims", test_bad_payload_shapes);

  run("refuses to mint a name outside #47's allowlist",
      test_mint_rejects_names_outside_the_allowlist);
  run("refuses to mint an unterminated name",
      test_mint_rejects_an_unterminated_name);
  run("refuses a secret shorter than the hash output", test_short_secret);
  run("refuses an output buffer too small", test_mint_refuses_a_small_buffer);

  printf("%d test(s), %d failed\n", tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
