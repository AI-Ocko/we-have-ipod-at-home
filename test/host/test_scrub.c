#include "scrub.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static scrub_t s;

#define L ((int64_t)5000000) // when the long press came

static void start(int direction, uint32_t elapsed_ms, uint32_t duration_ms) {
  memset(&s, 0xA5, sizeof(s)); // scrub_start must set everything
  scrub_start(&s, direction, elapsed_ms, duration_ms, L);
  TEST_ASSERT_TRUE(s.active);
  TEST_ASSERT_EQUAL_INT(direction, s.direction);
  TEST_ASSERT_EQUAL_UINT32(duration_ms, s.duration_ms);
  TEST_ASSERT_EQUAL_INT64(L, s.since_ms);
}

static void repeat(int64_t after_long_ms, uint32_t target_ms) {
  scrub_repeat(&s, L + after_long_ms);
  TEST_ASSERT_EQUAL_UINT32(target_ms, s.target_ms);
}

void test_forward_two_seconds_per_repeat_then_five(void) {
  start(+1, 10000, 300000);
  TEST_ASSERT_EQUAL_UINT32(10000, s.target_ms);
  // Repeats every 150 ms from the long press: 13 of them within 2 s.
  uint32_t target = 10000;
  int64_t t = 150;
  for (; t < SCRUB_FAST_AFTER_MS; t += 150) {
    target += SCRUB_STEP_MS;
    repeat(t, target);
  }
  // From 2 s after the long press (the keypad then repeats every 50 ms).
  target += SCRUB_FAST_STEP_MS;
  repeat(SCRUB_FAST_AFTER_MS, target);
  for (t = SCRUB_FAST_AFTER_MS + 50; t < 4000; t += 50) {
    target += SCRUB_FAST_STEP_MS;
    repeat(t, target);
  }
  TEST_ASSERT_TRUE(s.active);

  // The edge exactly: 1999 ms is still the slow step.
  start(+1, 0, 0);
  repeat(SCRUB_FAST_AFTER_MS - 1, 2000);
  repeat(SCRUB_FAST_AFTER_MS, 7000);

  // Back uses the same steps.
  start(-1, 100000, 300000);
  repeat(150, 98000);
  repeat(SCRUB_FAST_AFTER_MS, 93000);
}

void test_back_stops_at_zero(void) {
  start(-1, 3000, 300000);
  repeat(150, 1000);
  repeat(300, 0);
  repeat(450, 0);
  repeat(SCRUB_FAST_AFTER_MS, 0);
  // A fast step from less than 5 s.
  start(-1, 4000, 300000);
  repeat(SCRUB_FAST_AFTER_MS, 0);
  // From 0:00.
  start(-1, 0, 0);
  repeat(150, 0);
  TEST_ASSERT_EQUAL_UINT32(0, scrub_finish(&s));
}

void test_forward_stops_before_the_end(void) {
  start(+1, 5000, 10000);
  repeat(150, 7000);
  repeat(300, 9000);
  repeat(450, 9000);
  repeat(SCRUB_FAST_AFTER_MS, 9000);
  TEST_ASSERT_EQUAL_UINT32(9000, scrub_finish(&s));
  // Started inside the last second (or past a length that was a guess):
  // the target is one second before the end.
  start(+1, 9800, 10000);
  TEST_ASSERT_EQUAL_UINT32(9000, s.target_ms);
  start(+1, 20000, 10000);
  TEST_ASSERT_EQUAL_UINT32(9000, s.target_ms);
  // And rewinding from there goes back from that point.
  start(-1, 20000, 10000);
  repeat(150, 7000);
  // The longest length there is.
  start(+1, UINT32_MAX - 2000, UINT32_MAX);
  repeat(150, UINT32_MAX - SCRUB_END_MARGIN_MS);
}

void test_track_shorter_than_the_margin(void) {
  static const uint32_t LENGTHS[] = {1, 100, SCRUB_END_MARGIN_MS - 1,
                                     SCRUB_END_MARGIN_MS};
  for (size_t i = 0; i < sizeof(LENGTHS) / sizeof(LENGTHS[0]); i++) {
    start(+1, 50, LENGTHS[i]);
    TEST_ASSERT_EQUAL_UINT32(0, s.target_ms);
    repeat(150, 0);
    repeat(SCRUB_FAST_AFTER_MS, 0);
    TEST_ASSERT_EQUAL_UINT32(0, scrub_finish(&s));
    start(-1, 50, LENGTHS[i]);
    repeat(150, 0);
  }
  // Just over the margin: the limit is the difference.
  start(+1, 0, 1500);
  repeat(150, 500);
}

void test_unknown_length_has_no_end_limit(void) {
  start(+1, 300000, 0);
  repeat(150, 302000);
  repeat(SCRUB_FAST_AFTER_MS, 307000);
  // As far as the type goes, and no further.
  start(+1, UINT32_MAX - 3000, 0);
  repeat(150, UINT32_MAX - 1000);
  repeat(300, UINT32_MAX);
  repeat(450, UINT32_MAX);
}

void test_finish_returns_the_target_and_ends_the_scrub(void) {
  start(+1, 60000, 300000);
  repeat(150, 62000);
  repeat(300, 64000);
  TEST_ASSERT_EQUAL_UINT32(64000, scrub_finish(&s));
  TEST_ASSERT_FALSE(s.active);
  // A repeat after the end changes nothing.
  scrub_repeat(&s, L + 450);
  TEST_ASSERT_EQUAL_UINT32(64000, s.target_ms);
  TEST_ASSERT_FALSE(s.active);
  // A long press and its release with no repeat between: where it was.
  start(-1, 42000, 300000);
  TEST_ASSERT_EQUAL_UINT32(42000, scrub_finish(&s));
  TEST_ASSERT_FALSE(s.active);
  // A repeat with a clock before the long press is a slow step.
  start(+1, 0, 0);
  scrub_repeat(&s, INT64_MIN);
  TEST_ASSERT_EQUAL_UINT32(2000, s.target_ms);
  // A long press at the far end of the clock.
  scrub_start(&s, +1, 0, 0, INT64_MAX);
  scrub_repeat(&s, INT64_MAX);
  TEST_ASSERT_EQUAL_UINT32(2000, s.target_ms);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_forward_two_seconds_per_repeat_then_five);
  RUN_TEST(test_back_stops_at_zero);
  RUN_TEST(test_forward_stops_before_the_end);
  RUN_TEST(test_track_shorter_than_the_margin);
  RUN_TEST(test_unknown_length_has_no_end_limit);
  RUN_TEST(test_finish_returns_the_target_and_ends_the_scrub);
  return UNITY_END();
}
