#include "id3.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// An ID3v2 header with the given flags and body length.
static void header(uint8_t out[ID3V2_HEADER_BYTES], uint8_t flags,
                   uint32_t body) {
  const uint8_t h[ID3V2_HEADER_BYTES] = {'I',
                                         'D',
                                         '3',
                                         4,
                                         0,
                                         flags,
                                         (uint8_t)((body >> 21) & 0x7F),
                                         (uint8_t)((body >> 14) & 0x7F),
                                         (uint8_t)((body >> 7) & 0x7F),
                                         (uint8_t)(body & 0x7F)};
  memcpy(out, h, sizeof(h));
}

static void test_tag_length_counts_header_and_body(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0, 34);
  TEST_ASSERT_EQUAL_UINT32(44, id3v2_tag_bytes(h));
}

static void test_empty_tag_is_just_its_header(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(10, id3v2_tag_bytes(h));
}

static void test_length_bytes_carry_seven_bits_each(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  // 0x01 0x02 0x03 0x04 in 7-bit groups: a parser using 8-bit groups, or the
  // bytes in the other order, gets a different number.
  header(h, 0, (1u << 21) | (2u << 14) | (3u << 7) | 4u);
  TEST_ASSERT_EQUAL_UINT8(1, h[6]);
  TEST_ASSERT_EQUAL_UINT8(4, h[9]);
  TEST_ASSERT_EQUAL_UINT32(10 + 2097152 + 32768 + 384 + 4, id3v2_tag_bytes(h));
}

static void test_largest_tag(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0x10, 0x0FFFFFFF);
  TEST_ASSERT_EQUAL_UINT32(268435475u, id3v2_tag_bytes(h));
}

static void test_footer_adds_ten_bytes(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0x10, 100);
  TEST_ASSERT_EQUAL_UINT32(120, id3v2_tag_bytes(h));
}

static void test_other_flags_do_not_change_the_length(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0xE0, 100); // unsynchronisation, extended header, experimental
  TEST_ASSERT_EQUAL_UINT32(110, id3v2_tag_bytes(h));
}

static void test_older_versions_are_tags_too(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0, 300);
  h[3] = 2;
  TEST_ASSERT_EQUAL_UINT32(310, id3v2_tag_bytes(h));
  h[3] = 3;
  TEST_ASSERT_EQUAL_UINT32(310, id3v2_tag_bytes(h));
}

static void test_not_a_tag(void) {
  // The start of an MP3 frame, of a FLAC stream, and nothing at all.
  const uint8_t mp3[ID3V2_HEADER_BYTES] = {0xFF, 0xFB, 0xB0, 0x00};
  const uint8_t flac[ID3V2_HEADER_BYTES] = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
  const uint8_t zeros[ID3V2_HEADER_BYTES] = {0};
  TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(mp3));
  TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(flac));
  TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(zeros));
}

static void test_each_marker_letter_matters(void) {
  for (int i = 0; i < 3; i++) {
    uint8_t h[ID3V2_HEADER_BYTES];
    header(h, 0, 34);
    h[i] ^= 0x20; // the same letter in lower case
    TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(h));
  }
}

static void test_impossible_version_is_not_a_tag(void) {
  uint8_t h[ID3V2_HEADER_BYTES];
  header(h, 0, 34);
  h[3] = 0xFF;
  TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(h));
  header(h, 0, 34);
  h[4] = 0xFF;
  TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(h));
}

static void test_length_byte_with_top_bit_is_not_a_tag(void) {
  for (int i = 6; i < ID3V2_HEADER_BYTES; i++) {
    uint8_t h[ID3V2_HEADER_BYTES];
    header(h, 0, 34);
    h[i] |= 0x80;
    TEST_ASSERT_EQUAL_UINT32(0, id3v2_tag_bytes(h));
  }
}

static void test_id3v1_marker(void) {
  const uint8_t tag[ID3V1_MARKER_BYTES] = {'T', 'A', 'G'};
  TEST_ASSERT_TRUE(id3v1_tag_begins(tag));
}

static void test_id3v1_marker_needs_all_three_letters(void) {
  const uint8_t a[ID3V1_MARKER_BYTES] = {'t', 'A', 'G'};
  const uint8_t b[ID3V1_MARKER_BYTES] = {'T', 'a', 'G'};
  const uint8_t c[ID3V1_MARKER_BYTES] = {'T', 'A', 'g'};
  const uint8_t audio[ID3V1_MARKER_BYTES] = {0xAA, 0xAA, 0xAA};
  TEST_ASSERT_FALSE(id3v1_tag_begins(a));
  TEST_ASSERT_FALSE(id3v1_tag_begins(b));
  TEST_ASSERT_FALSE(id3v1_tag_begins(c));
  TEST_ASSERT_FALSE(id3v1_tag_begins(audio));
}

// The MP3 test files made by tools/make_test_audio.sh begin with a 44-byte
// tag written by ffmpeg, and an MP3 frame follows it. Checked against the
// real bytes so that the arithmetic above is not only tested against itself.
static void test_real_tag_from_ffmpeg(void) {
  const uint8_t h[ID3V2_HEADER_BYTES] = {0x49, 0x44, 0x33, 0x04, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x22};
  TEST_ASSERT_EQUAL_UINT32(44, id3v2_tag_bytes(h));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_tag_length_counts_header_and_body);
  RUN_TEST(test_empty_tag_is_just_its_header);
  RUN_TEST(test_length_bytes_carry_seven_bits_each);
  RUN_TEST(test_largest_tag);
  RUN_TEST(test_footer_adds_ten_bytes);
  RUN_TEST(test_other_flags_do_not_change_the_length);
  RUN_TEST(test_older_versions_are_tags_too);
  RUN_TEST(test_not_a_tag);
  RUN_TEST(test_each_marker_letter_matters);
  RUN_TEST(test_impossible_version_is_not_a_tag);
  RUN_TEST(test_length_byte_with_top_bit_is_not_a_tag);
  RUN_TEST(test_id3v1_marker);
  RUN_TEST(test_id3v1_marker_needs_all_three_letters);
  RUN_TEST(test_real_tag_from_ffmpeg);
  return UNITY_END();
}
