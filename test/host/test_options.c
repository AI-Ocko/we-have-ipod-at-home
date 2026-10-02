#include "options.h"
#include "screen.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// A value no action has, to see what a call wrote.
#define UNTOUCHED ((option_t)99)

// `kind` offers exactly these actions, in this order, and the rest of the
// caller's array is left alone.
static void assert_offers(browse_target_kind_t kind, const option_t *expected,
                          uint32_t n) {
  option_t out[OPTIONS_MAX + 1];
  for (uint32_t i = 0; i <= OPTIONS_MAX; i++) {
    out[i] = UNTOUCHED;
  }
  TEST_ASSERT_EQUAL_UINT32(n, options_for(kind, out));
  for (uint32_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL_INT(expected[i], out[i]);
  }
  for (uint32_t i = n; i <= OPTIONS_MAX; i++) {
    TEST_ASSERT_EQUAL_INT(UNTOUCHED, out[i]);
  }
}

void test_track_options(void) {
  static const option_t TRACK[] = {OPTION_PLAY, OPTION_PLAY_NEXT,
                                   OPTION_ADD_TO_QUEUE};
  assert_offers(BROWSE_TARGET_TRACK, TRACK, 3);
}

void test_album_and_artist_options(void) {
  static const option_t LIST[] = {OPTION_PLAY, OPTION_SHUFFLE_PLAY,
                                  OPTION_ADD_TO_QUEUE};
  assert_offers(BROWSE_TARGET_ALBUM, LIST, 3);
  assert_offers(BROWSE_TARGET_ARTIST, LIST, 3);
}

void test_no_options_for_nothing(void) {
  assert_offers(BROWSE_TARGET_NONE, NULL, 0);
  // A kind that is not one of the four offers nothing either.
  assert_offers((browse_target_kind_t)77, NULL, 0);
  // Nowhere to put them: nothing is offered and nothing is written.
  TEST_ASSERT_EQUAL_UINT32(0, options_for(BROWSE_TARGET_TRACK, NULL));
}

void test_texts(void) {
  TEST_ASSERT_EQUAL_STRING("Play", options_text(OPTION_PLAY));
  TEST_ASSERT_EQUAL_STRING("Play next", options_text(OPTION_PLAY_NEXT));
  TEST_ASSERT_EQUAL_STRING("Add to queue", options_text(OPTION_ADD_TO_QUEUE));
  TEST_ASSERT_EQUAL_STRING("Shuffle play", options_text(OPTION_SHUFFLE_PLAY));
  // An action that is not one of the four has an empty text, not no text.
  TEST_ASSERT_EQUAL_STRING("", options_text((option_t)77));
  TEST_ASSERT_EQUAL_STRING("", options_text((option_t)-1));
  // Each fits a row of the screen.
  for (int o = OPTION_PLAY; o <= OPTION_SHUFFLE_PLAY; o++) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SCREEN_COLS,
                                     strlen(options_text((option_t)o)));
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_track_options);
  RUN_TEST(test_album_and_artist_options);
  RUN_TEST(test_no_options_for_nothing);
  RUN_TEST(test_texts);
  return UNITY_END();
}
