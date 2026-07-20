/*
 * Unit tests for libballotclient admin logic (ballotctl).
 * Not in the TEST.md U-* numbering, but cheap and seam-free:
 *   - bc_build_transition accepts OPEN/CLOSE/PUBLISH and rejects a non-lifecycle
 *     op (e.g. BCL_JOIN) with BB_ERR_ILLEGAL_TRANSITION.
 *   - bc_prevalidate_config / bc_build_create return the SAME specific
 *     BB_ERR_CONFIG_* as the authoritative bb_validate_config, proving the
 *     client pre-check does not drift from the daemon's rules.
 */

#include "libballotclient/admin.h"
#include "libballotbrain/ballotbrain.h"
#include "unity.h"

#include <string.h>

static bb_config_t valid_config(void) {
  bb_config_t c;
  memset(&c, 0, sizeof(c));
  snprintf(c.title, BB_TITLE_LEN, "Officers 2026");
  snprintf(c.options[0], BB_OPTION_LEN, "Alice");
  snprintf(c.options[1], BB_OPTION_LEN, "Bob");
  c.option_count = 2;
  snprintf(c.open_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-01T01:00:00Z");
  return c;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- bc_build_transition ---------------------------------------------- */

void test_build_transition_accepts_lifecycle_ops(void) {
  const bcl_op_t ops[] = {BCL_OPEN, BCL_CLOSE, BCL_PUBLISH};
  for (int i = 0; i < 3; i++) {
    bcl_request_t req;
    memset(&req, 0xAA, sizeof(req)); /* poison to prove the builder writes it */
    TEST_ASSERT_EQUAL_INT(BB_OK, bc_build_transition(ops[i], "E-100", &req));
    TEST_ASSERT_EQUAL_INT(ops[i], req.op);
    TEST_ASSERT_EQUAL_STRING("E-100", req.election_id);
  }
}

void test_build_transition_rejects_non_lifecycle_op(void) {
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  /* JOIN/CAST/CREATE etc. are not lifecycle transitions. */
  TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION, bc_build_transition(BCL_JOIN, "E-100", &req));
  TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION, bc_build_transition(BCL_CREATE, "E-100", &req));
}

/* ---- no rule drift between client pre-check and daemon validator ------- */

void test_prevalidate_matches_brain_validator(void) {
  /* Valid */
  bb_config_t c = valid_config();
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_OK, bc_prevalidate_config(&c));

  /* Empty title */
  c = valid_config();
  c.title[0] = '\0';
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TITLE, bc_prevalidate_config(&c));

  /* Too few options */
  c = valid_config();
  c.option_count = 1;
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bc_prevalidate_config(&c));

  /* Non-positive time window (close <= open, lexicographic) */
  c = valid_config();
  snprintf(c.close_time, BB_TIME_LEN, "2026-01-01T00:00:00Z");
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_prevalidate_config(&c));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_TIME, bc_prevalidate_config(&c));
}

/* ---- bc_build_create --------------------------------------------------- */

void test_build_create_valid_config(void) {
  bb_config_t c = valid_config();
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  TEST_ASSERT_EQUAL_INT(BB_OK, bc_build_create(&c, &req));
  TEST_ASSERT_EQUAL_INT(BCL_CREATE, req.op);
  TEST_ASSERT_EQUAL_INT(2, req.config.option_count);
  TEST_ASSERT_EQUAL_STRING("Officers 2026", req.config.title);
}

void test_build_create_rejects_invalid_with_same_error(void) {
  bb_config_t c = valid_config();
  c.option_count = 0;
  bcl_request_t req;
  memset(&req, 0, sizeof(req));
  /* Same specific error as the authoritative validator; no request built. */
  TEST_ASSERT_EQUAL_INT(bb_validate_config(&c), bc_build_create(&c, &req));
  TEST_ASSERT_EQUAL_INT(BB_ERR_CONFIG_OPTIONS, bc_build_create(&c, &req));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_build_transition_accepts_lifecycle_ops);
  RUN_TEST(test_build_transition_rejects_non_lifecycle_op);
  RUN_TEST(test_prevalidate_matches_brain_validator);
  RUN_TEST(test_build_create_valid_config);
  RUN_TEST(test_build_create_rejects_invalid_with_same_error);
  return UNITY_END();
}
