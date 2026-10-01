#include "mp3_xing.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// --- Real files --------------------------------------------------------------
//
// The first frames of the two MP3 test files made by tools/make_test_audio.sh
// (the bytes after their 44-byte ID3v2 tag), both 10.0 s long.

// t04_44k_cbr192_st.mp3: MPEG-1, 44.1 kHz, stereo, "Info", 384 frames.
static const uint8_t T04[MP3_XING_PROBE_BYTES] = {
    0xff, 0xfb, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x6e, 0x66, 0x6f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x01, 0x80,
};

// t05_48k_vbr_mono.mp3: MPEG-1, 48 kHz, mono, "Xing", 418 frames.
static const uint8_t T05[MP3_XING_PROBE_BYTES] = {
    0xff, 0xfb, 0x54, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x69, 0x6e,
    0x67, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x01, 0xa2, 0x00, 0x00, 0x9f,
    0xf0, 0x00, 0x08, 0x0a, 0x0d, 0x0f, 0x12, 0x14, 0x16, 0x19, 0x1b, 0x1e,
};

static void test_real_cbr_stereo_file(void) {
  // 384 x 1152 / 44100 s
  TEST_ASSERT_EQUAL_UINT32(10031, mp3_xing_duration_ms(T04, sizeof(T04)));
}

static void test_real_vbr_mono_file(void) {
  // 418 x 1152 / 48000 s
  TEST_ASSERT_EQUAL_UINT32(10032, mp3_xing_duration_ms(T05, sizeof(T05)));
}

// --- Frames built by hand ----------------------------------------------------

static uint8_t img[MP3_XING_PROBE_BYTES + 16];

#define MPEG1 3
#define MPEG2 2
#define MPEG25 0
#define STEREO 0
#define JOINT 1
#define DUAL 2
#define MONO 3

// A Layer III frame header and an all-zero frame after it.
static void img_frame(unsigned version, unsigned rate_index, unsigned mode) {
  memset(img, 0, sizeof(img));
  img[0] = 0xFF;
  img[1] = (uint8_t)(0xE0 | (version << 3) | (1 << 1) | 1);
  img[2] = (uint8_t)((9 << 4) | (rate_index << 2));
  img[3] = (uint8_t)(mode << 6);
}

static void img_tag(size_t at, const char *tag, uint32_t flags,
                    uint32_t frames) {
  memcpy(&img[at], tag, 4);
  img[at + 4] = (uint8_t)(flags >> 24);
  img[at + 5] = (uint8_t)(flags >> 16);
  img[at + 6] = (uint8_t)(flags >> 8);
  img[at + 7] = (uint8_t)flags;
  img[at + 8] = (uint8_t)(frames >> 24);
  img[at + 9] = (uint8_t)(frames >> 16);
  img[at + 10] = (uint8_t)(frames >> 8);
  img[at + 11] = (uint8_t)frames;
}

static void test_tag_position_depends_on_version_and_channels(void) {
  // 1000 frames each. The tag sits after 32, 17, 17 or 9 bytes of side
  // information; put anywhere else it must not be found.
  static const struct {
    unsigned version, mode;
    size_t side;
    uint32_t ms; // 1000 frames at rate index 0
  } CASES[] = {
      {MPEG1, STEREO, 32, 26122}, // 1152000 samples at 44100
      {MPEG1, JOINT, 32, 26122},
      {MPEG1, DUAL, 32, 26122},
      {MPEG1, MONO, 17, 26122},
      {MPEG2, STEREO, 17, 26122}, // 576000 samples at 22050
      {MPEG2, MONO, 9, 26122},
      {MPEG25, STEREO, 17, 52244}, // 576000 samples at 11025
      {MPEG25, MONO, 9, 52244},
  };
  static const size_t SIDES[] = {9, 17, 32};
  for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    for (size_t s = 0; s < 3; s++) {
      img_frame(CASES[i].version, 0, CASES[i].mode);
      img_tag(4 + SIDES[s], "Xing", 1, 1000);
      uint32_t want = (SIDES[s] == CASES[i].side) ? CASES[i].ms : 0;
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(
          want, mp3_xing_duration_ms(img, sizeof(img)), "case");
    }
  }
}

static void test_every_sample_rate(void) {
  static const struct {
    unsigned version, rate_index;
    uint32_t ms; // of 1000 frames
  } CASES[] = {
      {MPEG1, 0, 26122},  {MPEG1, 1, 24000},  {MPEG1, 2, 36000},
      {MPEG2, 0, 26122},  {MPEG2, 1, 24000},  {MPEG2, 2, 36000},
      {MPEG25, 0, 52244}, {MPEG25, 1, 48000}, {MPEG25, 2, 72000},
  };
  for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    img_frame(CASES[i].version, CASES[i].rate_index, STEREO);
    img_tag(4 + (CASES[i].version == MPEG1 ? 32 : 17), "Info", 1, 1000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        CASES[i].ms, mp3_xing_duration_ms(img, sizeof(img)), "case");
  }
}

static void test_ordinary_audio_frame_has_no_length(void) {
  img_frame(MPEG1, 0, STEREO);
  memset(&img[4], 0x55, sizeof(img) - 4);
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
}

static void test_tag_without_a_frame_count(void) {
  img_frame(MPEG1, 0, STEREO);
  img_tag(36, "Xing", 0x0E, 1000); // bytes, TOC and quality, but no frames
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
}

static void test_zero_frames_is_unknown(void) {
  img_frame(MPEG1, 0, STEREO);
  img_tag(36, "Xing", 1, 0);
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
}

static void test_not_a_frame(void) {
  img_frame(MPEG1, 0, STEREO);
  img_tag(36, "Xing", 1, 1000);
  TEST_ASSERT_EQUAL_UINT32(26122, mp3_xing_duration_ms(img, sizeof(img)));

  uint8_t first = img[0], second = img[1], third = img[2];
  img[0] = 0xFE; // no sync
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
  img[0] = first;
  img[1] = (uint8_t)(second & ~0x20); // ten sync bits only
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
  img[1] = (uint8_t)((second & ~0x18) | (1 << 3)); // reserved version
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
  img[1] = (uint8_t)((second & ~0x06) | (2 << 1)); // Layer II
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
  img[1] = second;
  img[2] = (uint8_t)(third | (3 << 2)); // reserved sample rate
  TEST_ASSERT_EQUAL_UINT32(0, mp3_xing_duration_ms(img, sizeof(img)));
}

// The bytes are copied into a buffer of exactly the length under test, so
// that reading one byte too many is caught by the address sanitizer.
static uint32_t duration_of_first(const uint8_t *bytes, size_t len) {
  uint8_t exact[MP3_XING_PROBE_BYTES];
  uint8_t *copy = exact + sizeof(exact) - len; // ends at the array's end
  memcpy(copy, bytes, len);
  return mp3_xing_duration_ms(copy, len);
}

static void test_short_input_reads_nothing_beyond_it(void) {
  // T04 needs all 48 bytes, T05 (mono) 33.
  for (size_t len = 0; len < MP3_XING_PROBE_BYTES; len++) {
    TEST_ASSERT_EQUAL_UINT32(0, duration_of_first(T04, len));
    TEST_ASSERT_EQUAL_UINT32(len >= 33 ? 10032 : 0,
                             duration_of_first(T05, len));
  }
  TEST_ASSERT_EQUAL_UINT32(10031, duration_of_first(T04, MP3_XING_PROBE_BYTES));
}

static void test_huge_count_does_not_overflow(void) {
  img_frame(MPEG25, 2, STEREO); // 8000 Hz: the longest time per frame
  img_tag(4 + 17, "Xing", 1, 0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, mp3_xing_duration_ms(img, sizeof(img)));
  // The largest count that still fits: 72 ms a frame.
  img_tag(4 + 17, "Xing", 1, 59652323);
  TEST_ASSERT_EQUAL_UINT32(4294967256u, mp3_xing_duration_ms(img, sizeof(img)));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_real_cbr_stereo_file);
  RUN_TEST(test_real_vbr_mono_file);
  RUN_TEST(test_tag_position_depends_on_version_and_channels);
  RUN_TEST(test_every_sample_rate);
  RUN_TEST(test_ordinary_audio_frame_has_no_length);
  RUN_TEST(test_tag_without_a_frame_count);
  RUN_TEST(test_zero_frames_is_unknown);
  RUN_TEST(test_not_a_frame);
  RUN_TEST(test_short_input_reads_nothing_beyond_it);
  RUN_TEST(test_huge_count_does_not_overflow);
  return UNITY_END();
}
