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
                                   OPTION_ADD_TO_QUEUE, OPTION_ADD_TO_PLAYLIST};
  assert_offers(BROWSE_TARGET_TRACK, TRACK, 4);
}

void test_album_and_artist_options(void) {
  static const option_t LIST[] = {OPTION_PLAY, OPTION_SHUFFLE_PLAY,
                                  OPTION_ADD_TO_QUEUE, OPTION_ADD_TO_PLAYLIST};
  assert_offers(BROWSE_TARGET_ALBUM, LIST, 4);
  assert_offers(BROWSE_TARGET_ARTIST, LIST, 4);
}

void test_playlist_options(void) {
  static const option_t PLAYLIST[] = {OPTION_PLAY, OPTION_SHUFFLE_PLAY,
                                      OPTION_RENAME, OPTION_DELETE};
  assert_offers(BROWSE_TARGET_PLAYLIST, PLAYLIST, 4);
}

void test_entry_options(void) {
  static const option_t ENTRY[] = {OPTION_PLAY, OPTION_MOVE_UP,
                                   OPTION_MOVE_DOWN, OPTION_REMOVE};
  assert_offers(BROWSE_TARGET_ENTRY, ENTRY, 4);
}

void test_now_playing_options(void) {
  static const option_t NOW_PLAYING[] = {OPTION_ADD_TO_PLAYLIST,
                                         OPTION_SAVE_QUEUE};
  assert_offers(BROWSE_TARGET_NOW_PLAYING, NOW_PLAYING, 2);
}

void test_no_options_for_nothing(void) {
  assert_offers(BROWSE_TARGET_NONE, NULL, 0);
  // A kind that is not one of them offers nothing either.
  assert_offers((browse_target_kind_t)77, NULL, 0);
  // Nowhere to put them: nothing is offered and nothing is written.
  TEST_ASSERT_EQUAL_UINT32(0, options_for(BROWSE_TARGET_TRACK, NULL));
}

void test_texts(void) {
  static const char *const TEXTS[OPTION_COUNT] = {
      [OPTION_PLAY] = "Play",
      [OPTION_PLAY_NEXT] = "Play next",
      [OPTION_ADD_TO_QUEUE] = "Add to queue",
      [OPTION_SHUFFLE_PLAY] = "Shuffle play",
      [OPTION_ADD_TO_PLAYLIST] = "Add to playlist",
      [OPTION_RENAME] = "Rename",
      [OPTION_DELETE] = "Delete",
      [OPTION_MOVE_UP] = "Move up",
      [OPTION_MOVE_DOWN] = "Move down",
      [OPTION_REMOVE] = "Remove",
      [OPTION_SAVE_QUEUE] = "Save queue",
  };
  for (int o = 0; o < OPTION_COUNT; o++) {
    TEST_ASSERT_NOT_NULL(TEXTS[o]); // the table above names every option
    TEST_ASSERT_EQUAL_STRING(TEXTS[o], options_text((option_t)o));
    // Each fits a row of the screen beside the cursor mark ('>').
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SCREEN_COLS - 1,
                                     strlen(options_text((option_t)o)));
  }
  // What is not an option has an empty text, not no text.
  TEST_ASSERT_EQUAL_STRING("", options_text(OPTION_COUNT));
  TEST_ASSERT_EQUAL_STRING("", options_text((option_t)77));
  TEST_ASSERT_EQUAL_STRING("", options_text((option_t)-1));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_track_options);
  RUN_TEST(test_album_and_artist_options);
  RUN_TEST(test_playlist_options);
  RUN_TEST(test_entry_options);
  RUN_TEST(test_now_playing_options);
  RUN_TEST(test_no_options_for_nothing);
  RUN_TEST(test_texts);
  return UNITY_END();
}
