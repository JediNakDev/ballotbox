#ifndef LIBTETRISAUTH_JWT_H
#define LIBTETRISAUTH_JWT_H

/**
 * @file jwt.h
 * @brief A JWT/HS256 minter and verifier that depends on nothing but OpenSSL.
 *
 * Called by libtetrisauth's login path to mint the token in the LOGIN and
 * REGISTER 200 body, which the client discards (#46 decision 5). Nothing in
 * tetriSH ever verifies one; verify exists because this layer is intended for
 * reuse in a more security-serious system, which wants the risky half too.
 *
 * WHAT TRAVELS IS TWO FILES: this header and jwt.c. base64url and the JSON
 * reader are statics inside jwt.c (#53), each having exactly one consumer.
 * The single requirement that constrains the pair is that it compiles in
 * another tree against <openssl/hmac.h> and <openssl/crypto.h> alone, which is
 * why it sits outside src/libtetrissh/ despite that library already having the
 * HMAC and CRYPTO_memcmp patterns - common.h drags in the whole certificate
 * surface, and buying a transport library to reach 15 lines is the wrong
 * trade. bin/test_jwt links -lcrypto only, so the boundary is a build failure.
 * It catches USING a symbol, not merely including a header: a bare #include
 * still compiles, and has to be caught by review.
 *
 * No allocation anywhere. Fixed buffers off JWT_MAX_LEN, no cleanup path,
 * nothing to leak in a forked session process.
 *
 * Two rules are held structurally rather than by comment. RFC 7519 s7.2's
 * "verify before you read claims": the buffers a claim would be read from live
 * inside check_header() and read_claims(), which jwt_verify() calls only after
 * check_signature() returns JWT_OK, so reading one early means moving a CALL.
 * And "sign the received bytes, never a re-encoding": split_token() is the only
 * thing that ever produces a signing input, and it does so before anything is
 * decoded.
 *
 * One deliberate deviation from s7.2's step order: alg is checked AFTER the
 * MAC. The RFC's order lets a multi-algorithm library learn which key to use;
 * we hard-code HS256 and never dispatch on alg, so deferring the header parse
 * reduces attacker-controlled parsing before the signature is trusted. An
 * `alg: none` token still fails either way.
 *
 * Header and payload strictness are asymmetric on purpose, and RFC-mandated:
 * the header rejects any key but alg and typ (s7.2 step 5), the payload
 * tolerates unknown claims (s4). All four claims are required - a jwt_claims_t
 * whose iat is permanently zero after a successful verify is a trap, and every
 * JWT claim being OPTIONAL means a token with no exp otherwise reads as
 * non-expiring. Leeway is zero: issuer and verifier are the same machine, and
 * s4.1.4 gives a ceiling but no number.
 *
 * Rationale in full: #45 (research), #46 (scheme), #53 (encoding).
 */

#include <stddef.h>
#include <time.h>

/** Longest token this can mint or accept, NUL included. The real maximum is
 * 185: header 36, payload 104 at a 15-character name and a 10-digit id,
 * signature 43, two dots. */
#define JWT_MAX_LEN        256
/** RFC 7518 3.2: a secret at least the hash output size. */
#define JWT_SECRET_MIN_LEN 32

typedef struct {
    long long sub;      /**< User id. */
    char      name[16]; /**< 15 characters plus NUL, #47's cap. */
    long long iat;      /**< Issued at; an input to jwt_mint(), not computed. */
    long long exp;      /**< Expiry; likewise. */
} jwt_claims_t;

/**
 * Writes a NUL-terminated token to out.
 *
 * Called by the login path once credentials check out. The secret is
 * (pointer, length) and iat/exp are inputs, so this file reads no file and
 * never calls time(NULL) - which is what makes expiry testable without clock
 * mocking, and a hardcoded known-answer vector possible at all.
 *
 * The name is re-validated against #47's allowlist rather than trusted: the
 * emitter needs no escaper only because that allowlist holds, and widening it
 * later would otherwise silently produce malformed JSON here.
 *
 * @param out         Receives the token.
 * @param out_len     Capacity of out.
 * @param claims      Claims to mint.
 * @param secret      Signing key.
 * @param secret_len  Key length; at least JWT_SECRET_MIN_LEN.
 * @returns 0, or -1 on a name outside the allowlist, a short secret, or an out
 *          buffer too small.
 */
int jwt_mint(char *out, size_t out_len, const jwt_claims_t *claims,
             const unsigned char *secret, size_t secret_len);

/**
 * Why a token was rejected.
 *
 * The distinct codes exist for the test suite, the only consumer that will
 * exist in this repo: they let a test assert that a flipped byte in segment 3
 * comes back JWT_E_SIGNATURE rather than passing because it happened to be
 * reported as malformed.
 *
 * THESE MUST NEVER REACH A NETWORK PEER. In this repo they cannot, because
 * nothing here verifies a token; the reuse project inherits the warning where
 * it can be tripped over.
 */
typedef enum {
    JWT_OK = 0,
    JWT_E_MALFORMED, /**< Not three segments, bad base64url, bad JSON shape. */
    JWT_E_SIGNATURE, /**< MAC mismatch, or a signature that is not 32 bytes. */
    JWT_E_ALG,       /**< alg absent, not "HS256", or an unknown header param. */
    JWT_E_CLAIMS,    /**< A required claim missing or malformed. */
    JWT_E_EXPIRED
} jwt_result_t;

/**
 * Verifies a token and, on JWT_OK, fills *out.
 *
 * Exercised only by tests; see the file comment for the step order and what is
 * required.
 *
 * @param token       NUL-terminated token.
 * @param secret      Verifying key.
 * @param secret_len  Key length.
 * @param now         Compared against exp with zero leeway; `now >= exp`
 *                    rejects, per RFC 7519 s4.1.4's "on or after".
 * @param out         Zeroed on entry and on every failure.
 * @returns JWT_OK, or the reason for rejection.
 */
jwt_result_t jwt_verify(const char *token,
                        const unsigned char *secret, size_t secret_len,
                        time_t now, jwt_claims_t *out);

#endif /* LIBTETRISAUTH_JWT_H */
