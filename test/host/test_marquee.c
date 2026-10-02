#include "marquee.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define WIDTH 16
#define GUARD '#'

// 26 characters: the name of the music track on the test card.
static const char LONG[] = "16 - Kanye West - Hey Mama";
#define LONG_LEN (sizeof(LONG) - 1)
#define LONG_PERIOD (LONG_LEN + MARQUEE_GAP)

// When step number `n` is taken (n = 1 is the first). The first comes the
// moment the rest is over; the others follow one step time apart.
#define STEP_AT(n) (MARQUEE_REST_MS + ((n) - 1u) * MARQUEE_STEP_MS)
// When a text of `len` characters is back at its start.
#define CYCLE_MS(len) STEP_AT((len) + MARQUEE_GAP)

// The window is written into the middle of this, so that a write outside the
// `width` characters asked for is seen.
static char s_buf[64];

static const char *window(const char *text, size_t width, uint32_t elapsed) {
  memset(s_buf, GUARD, sizeof(s_buf));
  marquee_window(text, width, elapsed, s_buf + 8);
  for (size_t i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL_CHAR_MESSAGE(GUARD, s_buf[i], "wrote before the window");
  }
  for (size_t i = 8 + width; i < sizeof(s_buf); i++) {
    TEST_ASSERT_EQUAL_CHAR_MESSAGE(GUARD, s_buf[i], "wrote past the window");
  }
  return s_buf + 8;
}

// What the window must be at offset `k`: the text, the gap, the text again,
// read from character `k`. Worked out the slow, obvious way.
static const char *rotated(const char *text, size_t width, size_t k) {
  static char expected[64];
  size_t len = strlen(text);
  size_t period = len + MARQUEE_GAP;
  for (size_t col = 0; col < width; col++) {
    size_t at = (k + col) % period;
    expected[col] = at < len ? text[at] : ' ';
  }
  return expected;
}

static void test_short_text_is_padded_and_never_moves(void) {
  TEST_ASSERT_EQUAL_MEMORY("Kid A           ", window("Kid A", WIDTH, 0),
                           WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("Kid A           ", window("Kid A", WIDTH, 60000),
                           WIDTH);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms("Kid A", WIDTH, 0));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
                           marquee_next_change_ms("Kid A", WIDTH, 60000));
}

static void test_text_of_exactly_the_width_does_not_move(void) {
  const char *text = "t06_tone_30s_abc"; // 16 characters
  TEST_ASSERT_EQUAL_size_t(WIDTH, strlen(text));
  const uint32_t times[] = {0,     1,      MARQUEE_REST_MS - 1, MARQUEE_REST_MS,
                            STEP_AT(2), STEP_AT(7), 60000, UINT32_MAX};
  for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
    TEST_ASSERT_EQUAL_MEMORY(text, window(text, WIDTH, times[i]), WIDTH);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
                             marquee_next_change_ms(text, WIDTH, times[i]));
  }
}

static void test_one_character_too_long_moves(void) {
  const char *text = "0123456789abcdefg"; // 17 characters
  TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef", window(text, WIDTH, 0), WIDTH);
  // The rest is over and one step has been taken.
  TEST_ASSERT_EQUAL_MEMORY("123456789abcdefg", window(text, WIDTH, STEP_AT(1)),
                           WIDTH);
  TEST_ASSERT_NOT_EQUAL(UINT32_MAX, marquee_next_change_ms(text, WIDTH, 0));
}

static void test_it_rests_at_the_start(void) {
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ", window(LONG, WIDTH, 0), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ", window(LONG, WIDTH, 1), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ",
                           window(LONG, WIDTH, MARQUEE_REST_MS - 1), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("6 - Kanye West -",
                           window(LONG, WIDTH, MARQUEE_REST_MS), WIDTH);
}

static void test_each_step_moves_one_character(void) {
  static const char *const EXPECTED[] = {
      "6 - Kanye West -", " - Kanye West - ", "- Kanye West - H",
      " Kanye West - He", "Kanye West - Hey",
  };
  for (unsigned k = 1; k <= 5; k++) {
    // From the moment step k is taken until just before the next one.
    TEST_ASSERT_EQUAL_MEMORY(EXPECTED[k - 1], window(LONG, WIDTH, STEP_AT(k)),
                             WIDTH);
    TEST_ASSERT_EQUAL_MEMORY(EXPECTED[k - 1],
                             window(LONG, WIDTH, STEP_AT(k + 1) - 1), WIDTH);
    TEST_ASSERT_EQUAL_MEMORY(rotated(LONG, WIDTH, k),
                             window(LONG, WIDTH, STEP_AT(k)), WIDTH);
  }
}

static void test_the_gap_separates_end_and_start(void) {
  // After LONG_LEN - 1 steps the last character is at the left edge.
  const char *w = window(LONG, WIDTH, STEP_AT(LONG_LEN - 1));
  TEST_ASSERT_EQUAL_CHAR('a', w[0]);
  TEST_ASSERT_EQUAL_CHAR(' ', w[1]);
  TEST_ASSERT_EQUAL_CHAR(' ', w[2]);
  TEST_ASSERT_EQUAL_CHAR(' ', w[3]);
  TEST_ASSERT_EQUAL_CHAR('1', w[4]);
  TEST_ASSERT_EQUAL_MEMORY("a   16 - Kanye W", w, WIDTH);
}

static void test_it_comes_round_and_rests_again(void) {
  const uint32_t cycle = CYCLE_MS(LONG_LEN);
  // One step before it is home, the last gap space is still at the left.
  TEST_ASSERT_EQUAL_MEMORY(" 16 - Kanye West", window(LONG, WIDTH, cycle - 1),
                           WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ", window(LONG, WIDTH, cycle),
                           WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ",
                           window(LONG, WIDTH, cycle + MARQUEE_REST_MS - 1),
                           WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("6 - Kanye West -",
                           window(LONG, WIDTH, cycle + MARQUEE_REST_MS), WIDTH);
  // And the same again many cycles later.
  TEST_ASSERT_EQUAL_MEMORY("16 - Kanye West ",
                           window(LONG, WIDTH, 100 * cycle), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("- Kanye West - H",
                           window(LONG, WIDTH, 100 * cycle + STEP_AT(3)),
                           WIDTH);
}

static void test_next_change_during_rest_and_during_steps(void) {
  // During the rest: what is left of the rest.
  TEST_ASSERT_EQUAL_UINT32(MARQUEE_REST_MS,
                           marquee_next_change_ms(LONG, WIDTH, 0));
  TEST_ASSERT_EQUAL_UINT32(MARQUEE_REST_MS - 400,
                           marquee_next_change_ms(LONG, WIDTH, 400));
  TEST_ASSERT_EQUAL_UINT32(
      1, marquee_next_change_ms(LONG, WIDTH, MARQUEE_REST_MS - 1));
  // On a boundary: a whole step.
  TEST_ASSERT_EQUAL_UINT32(
      MARQUEE_STEP_MS, marquee_next_change_ms(LONG, WIDTH, MARQUEE_REST_MS));
  TEST_ASSERT_EQUAL_UINT32(MARQUEE_STEP_MS,
                           marquee_next_change_ms(LONG, WIDTH, STEP_AT(4)));
  // Part of the way through a step: what is left of it.
  TEST_ASSERT_EQUAL_UINT32(
      MARQUEE_STEP_MS - 100,
      marquee_next_change_ms(LONG, WIDTH, STEP_AT(4) + 100));
  TEST_ASSERT_EQUAL_UINT32(
      1, marquee_next_change_ms(LONG, WIDTH, STEP_AT(5) - 1));
  // The last step before home, then the rest of the next cycle.
  const uint32_t cycle = CYCLE_MS(LONG_LEN);
  TEST_ASSERT_EQUAL_UINT32(1, marquee_next_change_ms(LONG, WIDTH, cycle - 1));
  TEST_ASSERT_EQUAL_UINT32(MARQUEE_REST_MS,
                           marquee_next_change_ms(LONG, WIDTH, cycle));
  TEST_ASSERT_EQUAL_UINT32(MARQUEE_REST_MS - 7,
                           marquee_next_change_ms(LONG, WIDTH, cycle + 7));
}

static void test_null_and_empty_text(void) {
  TEST_ASSERT_EQUAL_MEMORY("                ", window(NULL, WIDTH, 0), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("                ", window(NULL, WIDTH, 5000), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("                ", window("", WIDTH, 0), WIDTH);
  TEST_ASSERT_EQUAL_MEMORY("                ", window("", WIDTH, 5000), WIDTH);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms(NULL, WIDTH, 0));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms("", WIDTH, 5000));
  marquee_window(NULL, 0, 0, NULL);
  marquee_window("", 0, 0, NULL);
}

static void test_width_one_and_width_zero(void) {
  // "ab" through one column: a, b, three spaces, a again.
  TEST_ASSERT_EQUAL_CHAR('a', window("ab", 1, 0)[0]);
  TEST_ASSERT_EQUAL_CHAR('b', window("ab", 1, STEP_AT(1))[0]);
  TEST_ASSERT_EQUAL_CHAR(' ', window("ab", 1, STEP_AT(2))[0]);
  TEST_ASSERT_EQUAL_CHAR(' ', window("ab", 1, STEP_AT(3))[0]);
  TEST_ASSERT_EQUAL_CHAR(' ', window("ab", 1, STEP_AT(4))[0]);
  TEST_ASSERT_EQUAL_CHAR('a', window("ab", 1, STEP_AT(5))[0]);
  TEST_ASSERT_EQUAL_CHAR('a', window("ab", 1, CYCLE_MS(2))[0]);
  TEST_ASSERT_EQUAL_CHAR('b', window("ab", 1, CYCLE_MS(2) + MARQUEE_REST_MS)[0]);
  // One character fits in one column.
  TEST_ASSERT_EQUAL_CHAR('a', window("a", 1, 60000)[0]);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms("a", 1, 0));

  // Width 0: nothing is written (the guard bytes say so; NULL must be safe),
  // and nothing visible can ever change.
  window(LONG, 0, 0);
  window(LONG, 0, STEP_AT(3));
  marquee_window(LONG, 0, STEP_AT(3), NULL);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms(LONG, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, marquee_next_change_ms(NULL, 0, 0));
}

static void test_elapsed_time_near_wraparound(void) {
  const uint32_t cycle = CYCLE_MS(LONG_LEN);
  const uint32_t times[] = {UINT32_MAX, UINT32_MAX - 1, UINT32_MAX - cycle,
                            UINT32_MAX / 2};
  for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
    // Whatever the offset is, the window is the text at some rotation.
    const char *w = window(LONG, WIDTH, times[i]);
    char seen[WIDTH];
    memcpy(seen, w, WIDTH);
    size_t k = 0;
    while (k < LONG_PERIOD && memcmp(rotated(LONG, WIDTH, k), seen, WIDTH)) {
      k++;
    }
    TEST_ASSERT_LESS_THAN_size_t(LONG_PERIOD, k);
    uint32_t next = marquee_next_change_ms(LONG, WIDTH, times[i]);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, next);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MARQUEE_REST_MS, next);
  }
}

// ---- beyond the required list ------------------------------------------------

// Every offset of a whole cycle, checked against the obvious rotation, and
// the next change always lands exactly on the next different offset.
static void test_a_whole_cycle_matches_the_rotation(void) {
  for (size_t n = 1; n <= LONG_PERIOD; n++) {
    size_t k = n % LONG_PERIOD; // the last step is the one that comes home
    TEST_ASSERT_EQUAL_MEMORY(rotated(LONG, WIDTH, k),
                             window(LONG, WIDTH, STEP_AT(n)), WIDTH);
  }
  // Walking by "next change" visits every offset once per cycle.
  uint32_t t = 0;
  for (size_t k = 0; k < 3 * LONG_PERIOD; k++) {
    TEST_ASSERT_EQUAL_MEMORY(rotated(LONG, WIDTH, k % LONG_PERIOD),
                             window(LONG, WIDTH, t), WIDTH);
    t += marquee_next_change_ms(LONG, WIDTH, t);
  }
  TEST_ASSERT_EQUAL_UINT32(3 * CYCLE_MS(LONG_LEN), t);
}

// A text shorter than the width is never asked to wrap, but a text only just
// too long shows the gap and its own start in the same window.
static void test_window_wraps_inside_a_short_period(void) {
  // 5 characters in 4 columns: period 8.
  TEST_ASSERT_EQUAL_MEMORY("abcd", window("abcde", 4, 0), 4);
  TEST_ASSERT_EQUAL_MEMORY("e   ", window("abcde", 4, STEP_AT(4)), 4);
  TEST_ASSERT_EQUAL_MEMORY("  ab", window("abcde", 4, STEP_AT(6)), 4);
  TEST_ASSERT_EQUAL_MEMORY(" abc", window("abcde", 4, STEP_AT(7)), 4);
  TEST_ASSERT_EQUAL_MEMORY("abcd", window("abcde", 4, STEP_AT(8)), 4);
}

// A width greater than the text plus its gap is only ever used with text
// that fits; it must still pad, not wrap.
static void test_wide_window_pads(void) {
  TEST_ASSERT_EQUAL_MEMORY("ab                  ", window("ab", 20, 123456), 20);
}

// A text so long that one cycle does not fit in 32 bits of milliseconds.
// (Static, not heap: about 13 MB of 'x'.)
static char s_huge[13u * 1000u * 1000u];

static void test_enormous_text_does_not_overflow(void) {
  memset(s_huge, 'x', sizeof(s_huge) - 1);
  s_huge[0] = 'A';
  s_huge[1] = 'B';
  s_huge[sizeof(s_huge) - 1] = '\0';
  TEST_ASSERT_EQUAL_MEMORY("ABxx", window(s_huge, 4, 0), 4);
  TEST_ASSERT_EQUAL_MEMORY("Bxxx", window(s_huge, 4, STEP_AT(1)), 4);
  const char *w = window(s_huge, 4, UINT32_MAX);
  TEST_ASSERT_EQUAL_MEMORY("xxxx", w, 4);
  uint32_t next = marquee_next_change_ms(s_huge, 4, UINT32_MAX);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, next);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(MARQUEE_STEP_MS, next);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_short_text_is_padded_and_never_moves);
  RUN_TEST(test_text_of_exactly_the_width_does_not_move);
  RUN_TEST(test_one_character_too_long_moves);
  RUN_TEST(test_it_rests_at_the_start);
  RUN_TEST(test_each_step_moves_one_character);
  RUN_TEST(test_the_gap_separates_end_and_start);
  RUN_TEST(test_it_comes_round_and_rests_again);
  RUN_TEST(test_next_change_during_rest_and_during_steps);
  RUN_TEST(test_null_and_empty_text);
  RUN_TEST(test_width_one_and_width_zero);
  RUN_TEST(test_elapsed_time_near_wraparound);
  RUN_TEST(test_a_whole_cycle_matches_the_rotation);
  RUN_TEST(test_window_wraps_inside_a_short_period);
  RUN_TEST(test_wide_window_pads);
  RUN_TEST(test_enormous_text_does_not_overflow);
  return UNITY_END();
}
