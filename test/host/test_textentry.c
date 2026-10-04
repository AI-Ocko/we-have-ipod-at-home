#include "textentry.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static textentry_t t;

// The text as '#' would finish it.
static const char *text(void) {
  static char out[TEXTENTRY_MAX + 1];
  textentry_text(&t, out, sizeof(out));
  return out;
}

static void press(char key, int64_t now_ms) {
  TEST_ASSERT_EQUAL(TEXTENTRY_CONTINUE, textentry_key(&t, key, now_ms));
}

#define ASSERT_ROW(expected)                                                   \
  do {                                                                         \
    char row_[TEXTENTRY_ROW + 1];                                              \
    row_[TEXTENTRY_ROW] = '!';                                                 \
    textentry_row(&t, row_);                                                   \
    TEST_ASSERT_EQUAL_CHAR('!', row_[TEXTENTRY_ROW]);                          \
    TEST_ASSERT_EQUAL_CHAR_ARRAY(expected, row_, TEXTENTRY_ROW);               \
  } while (0)

// --- Typing ------------------------------------------------------------------

void test_each_key_cycles_its_letters_then_its_digit(void) {
  static const struct {
    char key;
    const char *cycle;
  } KEYS[] = {
      {'2', "ABC2"},  {'3', "DEF3"},  {'4', "GHI4"},   {'5', "JKL5"},
      {'6', "MNO6"},  {'7', "PQRS7"}, {'8', "TUV8"},   {'9', "WXYZ9"},
      {'0', " 0"},    {'1', "-'&.1"},
  };
  for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
    size_t n = strlen(KEYS[k].cycle);
    textentry_init(&t, NULL);
    // Twice round the cycle and one more: after the digit it starts again.
    for (size_t p = 0; p <= 2 * n; p++) {
      press(KEYS[k].key, (int64_t)p * 100);
      char expected[2] = {KEYS[k].cycle[p % n], '\0'};
      TEST_ASSERT_EQUAL_STRING(expected, text());
      TEST_ASSERT_EQUAL_STRING("", t.text);
      TEST_ASSERT_EQUAL_UINT32(0, t.len);
    }
  }
}

void test_same_key_within_a_second_cycles_and_after_a_second_commits(void) {
  textentry_init(&t, NULL);
  press('2', 0);
  press('2', TEXTENTRY_COMMIT_MS - 1);
  TEST_ASSERT_EQUAL_STRING("B", text());
  // The second counts from the last press, not the first.
  press('2', 2 * (TEXTENTRY_COMMIT_MS - 1));
  TEST_ASSERT_EQUAL_STRING("C", text());
  TEST_ASSERT_EQUAL_STRING("", t.text);
  // A full second later the same key starts a new character, with no tick in
  // between.
  press('2', 2 * (TEXTENTRY_COMMIT_MS - 1) + TEXTENTRY_COMMIT_MS);
  TEST_ASSERT_EQUAL_STRING("C", t.text);
  TEST_ASSERT_EQUAL_STRING("CA", text());
  press('2', 10000);
  press('2', 10500);
  TEST_ASSERT_EQUAL_STRING("CA", t.text);
  TEST_ASSERT_EQUAL_STRING("CAB", text());
}

void test_a_different_key_commits_the_pending_character(void) {
  textentry_init(&t, NULL);
  press('2', 0);
  press('2', 10);
  press('3', 20);
  TEST_ASSERT_EQUAL_STRING("B", t.text);
  TEST_ASSERT_EQUAL_STRING("BD", text());
  TEST_ASSERT_EQUAL_INT64(20 + TEXTENTRY_COMMIT_MS, textentry_deadline(&t));
  press('3', 30);
  press('2', 40);
  TEST_ASSERT_EQUAL_STRING("BE", t.text);
  TEST_ASSERT_EQUAL_STRING("BEA", text());
  press('0', 50);
  press('1', 60);
  press('1', 70);
  TEST_ASSERT_EQUAL_STRING("BEA ", t.text);
  TEST_ASSERT_EQUAL_STRING("BEA '", text());
}

void test_tick_commits_after_the_deadline_and_not_before(void) {
  textentry_init(&t, NULL);
  press('7', 5000);
  TEST_ASSERT_EQUAL_INT64(5000 + TEXTENTRY_COMMIT_MS, textentry_deadline(&t));
  TEST_ASSERT_FALSE(textentry_tick(&t, 5000));
  TEST_ASSERT_FALSE(textentry_tick(&t, 5000 + TEXTENTRY_COMMIT_MS - 1));
  TEST_ASSERT_EQUAL_STRING("", t.text);
  TEST_ASSERT_EQUAL_STRING("P", text());
  TEST_ASSERT_TRUE(textentry_tick(&t, 5000 + TEXTENTRY_COMMIT_MS));
  TEST_ASSERT_EQUAL_STRING("P", t.text);
  TEST_ASSERT_EQUAL_STRING("P", text());
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  TEST_ASSERT_FALSE(textentry_tick(&t, 99999));
  TEST_ASSERT_EQUAL_STRING("P", text());

  // After a commit the same key starts a new character, however soon.
  press('7', 6100);
  press('7', 6200);
  TEST_ASSERT_EQUAL_STRING("PQ", text());
  TEST_ASSERT_EQUAL_INT64(6200 + TEXTENTRY_COMMIT_MS, textentry_deadline(&t));
  // A tick long after commits once.
  TEST_ASSERT_TRUE(textentry_tick(&t, 1000000));
  TEST_ASSERT_EQUAL_STRING("PQ", t.text);
  TEST_ASSERT_FALSE(textentry_tick(&t, 1000001));
}

void test_deadline_is_max_with_nothing_pending(void) {
  textentry_init(&t, NULL);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  textentry_init(&t, "ROAD");
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  press('*', 0);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  press('5', 10);
  press('*', 20); // drops the pending character
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  press('5', 30);
  TEST_ASSERT_EQUAL(TEXTENTRY_DONE, textentry_key(&t, '#', 40));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  // Negative times are times too.
  textentry_init(&t, NULL);
  press('2', -5000);
  TEST_ASSERT_EQUAL_INT64(-5000 + TEXTENTRY_COMMIT_MS, textentry_deadline(&t));
}

// --- Other keys --------------------------------------------------------------

void test_star_drops_the_pending_character_or_deletes_the_last(void) {
  textentry_init(&t, "AB");
  press('2', 0);
  press('2', 10);
  TEST_ASSERT_EQUAL_STRING("ABB", text());
  press('*', 20);
  TEST_ASSERT_EQUAL_STRING("AB", text());
  TEST_ASSERT_EQUAL_STRING("AB", t.text);
  press('*', 30);
  TEST_ASSERT_EQUAL_STRING("A", text());
  TEST_ASSERT_EQUAL_UINT32(1, t.len);
  press('*', 40);
  TEST_ASSERT_EQUAL_STRING("", text());
  press('*', 50); // nothing left to delete
  TEST_ASSERT_EQUAL_STRING("", text());
  TEST_ASSERT_EQUAL_UINT32(0, t.len);
  // After a drop the same key starts again from its first character.
  press('2', 60);
  press('2', 70);
  press('*', 80);
  press('2', 90);
  TEST_ASSERT_EQUAL_STRING("A", text());
}

void test_hash_commits_and_finishes(void) {
  textentry_init(&t, "RO");
  press('2', 0);
  TEST_ASSERT_EQUAL(TEXTENTRY_DONE, textentry_key(&t, '#', 10));
  TEST_ASSERT_EQUAL_STRING("ROA", t.text);
  TEST_ASSERT_EQUAL_UINT32(3, t.len);
  TEST_ASSERT_EQUAL_STRING("ROA", text());
  // Nothing pending: done, the text as it was.
  TEST_ASSERT_EQUAL(TEXTENTRY_DONE, textentry_key(&t, '#', 20));
  TEST_ASSERT_EQUAL_STRING("ROA", text());
  // A pending character long overdue is committed once.
  press('3', 30);
  TEST_ASSERT_EQUAL(TEXTENTRY_DONE, textentry_key(&t, '#', 90000));
  TEST_ASSERT_EQUAL_STRING("ROAD", t.text);
  // An empty text is finished too; whether it will do is the caller's affair.
  textentry_init(&t, NULL);
  TEST_ASSERT_EQUAL(TEXTENTRY_DONE, textentry_key(&t, '#', 0));
  TEST_ASSERT_EQUAL_STRING("", text());

  // textentry_text cuts to a small buffer and always terminates.
  textentry_init(&t, "ROAD");
  press('2', 0);
  char small[4];
  memset(small, 'x', sizeof(small));
  textentry_text(&t, small, 3);
  TEST_ASSERT_EQUAL_STRING("RO", small);
  TEST_ASSERT_EQUAL_CHAR('x', small[3]);
  textentry_text(&t, small, 1);
  TEST_ASSERT_EQUAL_STRING("", small);
  textentry_text(&t, small, 0); // writes nothing
  TEST_ASSERT_EQUAL_STRING("", small);
  char exact[6];
  textentry_text(&t, exact, sizeof(exact));
  TEST_ASSERT_EQUAL_STRING("ROADA", exact);
}

void test_c_cancels(void) {
  textentry_init(&t, "ROAD");
  press('2', 0);
  TEST_ASSERT_EQUAL(TEXTENTRY_CANCEL, textentry_key(&t, 'C', 10));
  TEST_ASSERT_EQUAL_STRING("ROAD", t.text);
  textentry_init(&t, NULL);
  TEST_ASSERT_EQUAL(TEXTENTRY_CANCEL, textentry_key(&t, 'C', 0));
}

void test_other_keys_do_nothing(void) {
  static const char OTHERS[] = {'A', 'B', 'D', 'c', 'a', 'X', ' ', '_', '/',
                                '\0', '\n', (char)0xC3, (char)0x7F};
  textentry_init(&t, "ROAD");
  press('8', 1000);
  for (size_t i = 0; i < sizeof(OTHERS); i++) {
    textentry_t before = t;
    // Within the second and long after it: neither commits nor changes.
    press(OTHERS[i], 1001);
    press(OTHERS[i], 50000);
    TEST_ASSERT_EQUAL_MEMORY(&before, &t, sizeof(t));
  }
  TEST_ASSERT_EQUAL_STRING("ROADT", text());
  // The pending character's second still runs from its own key.
  press('8', 1999);
  TEST_ASSERT_EQUAL_STRING("ROADU", text());
}

// --- Limits and the start ----------------------------------------------------

void test_length_limit(void) {
  // At the limit a typing key does nothing.
  textentry_init(&t, "ABCDEFGHIJKLMNOP");
  TEST_ASSERT_EQUAL_UINT32(TEXTENTRY_MAX, t.len);
  textentry_t before = t;
  press('2', 0);
  press('0', 5000);
  TEST_ASSERT_EQUAL_MEMORY(&before, &t, sizeof(t));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
  // Room again after a delete.
  press('*', 6000);
  press('9', 6100);
  press('9', 6200);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOX", text());

  // One short with one pending: the same key still cycles, ...
  textentry_init(&t, "ABCDEFGHIJKLMNO");
  press('2', 0);
  press('2', 10);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOB", text());
  // ... a different key commits the pending one and is then ignored.
  press('3', 20);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOB", t.text);
  TEST_ASSERT_EQUAL_UINT32(TEXTENTRY_MAX, t.len);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOB", text());
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));

  // The same key after the second does the same.
  textentry_init(&t, "ABCDEFGHIJKLMNO");
  press('2', 0);
  press('2', TEXTENTRY_COMMIT_MS);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOA", t.text);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOA", text());
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
}

void test_init_cleans_the_initial_text(void) {
  // Whatever was there before goes.
  memset(&t, 0x55, sizeof(t));
  textentry_init(&t, NULL);
  TEST_ASSERT_EQUAL_STRING("", t.text);
  TEST_ASSERT_EQUAL_UINT32(0, t.len);
  TEST_ASSERT_EQUAL_CHAR(0, t.key);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));

  textentry_init(&t, "road trip");
  TEST_ASSERT_EQUAL_STRING("ROAD TRIP", t.text);
  TEST_ASSERT_EQUAL_UINT32(9, t.len);
  textentry_init(&t, "-'&. 0123456789");
  TEST_ASSERT_EQUAL_STRING("-'&. 0123456789", t.text);
  // Characters a name cannot have, including UTF-8 bytes, are dropped.
  textentry_init(&t, "Caf\xC3\xA9/\xC3\xBC!?_x\t~");
  TEST_ASSERT_EQUAL_STRING("CAFX", t.text);
  TEST_ASSERT_EQUAL_UINT32(4, t.len);
  // Too long: cut to TEXTENTRY_MAX, after the dropping.
  textentry_init(&t, "abcdefghijklmnopqrstuvwxyz");
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", t.text);
  TEST_ASSERT_EQUAL_UINT32(TEXTENTRY_MAX, t.len);
  textentry_init(&t, "!!!!!!!!!!!!!!!!!!!!ab");
  TEST_ASSERT_EQUAL_STRING("AB", t.text);
  textentry_init(&t, "");
  TEST_ASSERT_EQUAL_STRING("", t.text);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, textentry_deadline(&t));
}

void test_row_shows_the_cursor_the_pending_character_and_the_end_of_a_long_text(
    void) {
  textentry_init(&t, NULL);
  ASSERT_ROW("_               ");
  press('7', 0);
  ASSERT_ROW("P               ");
  press('7', 10);
  ASSERT_ROW("Q               ");
  TEST_ASSERT_TRUE(textentry_tick(&t, 10 + TEXTENTRY_COMMIT_MS));
  ASSERT_ROW("Q_              ");

  textentry_init(&t, "road trip");
  ASSERT_ROW("ROAD TRIP_      ");
  press('0', 0);
  ASSERT_ROW("ROAD TRIP       "); // the pending space, then padding
  // One short: the pending character fills the row.
  textentry_init(&t, "ABCDEFGHIJKLMNO");
  ASSERT_ROW("ABCDEFGHIJKLMNO_");
  press('5', 0);
  ASSERT_ROW("ABCDEFGHIJKLMNOJ");
  // Full: no room for a cursor; the row ends with the text's end.
  press('6', 10);
  ASSERT_ROW("ABCDEFGHIJKLMNOJ");
  textentry_init(&t, "abcdefghijklmnopqrstuvwxyz");
  ASSERT_ROW("ABCDEFGHIJKLMNOP");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_each_key_cycles_its_letters_then_its_digit);
  RUN_TEST(test_same_key_within_a_second_cycles_and_after_a_second_commits);
  RUN_TEST(test_a_different_key_commits_the_pending_character);
  RUN_TEST(test_tick_commits_after_the_deadline_and_not_before);
  RUN_TEST(test_deadline_is_max_with_nothing_pending);
  RUN_TEST(test_star_drops_the_pending_character_or_deletes_the_last);
  RUN_TEST(test_hash_commits_and_finishes);
  RUN_TEST(test_c_cancels);
  RUN_TEST(test_other_keys_do_nothing);
  RUN_TEST(test_length_limit);
  RUN_TEST(test_init_cleans_the_initial_text);
  RUN_TEST(
      test_row_shows_the_cursor_the_pending_character_and_the_end_of_a_long_text);
  return UNITY_END();
}
