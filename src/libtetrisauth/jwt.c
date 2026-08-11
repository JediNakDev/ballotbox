/**
 * @file jwt.c
 * @brief JWT/HS256 mint and verify.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "libtetrisutil/logmsg.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include "libtetrisauth/jwt.h"

/* ====================================================================== *
 * base64url - RFC 4648 s5, unpadded                                       *
 *                                                                         *
 * Statics rather than a second file pair: jwt.c is the only consumer, so   *
 * the seam was hypothetical. EVP_DecodeUpdate and BIO_f_base64 are not     *
 * options - both treat '-' as end-of-input, and '-' is a base64url data    *
 * character. EVP_DecodeBlock would make its caller re-pad the segment and  *
 * then walk it anyway to reject the whitespace, '+' and '/' it tolerates.  *
 * ====================================================================== */

/* The alphabet: A-Z, a-z, 0-9, '-', '_'. Encoding is a lookup - take a 6-bit
 * value 0..63, index in, get the character. */
static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/*
 * The reverse, one entry per possible byte value: index it with a character
 * and get that character's 6-bit value back, or -1 if the character is not in
 * the alphabet.
 *
 * 256 entries rather than a handful of range checks, because that makes RFC
 * 4648 s3.3 strictness structural. '+' and '/' are base64 but not base64url,
 * '=' is padding this file never emits, and space, CR and LF are the
 * whitespace RFC 7515 forbids. Every one of them lands on -1 and is rejected
 * because it was never put in the table, not because someone remembered to
 * check for it.
 *
 * Reading the rows: 0x20 carries '-'=62 alone; 0x30 starts '0'=52; 0x40 opens
 * on '@'=-1 then 'A'=0, 'B'=1 and on; 0x50 ends 'Z'=25 and carries '_'=63;
 * 0x60 starts 'a'=26. Every other slot is -1.
 */
static const signed char B64_DEC[256] = {
    /* 0x00 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x10 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x20 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    62,
    -1,
    -1,
    /* 0x30 */ 52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x40 */ -1,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    /* 0x50 */ 15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    -1,
    -1,
    -1,
    -1,
    63,
    /* 0x60 */ -1,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    /* 0x70 */ 41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x80 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x90 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xa0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xb0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xc0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xd0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xe0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0xf0 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
};

static int b64url_decoded_len(size_t in_len)
{
    size_t rem = in_len % 4;
    if (rem == 1)
        return -1;
    return (int)(in_len / 4 * 3 + (rem == 0 ? 0 : rem - 1));
}

static int b64url_decode(unsigned char *out, size_t out_cap, const char *in,
                         size_t in_len)
{
    int want = b64url_decoded_len(in_len);
    if (want < 0 || (size_t)want > out_cap)
        return -1;

    size_t o = 0, i = 0;
    for (; i + 4 <= in_len; i += 4)
    {
        int a = B64_DEC[(unsigned char)in[i]];
        int b = B64_DEC[(unsigned char)in[i + 1]];
        int c = B64_DEC[(unsigned char)in[i + 2]];
        int d = B64_DEC[(unsigned char)in[i + 3]];
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return -1;
        unsigned v = (unsigned)a << 18 | (unsigned)b << 12 | (unsigned)c << 6 |
                     (unsigned)d;
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
        out[o++] = (unsigned char)v;
    }

    size_t rem = in_len - i;
    if (rem >= 2)
    {
        int a = B64_DEC[(unsigned char)in[i]];
        int b = B64_DEC[(unsigned char)in[i + 1]];
        if (a < 0 || b < 0)
            return -1;
        unsigned v = (unsigned)a << 18 | (unsigned)b << 12;
        if (rem == 3)
        {
            int c = B64_DEC[(unsigned char)in[i + 2]];
            if (c < 0)
                return -1;
            v |= (unsigned)c << 6;
        }
        /*
         * The bits a short tail does not spend must be zero. Two characters
         * carry 12 bits and yield one byte, three carry 18 and yield two.
         * Requiring the remainder to be zero keeps the encoding canonical, so a
         * token cannot be rewritten into a different string that still
         * verifies. Every encoder, including the one below, zero-fills them.
         */
        if ((v & (rem == 2 ? 0xffffu : 0xffu)) != 0)
            return -1;
        out[o++] = (unsigned char)(v >> 16);
        if (rem == 3)
            out[o++] = (unsigned char)(v >> 8);
    }
    return (int)o;
}

static int b64url_encode(char *out, size_t out_cap, const unsigned char *in,
                         size_t in_len)
{
    size_t rem = in_len % 3;
    size_t need = in_len / 3 * 4 + (rem == 0 ? 0 : rem + 1);
    if (need + 1 > out_cap)
        return -1;

    size_t o = 0, i = 0;
    for (; i + 3 <= in_len; i += 3)
    {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 |
                     (unsigned)in[i + 2];
        out[o++] = B64_ENC[v >> 18 & 63];
        out[o++] = B64_ENC[v >> 12 & 63];
        out[o++] = B64_ENC[v >> 6 & 63];
        out[o++] = B64_ENC[v & 63];
    }
    if (rem > 0)
    {
        unsigned v = (unsigned)in[i] << 16;
        if (rem == 2)
            v |= (unsigned)in[i + 1] << 8;
        out[o++] = B64_ENC[v >> 18 & 63];
        out[o++] = B64_ENC[v >> 12 & 63];
        if (rem == 2)
            out[o++] = B64_ENC[v >> 6 & 63];
    }
    out[o] = '\0';
    return (int)o;
}

/* ====================================================================== *
 * The JSON read side - no writer, no value model                          *
 *                                                                         *
 * A single-pass walk handing back each top-level member as slices into the *
 * caller's buffer. Four properties, each chosen:                          *
 *                                                                         *
 *  - Nesting is rejected outright, which removes recursion, a depth limit  *
 *    and attacker-controlled stack growth as a category.                   *
 *  - String slices come back still escaped and are never unescaped. jwt.c  *
 *    compares names against an allowlist with no backslash in it, so an    *
 *    escaped name is a rejection rather than a decode.                     *
 *  - Duplicate keys reject the object. "Last one wins" is how a smuggled   *
 *    claim gets past a check that read the first one.                      *
 *  - Integers go through json_int, not strtol: the slice is not            *
 *    NUL-terminated and strtol stops silently at the first junk character. *
 *                                                                         *
 * Not a general JSON reader, and must not become one.                     *
 * ====================================================================== */

typedef enum
{
    JSON_STRING, /* slice excludes the quotes and is still escaped   */
    JSON_BARE    /* a bare token; only json_int() validates its text */
} json_kind_t;

/* One top-level member, as two slices into the buffer being walked. Nothing is
 * copied and nothing is NUL-terminated. */
typedef struct
{
    const char *key;
    size_t key_len;
    const char *val;
    size_t val_len;
    json_kind_t kind;
} json_member_t;

typedef struct
{
    const char *p; /* just past the last value yielded */
    const char *end;
    size_t count; /* members yielded so far */
} json_iter_t;

static void json_iter_init(json_iter_t *it, const char *buf, size_t len)
{
    it->p = buf;
    it->end = buf + len;
    it->count = 0;
}

/* Advance past spaces, tabs, CR and LF. */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Check that nothing but whitespace follows the closing brace. Trailing bytes
 * are a rejection, not something to stop reading at. */
static int json_end(const char *p, const char *end)
{
    return skip_ws(p, end) == end ? 0 : -1;
}

/*
 * Read a quoted string starting at *p, which must be the opening quote, and
 * hand back the slice BETWEEN the quotes, still escaped. On success *p lands
 * just past the closing quote.
 *
 * It understands '\' only well enough not to mistake \" for the end of the
 * string; it never decodes an escape. Raw control characters are rejected.
 */
static int json_string(const char **p, const char *end, const char **val,
                       size_t *val_len)
{
    const char *q = *p + 1;
    const char *start = q;

    while (q < end && *q != '"')
    {
        if ((unsigned char)*q < 0x20)
            return -1; /* raw control character; RFC 8259 s7 */
        if (*q == '\\')
        {
            q++; /* just enough understanding of '\' not to stop at \" */
            if (q == end)
                return -1;
        }
        q++;
    }
    if (q == end)
        return -1;

    *val = start;
    *val_len = (size_t)(q - start);
    *p = q + 1;
    return 0;
}

static int json_next(json_iter_t *it, json_member_t *m)
{
    const char *end = it->end;
    const char *p;

    if (it->count == 0)
    {
        p = skip_ws(it->p, end);
        if (p == end || *p++ != '{')
            return -1;
        p = skip_ws(p, end);
        if (p < end && *p == '}')
            return json_end(p + 1, end); /* {} */
    }
    else
    {
        p = skip_ws(it->p, end);
        if (p == end)
            return -1; /* no closing brace */
        if (*p == '}')
            return json_end(p + 1, end);
        if (*p++ != ',')
            return -1; /* two members with nothing between them */
        p = skip_ws(p, end);
        /* A '}' here is a trailing comma, and falls into the key check below.
         */
    }

    if (p == end || *p != '"')
        return -1;
    if (json_string(&p, end, &m->key, &m->key_len) != 0)
        return -1;

    p = skip_ws(p, end);
    if (p == end || *p++ != ':')
        return -1;
    p = skip_ws(p, end);
    if (p == end)
        return -1;

    if (*p == '"')
    {
        m->kind = JSON_STRING;
        if (json_string(&p, end, &m->val, &m->val_len) != 0)
            return -1;
    }
    else if (*p == '{' || *p == '[')
    {
        return -1; /* nesting; see the header */
    }
    else
    {
        const char *start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r')
            p++;
        if (p == start)
            return -1;
        m->kind = JSON_BARE;
        m->val = start;
        m->val_len = (size_t)(p - start);
    }

    it->p = p;
    it->count++;
    return 1;
}

static int json_slice_is(const char *s, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(s, lit, len) == 0;
}

/** Finds one key: 1 found, 0 absent, -1 malformed OR duplicated. The duplicate
 * rejection is the part that matters - "last one wins" is how a smuggled claim
 * slips past a check that read the first copy. */
static int json_find(const char *buf, size_t len, const char *key,
                     json_member_t *m)
{
    json_iter_t it;
    json_member_t cur;
    int found = 0, rc;

    json_iter_init(&it, buf, len);
    while ((rc = json_next(&it, &cur)) == 1)
    {
        if (!json_slice_is(cur.key, cur.key_len, key))
            continue;
        if (found)
            return -1; /* duplicate */
        found = 1;
        *m = cur;
    }
    if (rc < 0)
        return -1;
    return found;
}

/* LLONG_MIN is one short of representable here and is rejected as overflow; no
 * NumericDate or user id reaches it. */
/** Reads -?[0-9]+ over exactly the slice, which is not NUL-terminated. strtol
 * would accept leading whitespace and a '+' and stop silently at the first junk
 * character; leading zeros, trailing junk and overflow are rejected here. */
static int json_int(const char *s, size_t len, long long *out)
{
    size_t i = 0;
    int neg = 0;
    unsigned long long acc = 0;

    if (i < len && s[i] == '-')
    {
        neg = 1;
        i++;
    }
    if (i == len)
        return -1;
    if (s[i] == '0' && len - i > 1)
        return -1;

    for (; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        unsigned d = (unsigned)(s[i] - '0');
        if (acc > (unsigned long long)LLONG_MAX / 10)
            return -1;
        acc *= 10;
        if (acc > (unsigned long long)LLONG_MAX - d)
            return -1;
        acc += d;
    }
    *out = neg ? -(long long)acc : (long long)acc;
    return 0;
}

/* HMAC-SHA256 output size. Local rather than libtetrissh's HMAC_LEN, because
 * this file links against nothing but -lcrypto and that is the point of it. */
#define JWT_SIG_LEN 32

/* #47's username cap, which is why jwt_claims_t.name is char[16]. */
#define JWT_NAME_MAX 15

/* Longest payload the emitter below can produce: {"sub": 7 + 20 for a full
 * long long, ,"name":" 9 + 15 + " 1, ,"iat": 7 + 20, ,"exp": 7 + 20, } 1 =
 * 107. Rounded up, and snprintf is checked rather than trusted. */
#define JWT_PAYLOAD_MAX 128

/*
 * The header as the bytes it encodes, not as its precomputed base64url
 * constant eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9. The constant saves one call
 * on a fixed string and costs the source containing the thing it is encoding;
 * it would also make two encode paths where there is now one.
 */
static const char JWT_HDR_JSON[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

/* One HMAC(EVP_sha256(), ...) call into mac[JWT_SIG_LEN]. Returns 0 on
 * success. This and the single CRYPTO_memcmp in jwt_verify() are the entire
 * duplication from libtetrissh, and the entire price of this file being
 * copyable into another tree. */
static int hmac_sha256(unsigned char mac[JWT_SIG_LEN],
                       const unsigned char *secret, size_t secret_len,
                       const void *msg, size_t msg_len)
{
    unsigned int mac_len = 0;
    if (HMAC(EVP_sha256(), secret, (int)secret_len, (const unsigned char *)msg,
             msg_len, mac, &mac_len) == NULL)
        return -1;
    return mac_len == JWT_SIG_LEN ? 0 : -1;
}

/*
 * Is this #47's allowlist: [A-Za-z0-9_-], 1 to JWT_NAME_MAX characters.
 *
 * Not a JSON rule and not a token rule, and it lives here rather than beside
 * the reader because it is what makes the escaper-free emitter safe: no
 * character in that set needs escaping, so write_payload() can format a name
 * straight into a JSON string. Widen the allowlist and that emitter starts
 * producing malformed JSON, which is why jwt_mint() re-checks rather than
 * trusting its caller.
 *
 * Case is not folded here; folding is the account layer's job. #55 will want
 * this same rule for its 400, and at that point it belongs in tetrisauth.h's
 * pair with the fold - copying it there and deleting it here is the move, not
 * reaching into the token layer for it.
 */
static int name_ok(const char *s, size_t len)
{
    if (len == 0 || len > JWT_NAME_MAX)
        return 0;
    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

/* A secret this file will use: non-NULL, at least JWT_SECRET_MIN_LEN bytes,
 * and small enough for OpenSSL's int length parameter. */
static int secret_ok(const unsigned char *secret, size_t secret_len)
{
    return secret != NULL && secret_len >= JWT_SECRET_MIN_LEN &&
           secret_len <= (size_t)INT_MAX;
}

/* ====================================================================== *
 * Minting                                                                 *
 * ====================================================================== */

/*
 * The whole JSON writer: one snprintf over a fixed format. Returns the
 * payload's length, or -1.
 *
 * There is no escaper, and none is needed only because #47 constrains
 * usernames to [A-Za-z0-9_-]. That safety is a decision in another ticket, so
 * the name is re-validated here rather than trusted: widening the allowlist
 * later would otherwise silently produce malformed JSON in this function.
 */
static int write_payload(char *pay, size_t cap, const jwt_claims_t *claims)
{
    if (memchr(claims->name, '\0', sizeof claims->name) == NULL)
        return -1;
    if (!name_ok(claims->name, strlen(claims->name)))
        return -1;

    int n = snprintf(pay, cap,
                     "{\"sub\":%lld,\"name\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
                     claims->sub, claims->name, claims->iat, claims->exp);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/*
 * header '.' payload '.' signature, NUL-terminated. Returns the token's
 * length, or -1 if any of it does not fit.
 *
 * THE MAC COVERS THE BYTES THAT SHIP. When hmac_sha256() is called, tok[0..n)
 * is already the first two segments and the dot between them, so what is
 * signed is exactly what the verifier will receive - not a re-encoding of the
 * values it was built from.
 */
static int write_token(char *tok, size_t cap, const char *pay, size_t pay_len,
                       const unsigned char *secret, size_t secret_len)
{
    int k = b64url_encode(tok, cap, (const unsigned char *)JWT_HDR_JSON,
                          sizeof JWT_HDR_JSON - 1);
    if (k < 0)
        return -1;
    size_t n = (size_t)k;

    if (n + 1 >= cap)
        return -1;
    tok[n++] = '.';

    k = b64url_encode(tok + n, cap - n, (const unsigned char *)pay, pay_len);
    if (k < 0)
        return -1;
    n += (size_t)k;

    unsigned char mac[JWT_SIG_LEN];
    if (hmac_sha256(mac, secret, secret_len, tok, n) != 0)
        return -1;

    if (n + 1 >= cap)
        return -1;
    tok[n++] = '.';

    k = b64url_encode(tok + n, cap - n, mac, sizeof mac);
    if (k < 0)
        return -1;
    return (int)(n + (size_t)k);
}

/* Build the payload, build the token, hand it over. Returns -1 rather than
 * truncating if anything does not fit. Contract in jwt.h. */
int jwt_mint(char *out, size_t out_len, const jwt_claims_t *claims,
             const unsigned char *secret, size_t secret_len)
{
    if (claims != NULL)
        log_send(LOG_DEBUG, "minting authentication token subject=%lld user=%s",
                 claims->sub, claims->name);
    if (out != NULL && out_len > 0)
        out[0] = '\0';
    if (out == NULL || claims == NULL || !secret_ok(secret, secret_len))
    {
        (void)log_send(LOG_INFO, "operation=jwt_mint phase=complete status=-1");
        return -1;
    }

    char pay[JWT_PAYLOAD_MAX];
    int pay_len = write_payload(pay, sizeof pay, claims);
    if (pay_len < 0)
    {
        (void)log_send(LOG_INFO, "operation=jwt_mint phase=complete status=-1");
        return -1;
    }

    char tok[JWT_MAX_LEN];
    int tok_len =
        write_token(tok, sizeof tok, pay, (size_t)pay_len, secret, secret_len);
    if (tok_len < 0)
    {
        (void)log_send(LOG_INFO, "operation=jwt_mint phase=complete status=-1");
        return -1;
    }

    if ((size_t)tok_len + 1 > out_len)
    {
        (void)log_send(LOG_INFO, "operation=jwt_mint phase=complete status=-1");
        return -1;
    }
    memcpy(out, tok, (size_t)tok_len + 1);
    (void)log_send(LOG_INFO, "operation=jwt_mint phase=complete status=0");
    return 0;
}

/* ====================================================================== *
 * Verifying                                                               *
 *                                                                         *
 * Four steps, one function each, called in RFC 7519 s7.2's order by        *
 * jwt_verify() at the bottom. The split is what holds the file's two       *
 * silent-failure rules:                                                    *
 *                                                                          *
 *  - NOTHING DECODES A SEGMENT UNTIL check_signature() HAS RETURNED         *
 *    JWT_OK. The buffers a claim would be read from live inside             *
 *    check_header() and read_claims() and do not exist in jwt_verify()'s    *
 *    frame at all, so reading a claim early means moving a CALL, which is   *
 *    a visible edit rather than an accident.                                *
 *  - THE SIGNING INPUT IS NEVER REBUILT. split_token() hands back a pointer  *
 *    and a length into the caller's buffer and nothing else ever produces    *
 *    one, so there is no re-encoding available to sign by mistake.           *
 * ====================================================================== */

/* A slice of the token. Pointer and length into the CALLER'S buffer; nothing
 * here is ever copied. */
typedef struct
{
    const char *p;
    size_t len;
} seg_t;

typedef struct
{
    seg_t hdr, pay, sig;
    seg_t signing; /* hdr through the end of pay: the bytes the MAC covers */
} jwt_parts_t;

/* Exactly two dots and three non-empty segments. */
static jwt_result_t split_token(const char *token, size_t len, jwt_parts_t *out)
{
    const char *dot1 = memchr(token, '.', len);
    if (dot1 == NULL || dot1 == token)
        return JWT_E_MALFORMED;

    const char *dot2 = memchr(dot1 + 1, '.', len - (size_t)(dot1 + 1 - token));
    if (dot2 == NULL || dot2 == dot1 + 1)
        return JWT_E_MALFORMED;

    const char *sig = dot2 + 1;
    size_t sig_len = len - (size_t)(sig - token);
    if (sig_len == 0 || memchr(sig, '.', sig_len) != NULL)
        return JWT_E_MALFORMED;

    out->hdr.p = token;
    out->hdr.len = (size_t)(dot1 - token);
    out->pay.p = dot1 + 1;
    out->pay.len = (size_t)(dot2 - (dot1 + 1));
    out->sig.p = sig;
    out->sig.len = sig_len;

    /* The received bytes, verbatim. This is the only place a signing input is
     * ever constructed, and it is constructed before anything has been decoded
     * - so the re-encoding that would differ in whitespace or key order does
     * not exist to be signed. */
    out->signing.p = token;
    out->signing.len = (size_t)(dot2 - token);
    return JWT_OK;
}

/* Decode the signature, reject it on length BEFORE any comparison, then MAC
 * the signing input and compare in constant time. */
static jwt_result_t check_signature(seg_t signing, seg_t sig_seg,
                                    const unsigned char *secret,
                                    size_t secret_len)
{
    int want = b64url_decoded_len(sig_seg.len);
    if (want < 0)
        return JWT_E_MALFORMED;
    if (want != JWT_SIG_LEN)
        return JWT_E_SIGNATURE;

    unsigned char sig[JWT_SIG_LEN];
    if (b64url_decode(sig, sizeof sig, sig_seg.p, sig_seg.len) != JWT_SIG_LEN)
        return JWT_E_MALFORMED;

    unsigned char mac[JWT_SIG_LEN];
    if (hmac_sha256(mac, secret, secret_len, signing.p, signing.len) != 0)
        return JWT_E_SIGNATURE;
    return CRYPTO_memcmp(mac, sig, JWT_SIG_LEN) == 0 ? JWT_OK : JWT_E_SIGNATURE;
}

/*
 * The header, which is strict: RFC 7519 s7.2 step 5 says reject header
 * parameters that are not understood. The payload below is the opposite, and
 * the asymmetry is RFC-mandated rather than an oversight.
 *
 * alg is read here, after the MAC, and is compared rather than dispatched on.
 * See the deviation note in jwt.h. typ is accepted and its value ignored, per
 * s5.1, which makes it advisory.
 */
static jwt_result_t check_header(seg_t hdr)
{
    char json[JWT_MAX_LEN];
    int len = b64url_decode((unsigned char *)json, sizeof json, hdr.p, hdr.len);
    if (len < 0)
        return JWT_E_MALFORMED;

    json_iter_t it;
    json_member_t m;
    int alg_seen = 0, typ_seen = 0, rc;

    json_iter_init(&it, json, (size_t)len);
    while ((rc = json_next(&it, &m)) == 1)
    {
        if (json_slice_is(m.key, m.key_len, "typ"))
        {
            if (typ_seen++)
                return JWT_E_ALG;
            continue;
        }
        if (!json_slice_is(m.key, m.key_len, "alg"))
            return JWT_E_ALG;
        if (alg_seen++)
            return JWT_E_ALG;
        if (m.kind != JSON_STRING || !json_slice_is(m.val, m.val_len, "HS256"))
            return JWT_E_ALG;
    }
    if (rc < 0)
        return JWT_E_MALFORMED;
    return alg_seen ? JWT_OK : JWT_E_ALG;
}

/* One required integer claim. Absent, duplicated, quoted, fractional or
 * out of range are all rejections. */
static jwt_result_t claim_int(const char *json, size_t len, const char *key,
                              long long *out)
{
    json_member_t m;
    int rc = json_find(json, len, key, &m);
    if (rc < 0)
        return JWT_E_MALFORMED;
    if (rc == 0 || m.kind != JSON_BARE || json_int(m.val, m.val_len, out) != 0)
        return JWT_E_CLAIMS;
    return JWT_OK;
}

/*
 * The claims, which tolerate company: RFC 7519 s4 says claims that are not
 * understood MUST be ignored, so a fifth claim passes and nbf is neither
 * emitted nor checked.
 *
 * All four are required. #53's sketch left iat unread; requiring it makes
 * verify's output equal to mint's input, and a jwt_claims_t whose iat is
 * permanently zero after a successful verify is a trap for the next reader.
 * exp is required for the reason on the record: every JWT claim is OPTIONAL,
 * so a token with no exp otherwise reads as non-expiring.
 */
static jwt_result_t read_claims(seg_t pay, jwt_claims_t *out)
{
    char json[JWT_MAX_LEN];
    int len = b64url_decode((unsigned char *)json, sizeof json, pay.p, pay.len);
    if (len < 0)
        return JWT_E_MALFORMED;

    jwt_result_t rc;
    if ((rc = claim_int(json, (size_t)len, "sub", &out->sub)) != JWT_OK)
        return rc;
    if ((rc = claim_int(json, (size_t)len, "iat", &out->iat)) != JWT_OK)
        return rc;
    if ((rc = claim_int(json, (size_t)len, "exp", &out->exp)) != JWT_OK)
        return rc;

    json_member_t m;
    int found = json_find(json, (size_t)len, "name", &m);
    if (found < 0)
        return JWT_E_MALFORMED;
    /* The slice is still escaped and nothing here unescapes it. '\' is not in
     * #47's allowlist, so an escaped name is a rejection rather than a decode.
     */
    if (found == 0 || m.kind != JSON_STRING || !name_ok(m.val, m.val_len))
        return JWT_E_CLAIMS;
    memcpy(out->name, m.val, m.val_len);
    out->name[m.val_len] = '\0';
    return JWT_OK;
}

/* The step order, and nothing else. Contract in jwt.h. */
jwt_result_t jwt_verify(const char *token, const unsigned char *secret,
                        size_t secret_len, time_t now, jwt_claims_t *out)
{
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (token == NULL || out == NULL)
        return JWT_E_MALFORMED;
    /* A caller error rather than a property of the token, reported as the most
     * conservative rejection this enum has. */
    if (!secret_ok(secret, secret_len))
        return JWT_E_MALFORMED;

    size_t len = strnlen(token, JWT_MAX_LEN);
    if (len == JWT_MAX_LEN)
        return JWT_E_MALFORMED; /* longer than anything this file mints */

    jwt_parts_t parts;
    jwt_result_t rc = split_token(token, len, &parts);
    if (rc != JWT_OK)
        return rc;

    rc = check_signature(parts.signing, parts.sig, secret, secret_len);
    if (rc != JWT_OK)
        return rc;

    /* Past this point the bytes are trusted, and only past this point is any of
     * them decoded. */
    rc = check_header(parts.hdr);
    if (rc != JWT_OK)
        return rc;

    rc = read_claims(parts.pay, out);
    if (rc != JWT_OK)
    {
        memset(out, 0, sizeof *out);
        return rc;
    }

    /* Leeway is zero, and s4.1.4's wording is "on or after". */
    if ((long long)now >= out->exp)
    {
        memset(out, 0, sizeof *out);
        return JWT_E_EXPIRED;
    }
    return JWT_OK;
}
