#include "keytest.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_before_any_key_it_shows_a_prompt(void) {
  screen_t s;
  memset(&s, 'x', sizeof(s));
  keytest_render(&s, NULL, 0);
  TEST_ASSERT_EQUAL_MEMORY("Key test       0", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("Press any key  \x08", s.cells[1], SCREEN_COLS);
}

static void test_it_shows_the_key_and_what_happened(void) {
  screen_t s;
  key_event_t ev = {.type = KEY_PRESS, .key = '5'};
  keytest_render(&s, &ev, 1);
  TEST_ASSERT_EQUAL_MEMORY("Key test       1", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("5 press        \x08", s.cells[1], SCREEN_COLS);

  ev = (key_event_t){.type = KEY_RELEASE, .key = '#'};
  keytest_render(&s, &ev, 42);
  TEST_ASSERT_EQUAL_MEMORY("Key test      42", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("# release      \x08", s.cells[1], SCREEN_COLS);
}

static void test_event_names(void) {
  TEST_ASSERT_EQUAL_STRING("press", keytest_event_name(KEY_PRESS));
  TEST_ASSERT_EQUAL_STRING("long", keytest_event_name(KEY_LONG));
  TEST_ASSERT_EQUAL_STRING("repeat", keytest_event_name(KEY_REPEAT));
  TEST_ASSERT_EQUAL_STRING("release", keytest_event_name(KEY_RELEASE));
}

static void test_count_rolls_over_instead_of_overflowing_the_row(void) {
  screen_t s;
  key_event_t ev = {.type = KEY_REPEAT, .key = '8'};
  keytest_render(&s, &ev, 99999);
  TEST_ASSERT_EQUAL_MEMORY("Key test   99999", s.cells[0], SCREEN_COLS);
  keytest_render(&s, &ev, 100003);
  TEST_ASSERT_EQUAL_MEMORY("Key test       3", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("8 repeat       \x08", s.cells[1], SCREEN_COLS);
}

static void test_glyph_is_a_five_pixel_wide_bitmap(void) {
  unsigned char any = 0;
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL_HEX8(0, KEYTEST_GLYPH[i] & 0xE0);
    any |= KEYTEST_GLYPH[i];
  }
  // A blank glyph would be indistinguishable from a space on the LCD.
  TEST_ASSERT_NOT_EQUAL_HEX8(0, any);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_before_any_key_it_shows_a_prompt);
  RUN_TEST(test_it_shows_the_key_and_what_happened);
  RUN_TEST(test_event_names);
  RUN_TEST(test_count_rolls_over_instead_of_overflowing_the_row);
  RUN_TEST(test_glyph_is_a_five_pixel_wide_bitmap);
  return UNITY_END();
}
