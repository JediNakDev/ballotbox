/*
 * Unit tests for the election lifecycle transition table.
 * Covers TEST.md U-06 (legal chain) and U-07 (illegal pairs).
 *
 * Legality is pure (bb_is_legal_transition), so it is fully testable now. The
 * U-06 "final state PUBLISHED" postcondition is dropped: confirming the stored
 * state needs DB readback, which the stubbed seam does not provide. We instead
 * assert each legal edge is accepted both as a pure predicate and by
 * bb_transition_state (which returns BB_OK for a legal edge; its write goes
 * through the logged-only DB stub).
 */

#include "libballotbrain/ballotbrain.h"
#include "unity.h"

static bb_ctx *ctx;

void setUp(void) {
  ctx = bb_create();
  bb_set_log(ctx, NULL); /* silence the seam/operation log */
}

void tearDown(void) {
  bb_destroy(ctx);
}

/* U-06: DRAFT->OPEN->CLOSED->PUBLISHED - each step is a legal transition. */
void test_U06_legal_transition_chain(void) {
  TEST_ASSERT_TRUE(bb_is_legal_transition(BB_STATE_DRAFT, BB_STATE_OPEN));
  TEST_ASSERT_TRUE(bb_is_legal_transition(BB_STATE_OPEN, BB_STATE_CLOSED));
  TEST_ASSERT_TRUE(bb_is_legal_transition(BB_STATE_CLOSED, BB_STATE_PUBLISHED));

  /* bb_transition_state accepts each legal edge (state readback deferred). */
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_transition_state(ctx, "E-100", BB_STATE_DRAFT, BB_STATE_OPEN));
  TEST_ASSERT_EQUAL_INT(BB_OK, bb_transition_state(ctx, "E-100", BB_STATE_OPEN, BB_STATE_CLOSED));
  TEST_ASSERT_EQUAL_INT(BB_OK,
                        bb_transition_state(ctx, "E-100", BB_STATE_CLOSED, BB_STATE_PUBLISHED));
}

/* U-07: one representative per illegal pair is rejected, both as a pure
 * predicate (0) and via bb_transition_state (BB_ERR_ILLEGAL_TRANSITION). */
void test_U07_illegal_transitions_rejected(void) {
  const bb_state_t from[] = {BB_STATE_PUBLISHED, BB_STATE_DRAFT, BB_STATE_OPEN, BB_STATE_CLOSED};
  const bb_state_t to[]   = {BB_STATE_OPEN,      BB_STATE_CLOSED, BB_STATE_DRAFT, BB_STATE_OPEN};

  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_FALSE(bb_is_legal_transition(from[i], to[i]));
    TEST_ASSERT_EQUAL_INT(BB_ERR_ILLEGAL_TRANSITION,
                          bb_transition_state(ctx, "E-100", from[i], to[i]));
  }
}

/* Guard: PUBLISHED is terminal - no successor is legal. */
void test_published_is_terminal(void) {
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_DRAFT));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_OPEN));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_CLOSED));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_PUBLISHED, BB_STATE_PUBLISHED));
}

/* Guard: a state is never a legal successor of itself (no self-loops). */
void test_no_self_transitions(void) {
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_DRAFT, BB_STATE_DRAFT));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_OPEN, BB_STATE_OPEN));
  TEST_ASSERT_FALSE(bb_is_legal_transition(BB_STATE_CLOSED, BB_STATE_CLOSED));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_U06_legal_transition_chain);
  RUN_TEST(test_U07_illegal_transitions_rejected);
  RUN_TEST(test_published_is_terminal);
  RUN_TEST(test_no_self_transitions);
  return UNITY_END();
}
