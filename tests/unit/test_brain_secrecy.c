/*
 * U-21: no ballot-to-voter link in logs (secrecy invariant R2).
 *
 * bb_record_ballot drives the crypto and DB seams, which log SQL-ish operation
 * lines. The APPEND_BALLOT rendering deliberately carries the hash and option
 * only - never the submitting cert - so no log line can pair a voter identity
 * with a ballot or hash. This test works against the stub today and stays valid
 * once SimpleDB backs the seam.
 *
 * We point the context log at an in-memory buffer (open_memstream), record a
 * ballot with a distinctive cert name, then assert:
 *   - the cert name never appears anywhere in the log (strictly stronger than
 *     "no single line contains both cert and hash"), and
 *   - the receipt hash *does* appear (proving the ballot activity was logged, so
 *     the absence of the cert is meaningful and not just an empty buffer).
 */

#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#define SECRET_CERT "ZZ-secret-cert-alice-9f3a"

static bb_ctx *ctx;
static char *log_buf;
static size_t log_size;
static FILE *log_stream;

void setUp(void) {
  ctx = bb_create();
  log_buf = NULL;
  log_size = 0;
  log_stream = open_memstream(&log_buf, &log_size);
  TEST_ASSERT_NOT_NULL(log_stream);
  bb_set_log(ctx, log_stream);
}

void tearDown(void) {
  bb_set_log(ctx, NULL);
  if (log_stream) {
    fclose(log_stream);
  }
  free(log_buf);
  bb_destroy(ctx);
}

void test_U21_no_cert_hash_pairing_in_logs(void) {
  bb_ballot_t ballot;
  memset(&ballot, 0, sizeof(ballot));
  snprintf(ballot.cert_name, BB_CERT_LEN, SECRET_CERT);
  snprintf(ballot.nonce, BB_NONCE_LEN, "nonce-001");
  ballot.payload[0] = 1; /* option index carried in payload[0] (placeholder) */
  ballot.payload_len = 1;

  bb_receipt_t receipt;
  memset(&receipt, 0, sizeof(receipt));
  bb_result_t r = bb_record_ballot(ctx, "E-100", &ballot, &receipt);
  TEST_ASSERT_EQUAL_INT(BB_OK, r);

  fflush(log_stream);

  /* The hash was logged (the append line): absence of the cert is meaningful. */
  TEST_ASSERT_GREATER_THAN(0, (int)strlen(receipt.hash));
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_buf, receipt.hash),
                               "expected the receipt hash to appear in the operation log");

  /* The submitting cert must never appear in any log line. */
  TEST_ASSERT_NULL_MESSAGE(strstr(log_buf, SECRET_CERT),
                           "secrecy violation: submitting cert found in the operation log");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U21_no_cert_hash_pairing_in_logs);
  return UNITY_END();
}
