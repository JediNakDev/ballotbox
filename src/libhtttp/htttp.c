#include "libhtttp/htttp.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- Static helpers -------------------------------------------------------
 *
 * Every bound check in this file routes through find_crlf / copy_token /
 * parse_uint, so the over-read surface for the whole library is these three
 * functions. Review them once and the parsers above are just composition.
 */

/* Case-insensitive compare of two NUL-terminated strings. Returns 1 on match.
 *
 * HTTP header names are case-insensitive ("Player-Id" == "player-id"), so
 * plain strcmp is wrong for lookups. Hand-rolled rather than strcasecmp,
 * which is POSIX and drags in feature-test-macro noise. Values are cast
 * through unsigned char because plain char may be signed, and a byte >= 0x80
 * would then go negative and break the range check. */
static int ci_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;

    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return 0;
  }
  /* The loop stops when EITHER string ends, so both must have ended for a
   * real match — without this, "Player" would match "Player-Id". */
  return *a == '\0' && *b == '\0';
}

/* Find the next CRLF at or after `from`. Writes the index of the '\r' to
 * *out. Returns 0 found / -1 not found.
 *
 * Strict CRLF only (decision D1): a bare LF is not a line break here. Both
 * peers are our own code, so there is no legacy leniency to preserve, and one
 * accepted terminator keeps the parser small enough to reason about. */
static int find_crlf(const uint8_t *buf, size_t len, size_t from, size_t *out) {
  size_t index = from;
  /* index + 1 < len, NOT index < len: testing buf[index + 1] at the last
   * byte would read one past the end. */
  while (index + 1 < len) {
    if (buf[index] == '\r' && buf[index + 1] == '\n') {
      *out = index;
      return 0;
    }
    index++;
  }
  return -1;
}

/* Copy `len` wire bytes into a fixed-size field and NUL-terminate.
 *
 * This is the boundary where unterminated wire bytes become a C string —
 * hence const uint8_t * in, char * out. Use it for string struct fields only;
 * numbers are parsed in place by parse_uint, never copied first. */
static int copy_token(char *dst, size_t cap, const uint8_t *src, size_t len) {
  if (!dst || !src)
    return HTTTP_ERR_MALFORMED;
  /* >= not >: dst[len] = '\0' writes at index len, so the field must hold
   * the bytes AND the terminator. */
  if (len >= cap)
    return HTTTP_ERR_TOOLONG;

  /* A NUL byte inside the token is malformed, not data.
   *
   * This is the line where wire bytes become a C string, so an embedded NUL
   * makes the two disagree about where the token ends: "\0Cert-Name" is
   * eleven bytes on the wire and the empty string to every strcmp after it,
   * including htttp_header_get's. The parser would accept the header and
   * the app would never find it - one peer sees an identity header, the
   * other sees none, which is the shape header smuggling takes. The same
   * applies to a method or path with a NUL in it, and both route through
   * here, which is why the check lives in this function rather than at the
   * four call sites. No honest peer is affected: this library's own
   * serializer writes C strings and so can never emit one.
   * Found by tests/fuzz/fuzz_htttp_request. */
  if (memchr(src, '\0', len) != NULL)
    return HTTTP_ERR_MALFORMED;

  memcpy(dst, src, len);
  dst[len] = '\0';
  return HTTTP_OK;
}

/* Parse `len` bytes as an unsigned decimal, in place. Returns 0 on success.
 *
 * Takes a length rather than a NUL-terminated string because it reads
 * directly out of the wire buffer, where nothing is terminated. Digits only —
 * no sign, no whitespace, no 0x. strtoul accepts all of those and reports
 * failure through errno, which is more surface than we want on
 * attacker-controlled bytes. */
static int parse_uint(const uint8_t *src, size_t len, uint32_t *out) {
  uint32_t value = 0;
  /* len == 0 is an error, not zero: "Content-Length: " with nothing after
   * the space must not read as a zero-length body. */
  if (!src || !out || len == 0)
    return HTTTP_ERR_MALFORMED;
  if (len > 32)
    return HTTTP_ERR_TOOLONG;
  for (size_t i = 0; i < len; i++) {
    /* Digits are contiguous in ASCII ('0' = 0x30 .. '9' = 0x39), so a
     * range check is enough — and src[i] - '0' gives the value. */
    if (src[i] < '0' || src[i] > '9')
      return HTTTP_ERR_MALFORMED;
    uint32_t digit = (uint32_t)(src[i] - '0');
    /* Guard BEFORE the multiply — unsigned wraparound is silent, so
     * checking afterwards is too late to detect. */
    if (value > (UINT32_MAX - digit) / 10)
      return HTTTP_ERR_TOOLONG;
    value = value * 10 + (src[i] - '0');
  }
  *out = value;
  return 0;
}
/* --- Parsing -------------------------------------------------------------- */
/* Everything after the first line: the header block, then the body.
 *
 * Requests and responses differ ONLY in line 1, so both parsers share this.
 * That is deliberate — in libtetrissh the mirrored connect/accept pair kept
 * growing twin bugs because a fix in one was not swept into the other.
 * Sharing the common half removes most of that exposure by construction.
 *
 * `off` is the index just past line 1's CRLF. */
static int parse_tail(const uint8_t *buf, size_t len, size_t off,
                      htttp_header_t *headers, size_t *n_headers,
                      const uint8_t **body, uint32_t *body_len) {
  /* --- Phase 1: header lines --------------------------------------------
   * Two exits only: the blank line (break) or malformed (return). A
   * `while (found)` loop would treat "ran out of buffer" as normal
   * termination, so a truncated frame would parse as a valid message. */
  for (;;) {
    size_t eol;
    if (find_crlf(buf, len, off, &eol) != 0)
      return HTTTP_ERR_MALFORMED; /* no blank line ever arrived */

    /* A CRLF right at the current position means an empty line: the
     * \r\n\r\n separator. Headers are done, body starts at off + 2. */
    if (eol == off) {
      off += 2;
      break;
    }

    /* header = KEY ":" SP VALUE. Scan bounded by eol, never len, so a
     * colon on a later line cannot be picked up for this one. */
    size_t colon = off;
    while (colon < eol && buf[colon] != ':')
      colon++;
    /* Bounds are checked before buf[colon + 1] is read, not after. */
    if (colon == off || colon >= eol || colon + 1 >= eol ||
        buf[colon + 1] != ' ')
      return HTTTP_ERR_MALFORMED;

    if (*n_headers >= HTTTP_MAX_HEADERS)
      return HTTTP_ERR_TOOLONG;

    /* key = [off, colon), value = [colon + 2, eol) — the +2 skips ": ".
     * An empty value is legal: "Key: " yields "". */
    int rc = copy_token(headers[*n_headers].key, HTTTP_MAX_KEY, buf + off,
                        colon - off);
    if (rc != HTTTP_OK)
      return rc;
    rc = copy_token(headers[*n_headers].value, HTTTP_MAX_VALUE, buf + colon + 2,
                    eol - colon - 2);
    if (rc != HTTTP_OK)
      return rc;
    (*n_headers)++;
    off = eol + 2;
  }

  /* --- Phase 2: body ----------------------------------------------------
   * Cross-check what the sender declared against what actually arrived.
   * Trusting "Content-Length: 5000" on a 10-byte message would hand the app
   * a pointer with a 5000-byte promise behind it — a heap over-read waiting
   * to happen. The equality check below is the defence.
   *
   * Runs after the loop because it looks up a header the loop just filled. */
  size_t remaining = len - off;
  const char *cl = htttp_header_get(headers, *n_headers, "Content-Length");
  if (cl == NULL) {
    /* No declared length means no body. Trailing bytes are unaccounted
     * for, so reject rather than silently ignore them. */
    if (remaining != 0)
      return HTTTP_ERR_LENGTH;
    *body = NULL;
    *body_len = 0;
    return HTTTP_OK;
  }
  uint32_t declared;
  if (parse_uint((const uint8_t *)cl, strlen(cl), &declared) != 0)
    return HTTTP_ERR_MALFORMED;
  if (declared != remaining)
    return HTTTP_ERR_LENGTH;
  /* Zero-copy: body points INTO the caller's buffer. No malloc, no free,
   * no copy of a payload up to 64 KiB. Cost: buf must outlive the struct. */
  *body = remaining ? buf + off : NULL;
  *body_len = declared;
  return HTTTP_OK;
}

/* Parse one complete request from buf (exactly one frame from session_recv —
 * framing guarantees message boundaries, so no incremental parsing needed).
 * Zero-copy: req->body points into buf; buf must outlive req. */
int htttp_parse_request(const uint8_t *buf, uint32_t len,
                        htttp_request_t *req) {
  if (!buf || !req)
    return HTTTP_ERR_MALFORMED;
  /* Zero first, so a rejected message leaves an unambiguously empty struct
   * rather than stale fields from a previous parse. Must precede every
   * write below — zeroing afterwards would erase the parse. */
  memset(req, 0, sizeof(*req));
  size_t eol;
  if (find_crlf(buf, len, 0, &eol) != 0)
    return HTTTP_ERR_MALFORMED;

  /* request-line = METHOD SP PATH SP VERSION, split on the first two
   * spaces:
   *     MOVE /game/move HTTTP/1.0
   *         ^          ^
   *        sp1        sp2
   */
  size_t sp1 = 0;
  while (sp1 < eol && buf[sp1] != ' ')
    sp1++;
  if (sp1 == 0 || sp1 >= eol) /* empty method, or no space at all */
    return HTTTP_ERR_MALFORMED;
  if (copy_token(req->method, HTTTP_MAX_METHOD, buf, sp1) != 0)
    return HTTTP_ERR_TOOLONG;

  /* Starts one past sp1 so it cannot re-find the same space. The path
   * therefore cannot contain a space by construction. */
  size_t sp2 = sp1 + 1;
  while (sp2 < eol && buf[sp2] != ' ')
    sp2++;
  if (sp2 == sp1 + 1 || sp2 >= eol) /* empty path, or no version field */
    return HTTTP_ERR_MALFORMED;
  if (copy_token(req->path, HTTTP_MAX_PATH, buf + sp1 + 1, sp2 - sp1 - 1) != 0)
    return HTTTP_ERR_TOOLONG;

  /* Length first, then content — memcmp on a short line would read past
   * eol. This also covers "too many spaces": a third space lands inside the
   * version field, where the exact compare rejects it. */
  if (eol - sp2 - 1 != VERSION_LEN ||
      memcmp(buf + sp2 + 1, HTTTP_VERSION, VERSION_LEN) != 0)
    return HTTTP_ERR_MALFORMED;

  /* eol + 2 skips the CRLF, landing on the first header byte. */
  int status = parse_tail(buf, len, eol + 2, req->headers, &req->n_headers,
                          &req->body, &req->body_len);
  return status;
}

/* Mirror: parse one complete response. */
int htttp_parse_response(const uint8_t *buf, uint32_t len,
                         htttp_response_t *res) {
  if (!buf || !res)
    return HTTTP_ERR_MALFORMED;
  memset(res, 0, sizeof(*res));
  size_t eol;
  if (find_crlf(buf, len, 0, &eol) != 0)
    return HTTTP_ERR_MALFORMED;

  /* status-line = VERSION SP STATUS SP REASON
   *     HTTTP/1.0 200 OK
   *              ^   ^
   *   VERSION_LEN   sp1
   *
   * Unlike a request line, the version sits at a fixed offset, so only the
   * second space needs searching — one scan, not two. */
  if (eol <= VERSION_LEN || memcmp(buf, HTTTP_VERSION, VERSION_LEN) != 0 ||
      buf[VERSION_LEN] != ' ')
    return HTTTP_ERR_MALFORMED;

  size_t sp1 = VERSION_LEN + 1;
  while (sp1 < eol && buf[sp1] != ' ')
    sp1++;
  if (sp1 == VERSION_LEN + 1 || sp1 >= eol) /* empty status, or no reason */
    return HTTTP_ERR_MALFORMED;

  /* Parsed in place — parse_uint reads straight out of the wire buffer, so
   * no copy_token here. Only string fields get copied; numbers do not.
   * Parsed as uint32_t and range-checked before the cast to int. */
  size_t status_start = VERSION_LEN + 1;
  uint32_t status;
  if (parse_uint(buf + status_start, sp1 - status_start, &status) != 0)
    return HTTTP_ERR_MALFORMED;
  if (status < 100 || status > 599)
    return HTTTP_ERR_MALFORMED;
  res->status = (int)status;

  /* Reason = [sp1 + 1, eol). Deliberately not stored: it may contain spaces
   * ("Not Found"), and htttp_reason() is the single source of truth for
   * phrases. Requiring the space to exist is the whole validation. */

  int parsed = parse_tail(buf, len, eol + 2, res->headers, &res->n_headers,
                          &res->body, &res->body_len);
  return parsed;
}

/* --- Serializing ---------------------------------------------------------- */

/* All bytes must be visible ASCII (0x21..0x7e), plus ' ' when allow_space.
 *
 * The security check in the serializers: a space or CRLF in a method, path, or
 * key forges an extra request line or header. Rejected, not escaped — this
 * grammar has no escaping layer. */
static int valid_field(const char *s, int allow_space) {
  if (!s)
    return HTTTP_ERR_MALFORMED;
  for (size_t index = 0; s[index] != '\0'; index++) {
    /* unsigned char as in ci_eq: plain char may be signed, so a byte
     * >= 0x80 would go negative. */
    unsigned char c = (unsigned char)s[index];
    if (c >= '!' && c <= '~')
      continue;
    if (allow_space && c == ' ')
      continue;
    return HTTTP_ERR_MALFORMED;
  }
  return HTTTP_OK;
}

/* Append n bytes at *off, advancing *off. NOSPACE if they do not fit, with
 * nothing written — truncation is never silent. */
static int buf_append(uint8_t *out, size_t cap, size_t *off, const void *src,
                      size_t n) {
  /* A zero-length body arrives with src == NULL, and memcpy(dst, NULL, 0) is
   * UB even though it works everywhere. */
  if (n == 0)
    return HTTTP_OK;
  /* Never `*off + n > cap`: that can overflow and then pass. This form
   * cannot, because *off <= cap is invariant. */
  if (n > cap - *off)
    return HTTTP_ERR_NOSPACE;
  memcpy(out + *off, src, n);
  *off += n;
  return HTTTP_OK;
}

/* Append one "KEY: VALUE CRLF" line. */
static int emit_header(uint8_t *out, size_t cap, size_t *off, const char *key,
                       const char *value) {
  int rc = buf_append(out, cap, off, key, strlen(key));
  if (rc != HTTTP_OK)
    return rc;
  rc = buf_append(out, cap, off, ": ", 2);
  if (rc != HTTTP_OK)
    return rc;
  rc = buf_append(out, cap, off, value, strlen(value));
  if (rc != HTTTP_OK)
    return rc;
  return buf_append(out, cap, off, "\r\n", 2);
}

/* Emit the caller's headers, skipping what this library regenerates (D5): a
 * parsed message still carries its Content-Length, so re-emitting it verbatim
 * would duplicate the field on round-trip. skip_date is for responses only.
 *
 * Shared by both serializers so a fix lands on both — as with parse_tail.
 *
 * A bad key or value is rejected, not skipped: dropping it silently would
 * leave the caller believing the header went out. */
static int emit_headers(uint8_t *out, size_t cap, size_t *off,
                        const htttp_header_t *headers, size_t n,
                        int skip_date) {
  /* Nothing forces the caller's n to match the fixed array behind it.
   * Without this, n = 100 reads past the end. */
  if (n > HTTTP_MAX_HEADERS)
    return HTTTP_ERR_TOOLONG;
  for (size_t i = 0; i < n; i++) {
    if (ci_eq(headers[i].key, "Content-Length"))
      continue;
    if (skip_date && ci_eq(headers[i].key, "Date"))
      continue;
    /* valid_field passes an empty string — the loop body never runs — so
     * the empty key has to be caught separately, or it emits ": v\r\n". */
    if (headers[i].key[0] == '\0')
      return HTTTP_ERR_MALFORMED;
    /* Keys take no space, values do — a space in a key splits it into two
     * tokens on the wire. */
    if (valid_field(headers[i].key, 0) != HTTTP_OK ||
        valid_field(headers[i].value, 1) != HTTTP_OK)
      return HTTTP_ERR_MALFORMED;
    int rc = emit_header(out, cap, off, headers[i].key, headers[i].value);
    if (rc != HTTTP_OK)
      return rc;
  }
  return HTTTP_OK;
}

/* Generated Content-Length (only when there is a body), the blank line ending
 * the header block, then the body. Shared by both serializers. */
/* How many header lines this message will actually put on the wire, counting
 * the ones the serializer generates and discounting the ones emit_headers
 * drops.
 *
 * Exists because serialize and parse disagreed about the ceiling: a message
 * carrying exactly HTTTP_MAX_HEADERS headers serialized happily, and the
 * generated Date (responses) or Content-Length (any message with a body) made
 * it HTTTP_MAX_HEADERS + 1 on the wire - which the peer's parser then rejects
 * with TOOLONG. The sender sees success, the receiver sees a malformed frame,
 * and nothing in between explains why. Emitting a frame this library cannot
 * itself read is the bug; refusing to emit it is the fix.
 * Found by tests/fuzz/fuzz_htttp_response. */
static size_t emitted_header_count(const htttp_header_t *headers, size_t n,
                                   int skip_date, int generates_date,
                                   int has_body) {
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    /* Mirrors emit_headers' skips exactly - if one grows a case, so must
     * the other, which is why they sit next to each other. */
    if (ci_eq(headers[i].key, "Content-Length"))
      continue;
    if (skip_date && ci_eq(headers[i].key, "Date"))
      continue;
    count++;
  }
  if (generates_date)
    count++;
  if (has_body)
    count++;
  return count;
}

static int emit_body(uint8_t *out, size_t cap, size_t *off, const uint8_t *body,
                     uint32_t body_len) {
  int rc;
  if (body_len > 0) {
    /* body_len must become ASCII digits to go on the wire. 16 bytes holds
     * any uint32_t (10 digits), so the truncation branch is unreachable —
     * kept as a hard stop rather than an assumption. */
    char cl[16];
    int m = snprintf(cl, sizeof cl, "%u", body_len);
    if (m < 0 || (size_t)m >= sizeof cl)
      return HTTTP_ERR_MALFORMED;
    rc = emit_header(out, cap, off, "Content-Length", cl);
    if (rc != HTTTP_OK)
      return rc;
  }
  rc = buf_append(out, cap, off, "\r\n", 2);
  if (rc != HTTTP_OK)
    return rc;
  return buf_append(out, cap, off, body, body_len);
}

/* Write request as wire bytes into out. On entry *out_len = capacity;
 * on success *out_len = bytes written. Adds Content-Length automatically
 * when body_len > 0. */
int htttp_serialize_request(const htttp_request_t *req, uint8_t *out,
                            uint32_t *out_len) {
  if (!req || !out || !out_len)
    return HTTTP_ERR_MALFORMED;
  /* A length with no buffer behind it would be read as body bytes. */
  if (req->body == NULL && req->body_len > 0)
    return HTTTP_ERR_MALFORMED;
  /* Empty method or path emits two adjacent spaces, which reparses as a
   * different message. */
  if (req->method[0] == '\0' || req->path[0] == '\0')
    return HTTTP_ERR_MALFORMED;
  /* No space in either: one would forge an extra request-line field. */
  if (valid_field(req->method, 0) != HTTTP_OK ||
      valid_field(req->path, 0) != HTTTP_OK)
    return HTTTP_ERR_MALFORMED;

  /* Refused before a byte is written, so the caller's buffer is untouched:
   * see emitted_header_count for why a message that fits the parse bounds
   * can still exceed them once Content-Length is added. */
  if (emitted_header_count(req->headers, req->n_headers, 0, 0,
                           req->body_len > 0) > HTTTP_MAX_HEADERS)
    return HTTTP_ERR_TOOLONG;

  /* *out_len is capacity in, bytes written out. Read it before anything
   * writes to it — the caller's buffer is this big and no bigger, whatever
   * HTTTP_MAX_FRAME says. */
  size_t cap = *out_len;
  size_t off = 0;
  int rc;

  /* METHOD SP PATH SP VERSION CRLF. Chained on || so it short-circuits: the
   * first failure leaves its code in rc. Keeps "every append is checked"
   * visible in one place instead of across eighteen if statements. */
  if ((rc = buf_append(out, cap, &off, req->method, strlen(req->method))) ||
      (rc = buf_append(out, cap, &off, " ", 1)) ||
      (rc = buf_append(out, cap, &off, req->path, strlen(req->path))) ||
      (rc = buf_append(out, cap, &off, " ", 1)) ||
      (rc = buf_append(out, cap, &off, HTTTP_VERSION, VERSION_LEN)) ||
      (rc = buf_append(out, cap, &off, "\r\n", 2)))
    return rc;

  /* skip_date = 0: requests carry no generated Date. */
  if ((rc = emit_headers(out, cap, &off, req->headers, req->n_headers, 0)) !=
      HTTTP_OK)
    return rc;
  if ((rc = emit_body(out, cap, &off, req->body, req->body_len)) != HTTTP_OK)
    return rc;

  /* off, not strlen(out): out is raw bytes with no terminator, and a body may
   * legitimately contain NULs. */
  *out_len = (uint32_t)off;
  return HTTTP_OK;
}

/* Response mirror. Adds Content-Length and Date (RFC 1123) automatically. */
int htttp_serialize_response(const htttp_response_t *res, uint8_t *out,
                             uint32_t *out_len) {
  if (!res || !out || !out_len)
    return HTTTP_ERR_MALFORMED;
  if (res->body == NULL && res->body_len > 0)
    return HTTTP_ERR_MALFORMED;

  /* Unknown status has no canonical phrase, so nothing valid can go on the
   * status line. htttp_reason returning NULL is the guard — this is why it
   * must not return a placeholder. Checked before any bytes are written, so a
   * rejected response leaves out untouched. */
  const char *reason = htttp_reason(res->status);
  if (reason == NULL)
    return HTTTP_ERR_MALFORMED;

  /* Status becomes ASCII digits here. No valid_field needed: %d of a value
   * htttp_reason accepted emits digits only. */
  char st[8];
  int m = snprintf(st, sizeof st, "%d", res->status);
  if (m < 0 || (size_t)m >= sizeof st)
    return HTTTP_ERR_MALFORMED;

  /* Built before the writes so a strftime failure cannot abort halfway
   * through a partly written buffer. gmtime_r, not gmtime: tetrisd is
   * threaded and gmtime returns a shared static struct. */
  char date[48];
  time_t now = time(NULL);
  struct tm tm_gmt;
  if (gmtime_r(&now, &tm_gmt) == NULL)
    return HTTTP_ERR_MALFORMED;
  if (strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt) == 0)
    return HTTTP_ERR_MALFORMED;

  /* skip_date = 1 and generates_date = 1: the caller's own Date is dropped
   * and ours replaces it, so the two cancel - but every other header still
   * counts, and so does the Content-Length emit_body will add. */
  if (emitted_header_count(res->headers, res->n_headers, 1, 1,
                           res->body_len > 0) > HTTTP_MAX_HEADERS)
    return HTTTP_ERR_TOOLONG;

  size_t cap = *out_len;
  size_t off = 0;
  int rc;

  /* VERSION SP STATUS SP REASON CRLF. Reason comes from htttp_reason, not the
   * wire, so no validation — and it may contain spaces ("Not Found"). */
  if ((rc = buf_append(out, cap, &off, HTTTP_VERSION, VERSION_LEN)) ||
      (rc = buf_append(out, cap, &off, " ", 1)) ||
      (rc = buf_append(out, cap, &off, st, (size_t)m)) ||
      (rc = buf_append(out, cap, &off, " ", 1)) ||
      (rc = buf_append(out, cap, &off, reason, strlen(reason))) ||
      (rc = buf_append(out, cap, &off, "\r\n", 2)))
    return rc;

  if ((rc = emit_header(out, cap, &off, "Date", date)) != HTTTP_OK)
    return rc;
  /* skip_date = 1: the Date just emitted is authoritative, so a caller's own
   * — which a parsed response still carries — is dropped. */
  if ((rc = emit_headers(out, cap, &off, res->headers, res->n_headers, 1)) !=
      HTTTP_OK)
    return rc;
  if ((rc = emit_body(out, cap, &off, res->body, res->body_len)) != HTTTP_OK)
    return rc;

  *out_len = (uint32_t)off;
  return HTTTP_OK;
}

/* --- Header helpers ------------------------------------------------------- */

/* Case-insensitive lookup ("Player-Id" == "player-id"). NULL if absent. */
const char *htttp_header_get(const htttp_header_t *headers, size_t n,
                             const char *key) {
  if (!headers || !key)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    if (ci_eq(headers[i].key, key))
      return headers[i].value;
  }
  return NULL;
}

/* Append key/value; HTTTP_ERR_TOOLONG if full or oversized. */
int htttp_header_set(htttp_header_t *headers, size_t *n, const char *key,
                     const char *value) {
  if (!headers || !n || !key || !value)
    return HTTTP_ERR_MALFORMED;
  /* An empty key serializes to ": value\r\n" — malformed wire. */
  if (key[0] == '\0')
    return HTTTP_ERR_MALFORMED;
  /* Validated here as well as at serialize time: a key holding "\r\n" would
   * otherwise sit in the struct until then. Failing at the setter names the
   * caller that introduced it. */
  if (valid_field(key, 0) != HTTTP_OK || valid_field(value, 1) != HTTTP_OK)
    return HTTTP_ERR_MALFORMED;
  if (*n >= HTTTP_MAX_HEADERS)
    return HTTTP_ERR_TOOLONG;
  if (copy_token(headers[*n].key, HTTTP_MAX_KEY, (const uint8_t *)key,
                 strlen(key)) != 0)
    return HTTTP_ERR_TOOLONG;
  if (copy_token(headers[*n].value, HTTTP_MAX_VALUE, (const uint8_t *)value,
                 strlen(value)) != 0)
    return HTTTP_ERR_TOOLONG;
  (*n)++;
  return HTTTP_OK;
}

/* --- Misc ----------------------------------------------------------------- */

/* Canonical reason phrase: 200->"OK", 404->"Not Found",
 * 413->"Payload Too Large", ... NULL for unknown status. */
const char *htttp_reason(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 409:
    return "Conflict";
  case 413:
    return "Payload Too Large";
  case 429:
    return "Too Many Requests";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  default:
    return NULL;
  }
}
