#include "translit.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ascii_passes_through(void) {
  char out[32];
  translit_stats_t s = translit("Kid A (2000)", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Kid A (2000)", out);
  TEST_ASSERT_EQUAL_UINT(12, s.chars);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_backslash_and_tilde_are_replaced(void) {
  char out[32];
  translit("AC\\DC ~1", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AC/DC -1", out);
}

static void test_control_characters_become_spaces(void) {
  char out[32];
  translit_stats_t s = translit("A\tB\nC", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A B C", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_latin1_accents_lose_their_marks(void) {
  char out[32];
  translit("Beyonc\xC3\xA9", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Beyonce", out);
  translit("Sigur R\xC3\xB3" "s", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Sigur Ros", out);
  translit("Mot\xC3\xB6" "rhead", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Motorhead", out);
  translit("\xC3\x98" "l", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Ol", out);
  translit("\xC3\x80\xC3\xBF", out, sizeof(out)); // first and last of the range
  TEST_ASSERT_EQUAL_STRING("Ay", out);
}

static void test_multi_letter_replacements(void) {
  char out[32];
  translit_stats_t s = translit("Stra\xC3\x9F" "e", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Strasse", out);
  TEST_ASSERT_EQUAL_UINT(6, s.chars); // counts input characters, not output
  translit("\xC3\x86" "on", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AEon", out);
  translit("\xC3\xA6\xC3\x9E\xC3\xBE", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("aeThth", out);
  translit("\xC5\x92" "uvre \xC5\x93", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("OEuvre oe", out);
  translit("\xC4\xB2\xC4\xB3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("IJij", out);
}

static void test_latin_extended_a(void) {
  char out[32];
  translit("\xC5\x81\xC3\xB3" "d\xC5\xBA", out, sizeof(out)); // Lodz
  TEST_ASSERT_EQUAL_STRING("Lodz", out);
  translit("Dvo\xC5\x99\xC3\xA1" "k", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Dvorak", out);
  translit("\xC4\x80\xC5\xBF", out, sizeof(out)); // first and last of the range
  TEST_ASSERT_EQUAL_STRING("As", out);
  translit("\xC5\xA0\xC5\xA1\xC5\xBD\xC5\xBE", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("SsZz", out);
  translit("\xC4\x9E\xC4\xB1\xC5\x9F", out, sizeof(out)); // Turkish G, dotless i, s
  TEST_ASSERT_EQUAL_STRING("Gis", out);
}

static void test_typographic_punctuation(void) {
  char out[32];
  translit_stats_t s = translit("Don\xE2\x80\x99" "t Stop", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Don't Stop", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
  translit("\xE2\x80\x9C" "Hi\xE2\x80\x9D", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\"Hi\"", out);
  translit("A\xE2\x80\x93" "B\xE2\x80\x94" "C", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A-B-C", out);
  translit("Wait\xE2\x80\xA6", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Wait...", out);
  translit("\xC2\xAB" "x\xC2\xBB \xC2\xA1\xC2\xBF", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\"x\" !?", out);
  s = translit("a\xC2\xA0" "b\xE2\x80\xA2", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a b*", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_unsupported_scripts_become_question_marks(void) {
  char out[32];
  translit_stats_t s = translit("\xE4\xBD\xA0\xE5\xA5\xBD", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("??", out);
  TEST_ASSERT_EQUAL_UINT(2, s.chars);
  TEST_ASSERT_EQUAL_UINT(2, s.unknown);
  s = translit("\xD0\x9C\xD0\xB8\xD1\x80", out, sizeof(out)); // Cyrillic
  TEST_ASSERT_EQUAL_STRING("???", out);
  TEST_ASSERT_EQUAL_UINT(3, s.unknown);
  s = translit("\xF0\x9F\x8E\xB5", out, sizeof(out)); // four-byte emoji
  TEST_ASSERT_EQUAL_STRING("?", out);
  TEST_ASSERT_EQUAL_UINT(1, s.chars);
}

static void test_invalid_utf8_stray_continuation(void) {
  char out[32];
  translit_stats_t s = translit("\x80" "abc", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("?abc", out);
  TEST_ASSERT_EQUAL_UINT(1, s.unknown);
}

static void test_invalid_utf8_truncated_at_end(void) {
  // Exactly-sized heap copies, so AddressSanitizer catches any read past the
  // terminator.
  const char two_byte[] = "abc\xC3";
  char *in = malloc(sizeof(two_byte));
  memcpy(in, two_byte, sizeof(two_byte));
  char out[32];
  translit(in, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("abc?", out);
  free(in);

  const char three_byte[] = "ab\xE4\xBD";
  in = malloc(sizeof(three_byte));
  memcpy(in, three_byte, sizeof(three_byte));
  translit_stats_t s = translit(in, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ab??", out);
  TEST_ASSERT_EQUAL_UINT(2, s.unknown);
  free(in);
}

static void test_truncation_keeps_terminator(void) {
  char out[5];
  translit_stats_t s = translit("Radiohead", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Radi", out);
  TEST_ASSERT_EQUAL_UINT(9, s.chars); // whole input is still counted
}

static void test_truncation_inside_multi_letter_replacement(void) {
  char out[3];
  translit("\xE2\x80\xA6", out, sizeof(out)); // "..." into room for two
  TEST_ASSERT_EQUAL_STRING("..", out);
}

static void test_truncation_size_one_and_zero(void) {
  char one[1] = {'x'};
  translit("abc", one, sizeof(one));
  TEST_ASSERT_EQUAL_CHAR('\0', one[0]);

  char canary[2] = {'x', 'y'};
  translit("abc", canary, 0);
  TEST_ASSERT_EQUAL_CHAR('x', canary[0]);
  TEST_ASSERT_EQUAL_CHAR('y', canary[1]);
}

static void test_null_input_gives_empty_string(void) {
  char out[8] = "junk";
  translit_stats_t s = translit(NULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT(0, s.chars);
}

static void test_title_is_used_when_displayable(void) {
  char out[32];
  translit_title("Beyonc\xC3\xA9", "01 track.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Beyonce", out);
}

static void test_title_mostly_unknown_falls_back_to_file_name(void) {
  char out[32];
  translit_title("\xE4\xBD\xA0\xE5\xA5\xBD", "nihao.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("nihao", out);
}

static void test_title_exactly_half_unknown_is_kept(void) {
  char out[32];
  translit_title("A\xE4\xBD\xA0", "fallback.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A?", out);
}

static void test_title_missing_falls_back_to_file_name(void) {
  char out[32];
  translit_title("", "07 - Song.mp3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("07 - Song", out);
  translit_title(NULL, "07 - Song.mp3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("07 - Song", out);
}

static void test_title_fallback_extension_handling(void) {
  char out[32];
  translit_title("", "README", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("README", out);
  translit_title("", "a.b.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a.b", out);
  translit_title("", ".hidden", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(".hidden", out);
  translit_title(NULL, NULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_passes_through);
  RUN_TEST(test_backslash_and_tilde_are_replaced);
  RUN_TEST(test_control_characters_become_spaces);
  RUN_TEST(test_latin1_accents_lose_their_marks);
  RUN_TEST(test_multi_letter_replacements);
  RUN_TEST(test_latin_extended_a);
  RUN_TEST(test_typographic_punctuation);
  RUN_TEST(test_unsupported_scripts_become_question_marks);
  RUN_TEST(test_invalid_utf8_stray_continuation);
  RUN_TEST(test_invalid_utf8_truncated_at_end);
  RUN_TEST(test_truncation_keeps_terminator);
  RUN_TEST(test_truncation_inside_multi_letter_replacement);
  RUN_TEST(test_truncation_size_one_and_zero);
  RUN_TEST(test_null_input_gives_empty_string);
  RUN_TEST(test_title_is_used_when_displayable);
  RUN_TEST(test_title_mostly_unknown_falls_back_to_file_name);
  RUN_TEST(test_title_exactly_half_unknown_is_kept);
  RUN_TEST(test_title_missing_falls_back_to_file_name);
  RUN_TEST(test_title_fallback_extension_handling);
  return UNITY_END();
}
