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
  // a forward scrub is not pulled back; it stays where it started.
  start(+1, 9800, 10000);
  TEST_ASSERT_EQUAL_UINT32(9800, s.target_ms);
  repeat(150, 9800);
  TEST_ASSERT_EQUAL_UINT32(9800, scrub_finish(&s));
  start(+1, 20000, 10000);
  TEST_ASSERT_EQUAL_UINT32(20000, s.target_ms);
  repeat(SCRUB_FAST_AFTER_MS, 20000);
  // Rewinding from past the limit goes back from the limit.
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
    start(+1, 0, LENGTHS[i]);
    TEST_ASSERT_EQUAL_UINT32(0, s.target_ms);
    repeat(150, 0);
    repeat(SCRUB_FAST_AFTER_MS, 0);
    TEST_ASSERT_EQUAL_UINT32(0, scrub_finish(&s));
    // Forward from inside it: it stays where it was.
    start(+1, 50, LENGTHS[i]);
    repeat(150, 50);
    TEST_ASSERT_EQUAL_UINT32(50, scrub_finish(&s));
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

// Whatever the start and the length, every repeat of a forward scrub leaves
// the target at or above where playback was and never lower than before.
void test_forward_never_moves_the_target_back(void) {
  static const uint32_t LENGTHS[] = {0, 1, 500, 1000, 1500, 10000, 300000};
  static const uint32_t STARTS[] = {0,    50,   499,   999,   8500,   9000,
                                    9001, 9999, 10000, 20000, 299500, 400000};
  for (size_t l = 0; l < sizeof(LENGTHS) / sizeof(LENGTHS[0]); l++) {
    for (size_t e = 0; e < sizeof(STARTS) / sizeof(STARTS[0]); e++) {
      start(+1, STARTS[e], LENGTHS[l]);
      TEST_ASSERT_EQUAL_UINT32(STARTS[e], s.target_ms);
      uint32_t before = s.target_ms;
      for (int64_t t = 150; t < 3000; t += 150) {
        scrub_repeat(&s, L + t);
        TEST_ASSERT_TRUE(s.target_ms >= before);
        TEST_ASSERT_TRUE(s.target_ms >= STARTS[e]);
        // It moves only up to the limit, never past it.
        if (LENGTHS[l] != 0 && s.target_ms > before) {
          TEST_ASSERT_TRUE(s.target_ms + SCRUB_END_MARGIN_MS <= LENGTHS[l]);
        }
        before = s.target_ms;
      }
      TEST_ASSERT_TRUE(scrub_finish(&s) >= STARTS[e]);
    }
  }
}

// The same for a rewind: never above where playback was, never higher than
// before.
void test_rewind_never_moves_the_target_forward(void) {
  static const uint32_t LENGTHS[] = {0, 1, 500, 1000, 1500, 10000, 300000};
  static const uint32_t STARTS[] = {0,    50,   499,   999,   8500,   9000,
                                    9001, 9999, 10000, 20000, 299500, 400000};
  for (size_t l = 0; l < sizeof(LENGTHS) / sizeof(LENGTHS[0]); l++) {
    for (size_t e = 0; e < sizeof(STARTS) / sizeof(STARTS[0]); e++) {
      start(-1, STARTS[e], LENGTHS[l]);
      TEST_ASSERT_TRUE(s.target_ms <= STARTS[e]);
      uint32_t before = s.target_ms;
      for (int64_t t = 150; t < 3000; t += 150) {
        scrub_repeat(&s, L + t);
        TEST_ASSERT_TRUE(s.target_ms <= before);
        before = s.target_ms;
      }
      TEST_ASSERT_TRUE(scrub_finish(&s) <= STARTS[e]);
    }
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_forward_two_seconds_per_repeat_then_five);
  RUN_TEST(test_back_stops_at_zero);
  RUN_TEST(test_forward_stops_before_the_end);
  RUN_TEST(test_track_shorter_than_the_margin);
  RUN_TEST(test_unknown_length_has_no_end_limit);
  RUN_TEST(test_finish_returns_the_target_and_ends_the_scrub);
  RUN_TEST(test_forward_never_moves_the_target_back);
  RUN_TEST(test_rewind_never_moves_the_target_forward);
  return UNITY_END();
}
