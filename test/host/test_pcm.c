#include "pcm.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#define UNITY_GAIN PCM_GAIN_UNITY
#define SENTINEL 0x5A5A5A5A

// The longest block the player passes to pcm_apply_gain.
#define LONG_FRAMES 8192

static int32_t out[64];
static int32_t big[LONG_FRAMES * 2];

void setUp(void) {
  for (size_t i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
    out[i] = SENTINEL;
  }
}
void tearDown(void) {}

// What pcm_apply_gain must produce for one sample: the product rounded down.
// Written with division so it does not lean on how `>>` treats negatives.
static int32_t scaled(int32_t sample, int32_t gain) {
  int64_t p = (int64_t)sample * gain;
  int64_t q = p / 32768;
  if (p % 32768 < 0) {
    q--;
  }
  return (int32_t)q;
}

// Fill `frames` stereo frames of `big` with one value.
static void fill_big(size_t frames, int32_t value) {
  for (size_t i = 0; i < frames * 2; i++) {
    big[i] = value;
  }
}

// ---- conversion -------------------------------------------------------------

static void test_bytes_per_sample(void) {
  TEST_ASSERT_EQUAL_size_t(2, pcm_bytes_per_sample(PCM_FMT_S16));
  TEST_ASSERT_EQUAL_size_t(3, pcm_bytes_per_sample(PCM_FMT_S24_PACKED));
  TEST_ASSERT_EQUAL_size_t(4, pcm_bytes_per_sample(PCM_FMT_S24_IN_S32));
  TEST_ASSERT_EQUAL_size_t(4, pcm_bytes_per_sample(PCM_FMT_S32));
}

static void test_s16_stereo(void) {
  const uint8_t in[] = {0x01, 0x00, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S16, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x00010000, out[0]);
  TEST_ASSERT_EQUAL_HEX32((int32_t)0xFFFF0000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

static void test_s16_mono_is_copied_to_both_channels(void) {
  const uint8_t in[] = {0x34, 0x12};
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S16, 1, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12340000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x12340000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

static void test_s16_extremes(void) {
  const uint8_t in[] = {0x00, 0x80, 0xFF, 0x7F}; // -32768, 32767
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S16, 2, out, 8));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x7FFF0000, out[1]);
}

static void test_s24_packed(void) {
  const uint8_t in[] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x80};
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S24_PACKED, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[0]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

static void test_s24_in_s32(void) {
  // 0x00123456 and 0xFF800000, little-endian.
  const uint8_t in[] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x80, 0xFF};
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S24_IN_S32, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[0]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

static void test_s32_passes_through(void) {
  const uint8_t in[] = {
      0x00, 0x00, 0x00, 0x80, // INT32_MIN
      0xFF, 0xFF, 0xFF, 0x7F, // INT32_MAX
      0x78, 0x56, 0x34, 0x12, // 0x12345678
      0xFF, 0xFF, 0xFF, 0xFF, // -1
  };
  TEST_ASSERT_EQUAL_size_t(
      2, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S32, 2, out, 8));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[0]);
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, out[1]);
  TEST_ASSERT_EQUAL_HEX32(0x12345678, out[2]);
  TEST_ASSERT_EQUAL_INT32(-1, out[3]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[4]);
}

static void test_partial_trailing_frame_is_ignored(void) {
  const uint8_t in[7] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04};
  TEST_ASSERT_EQUAL_size_t(1, pcm_to_s32_stereo(in, 7, PCM_FMT_S16, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x00010000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x00020000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[3]);
}

static void test_output_limit_is_respected(void) {
  uint8_t in[10 * 4];
  for (size_t i = 0; i < sizeof(in); i++) {
    in[i] = (uint8_t)(i + 1);
  }
  TEST_ASSERT_EQUAL_size_t(
      4, pcm_to_s32_stereo(in, sizeof(in), PCM_FMT_S16, 2, out, 4));
  TEST_ASSERT_EQUAL_HEX32(0x02010000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x100F0000, out[7]); // right sample of frame 4
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);   // fifth frame untouched
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[9]);
}

static void test_zero_input(void) {
  const uint8_t in[1] = {0};
  TEST_ASSERT_EQUAL_size_t(0, pcm_to_s32_stereo(in, 0, PCM_FMT_S16, 2, out, 8));
  TEST_ASSERT_EQUAL_size_t(
      0, pcm_to_s32_stereo(in, 0, PCM_FMT_S24_PACKED, 1, out, 8));
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[0]);
}

// Extra: every format has its own mono loop.
static void test_mono_is_copied_for_every_format(void) {
  const uint8_t s24[] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x80};
  TEST_ASSERT_EQUAL_size_t(
      2, pcm_to_s32_stereo(s24, sizeof(s24), PCM_FMT_S24_PACKED, 1, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[1]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[2]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[3]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[4]);

  const uint8_t s24in32[] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x80, 0xFF};
  setUp();
  TEST_ASSERT_EQUAL_size_t(2, pcm_to_s32_stereo(s24in32, sizeof(s24in32),
                                                PCM_FMT_S24_IN_S32, 1, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x12345600, out[1]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[2]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[3]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[4]);

  const uint8_t s32[] = {0x78, 0x56, 0x34, 0x12, 0xFF, 0xFF, 0xFF, 0x7F};
  setUp();
  TEST_ASSERT_EQUAL_size_t(
      2, pcm_to_s32_stereo(s32, sizeof(s32), PCM_FMT_S32, 1, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x12345678, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x12345678, out[1]);
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, out[2]);
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, out[3]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[4]);
}

// Extra: several frames keep their order and their channels, read from an
// address that is not a multiple of 2 or 4 (the decoder's buffer offset is
// arbitrary and the ESP32 traps on unaligned word loads).
static void test_frames_keep_their_order_at_any_alignment(void) {
  static const uint8_t buf[1 + 12] = {0xEE, 0x01, 0x00, 0x10, 0x02, 0x00, 0x20,
                                      0x03, 0x00, 0x30, 0x04, 0x00, 0xC0};
  TEST_ASSERT_EQUAL_size_t(
      2, pcm_to_s32_stereo(buf + 1, 12, PCM_FMT_S24_PACKED, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x10000100, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x20000200, out[1]);
  TEST_ASSERT_EQUAL_HEX32(0x30000300, out[2]);
  TEST_ASSERT_EQUAL_HEX32((int32_t)0xC0000400, out[3]);

  setUp();
  TEST_ASSERT_EQUAL_size_t(
      3, pcm_to_s32_stereo(buf + 1, 12, PCM_FMT_S16, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x00010000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x02100000, out[1]);
  TEST_ASSERT_EQUAL_HEX32((int32_t)0xC0000000, out[5]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[6]);

  setUp();
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(buf + 1, 12, PCM_FMT_S32, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(0x02100001, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x00032000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

// Extra: the partial-frame and output-limit rules for the other frame sizes.
static void test_partial_frame_and_limit_for_other_formats(void) {
  uint8_t in[32];
  memset(in, 0x11, sizeof(in));
  // 11 bytes of 24-bit stereo: one 6-byte frame and 5 spare bytes.
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, 11, PCM_FMT_S24_PACKED, 2, out, 8));
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
  // 5 bytes of 24-bit mono: one frame.
  TEST_ASSERT_EQUAL_size_t(
      1, pcm_to_s32_stereo(in, 5, PCM_FMT_S24_PACKED, 1, out, 8));
  // 15 bytes of 32-bit stereo: one frame; of 32-bit mono: three.
  TEST_ASSERT_EQUAL_size_t(1,
                           pcm_to_s32_stereo(in, 15, PCM_FMT_S32, 2, out, 8));
  TEST_ASSERT_EQUAL_size_t(
      3, pcm_to_s32_stereo(in, 15, PCM_FMT_S24_IN_S32, 1, out, 8));
  // Less than one frame.
  TEST_ASSERT_EQUAL_size_t(
      0, pcm_to_s32_stereo(in, 5, PCM_FMT_S24_PACKED, 2, out, 8));
  // Mono, limited by the output.
  setUp();
  TEST_ASSERT_EQUAL_size_t(2,
                           pcm_to_s32_stereo(in, 32, PCM_FMT_S16, 1, out, 2));
  TEST_ASSERT_EQUAL_HEX32(0x11110000, out[3]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[4]);
  // No room at all.
  setUp();
  TEST_ASSERT_EQUAL_size_t(0,
                           pcm_to_s32_stereo(in, 32, PCM_FMT_S16, 2, out, 0));
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[0]);
}

// Extra: input the interface does not allow is refused, not guessed at.
static void test_bad_channels_or_format_writes_nothing(void) {
  const uint8_t in[24] = {0};
  TEST_ASSERT_EQUAL_size_t(0,
                           pcm_to_s32_stereo(in, 24, PCM_FMT_S16, 0, out, 8));
  TEST_ASSERT_EQUAL_size_t(0,
                           pcm_to_s32_stereo(in, 24, PCM_FMT_S16, 3, out, 8));
  TEST_ASSERT_EQUAL_size_t(0,
                           pcm_to_s32_stereo(in, 24, PCM_FMT_S16, -1, out, 8));
  TEST_ASSERT_EQUAL_size_t(
      0, pcm_to_s32_stereo(in, 24, (pcm_format_t)99, 2, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, pcm_bytes_per_sample((pcm_format_t)99));
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[0]);
}

// ---- volume -----------------------------------------------------------------

static void test_volume_table(void) {
  static const int32_t want[PCM_VOLUME_MAX + 1] = {
      0,    413,  519,  654,  823,   1036,  1305,  1642,  2068,  2603, 3277,
      4125, 5193, 6538, 8231, 10362, 13045, 16423, 20675, 26029, 32768};
  for (int level = 0; level <= PCM_VOLUME_MAX; level++) {
    TEST_ASSERT_EQUAL_INT32(want[level], pcm_volume_gain(level));
  }
}

static void test_volume_is_clamped(void) {
  TEST_ASSERT_EQUAL_INT32(0, pcm_volume_gain(-5));
  TEST_ASSERT_EQUAL_INT32(32768, pcm_volume_gain(99));
  TEST_ASSERT_EQUAL_INT32(0, pcm_volume_gain(-1));
  TEST_ASSERT_EQUAL_INT32(32768, pcm_volume_gain(21));
}

// ---- gain -------------------------------------------------------------------

static void test_unity_gain_changes_nothing(void) {
  const int32_t in[8] = {INT32_MIN, INT32_MAX,  0,           -1,
                         1,         0x12345678, -0x12345678, 0x00010000};
  memcpy(out, in, sizeof(in));
  pcm_apply_gain(out, 4, UNITY_GAIN, UNITY_GAIN);
  TEST_ASSERT_EQUAL_INT32_ARRAY(in, out, 8);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);
}

static void test_zero_gain_gives_exact_silence(void) {
  const int32_t in[8] = {INT32_MIN, INT32_MAX,  0,           -1,
                         1,         0x12345678, -0x12345678, 0x00010000};
  memcpy(out, in, sizeof(in));
  pcm_apply_gain(out, 4, 0, 0);
  TEST_ASSERT_EACH_EQUAL_INT32(0, out, 8);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);
}

static void test_half_gain(void) {
  out[0] = 0x40000000;
  out[1] = INT32_MIN;
  pcm_apply_gain(out, 1, 16384, 16384);
  TEST_ASSERT_EQUAL_HEX32(0x20000000, out[0]);
  TEST_ASSERT_EQUAL_INT32(-0x40000000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);
}

static void test_ramp_starts_at_from_and_approaches_to(void) {
  const int32_t x = 0x40000000;
  for (int i = 0; i < 8; i++) {
    out[i] = x;
  }
  pcm_apply_gain(out, 4, 0, UNITY_GAIN);
  // Frame f should be x * f/4; "1 part in 4096" of full gain is x / 4096.
  for (int f = 0; f < 4; f++) {
    TEST_ASSERT_INT32_WITHIN(x / 4096, (x / 4) * f, out[2 * f]);
    TEST_ASSERT_INT32_WITHIN(x / 4096, (x / 4) * f, out[2 * f + 1]);
  }
  TEST_ASSERT_EQUAL_INT32(0, out[0]); // starts exactly at `from`
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);
}

static void test_ramps_join_without_a_step(void) {
  const int32_t x = 0x30000000;
  fill_big(800, x);
  pcm_apply_gain(big, 400, 0, 16384);
  pcm_apply_gain(big + 800, 400, 16384, UNITY_GAIN);

  int64_t smallest = INT64_MAX, largest = 0;
  for (size_t f = 1; f < 800; f++) {
    int64_t jump = (int64_t)big[2 * f] - big[2 * (f - 1)];
    TEST_ASSERT_TRUE_MESSAGE(jump >= 0, "the output decreased");
    if (jump > largest) {
      largest = jump;
    }
    if (jump > 0 && jump < smallest) {
      smallest = jump;
    }
  }
  TEST_ASSERT_TRUE(largest > 0);
  TEST_ASSERT_TRUE_MESSAGE(largest <= 2 * smallest, "uneven steps");
  // The second block starts exactly where the first was heading, and the
  // whole thing ends one step short of unity.
  TEST_ASSERT_EQUAL_INT32(x / 2, big[2 * 400]);
  TEST_ASSERT_TRUE(big[2 * 799] < x);
  TEST_ASSERT_TRUE((int64_t)x - big[2 * 799] <= largest);
}

static void test_both_channels_get_the_same_gain(void) {
  for (int f = 0; f < 32; f++) {
    out[2 * f] = out[2 * f + 1] = 0x7FFF0000 - f * 0x01234567;
  }
  pcm_apply_gain(out, 32, 300, 30000);
  for (int f = 0; f < 32; f++) {
    TEST_ASSERT_EQUAL_INT32(out[2 * f], out[2 * f + 1]);
  }
  // And the gain did change along the way.
  TEST_ASSERT_TRUE(out[0] < 0x02000000);
  TEST_ASSERT_TRUE(out[62] > 0x30000000);
}

static void test_full_scale_does_not_overflow_under_ramp(void) {
  static const size_t lengths[] = {1, 2, 8, 400};
  for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
    size_t frames = lengths[n];
    for (int pass = 0; pass < 2; pass++) {
      for (size_t f = 0; f < frames; f++) {
        big[2 * f] = INT32_MIN;
        big[2 * f + 1] = INT32_MAX;
      }
      pcm_apply_gain(big, frames, pass == 0 ? UNITY_GAIN : 32760, UNITY_GAIN);
      for (size_t f = 0; f < frames; f++) {
        TEST_ASSERT_TRUE_MESSAGE(big[2 * f] < 0, "INT32_MIN wrapped");
        TEST_ASSERT_TRUE_MESSAGE(big[2 * f + 1] > 0, "INT32_MAX wrapped");
        // Nearly full gain, so still nearly full scale.
        TEST_ASSERT_TRUE(big[2 * f] <= INT32_MIN + 0x00100000);
        TEST_ASSERT_TRUE(big[2 * f + 1] >= INT32_MAX - 0x00100000);
      }
    }
  }
}

// Extra: pins the rounding. Products are rounded down, for either sign.
static void test_products_are_rounded_down(void) {
  out[0] = 1;  // 0.5    -> 0
  out[1] = -1; // -0.5   -> -1
  out[2] = 3;  // 1.5    -> 1
  out[3] = -3; // -1.5   -> -2
  out[4] = 2;  // exact
  out[5] = -2; // exact
  pcm_apply_gain(out, 3, 16384, 16384);
  TEST_ASSERT_EQUAL_INT32(0, out[0]);
  TEST_ASSERT_EQUAL_INT32(-1, out[1]);
  TEST_ASSERT_EQUAL_INT32(1, out[2]);
  TEST_ASSERT_EQUAL_INT32(-2, out[3]);
  TEST_ASSERT_EQUAL_INT32(1, out[4]);
  TEST_ASSERT_EQUAL_INT32(-1, out[5]);
}

// Extra: every volume level, as a constant gain, against the reference.
static void test_every_volume_level_scales_exactly(void) {
  const int32_t in[8] = {INT32_MIN,   INT32_MAX,          0, -1, 1, 0x12345678,
                         -0x12345678, (int32_t)0xFFFF0000};
  for (int level = 0; level <= PCM_VOLUME_MAX; level++) {
    int32_t g = pcm_volume_gain(level);
    memcpy(out, in, sizeof(in));
    pcm_apply_gain(out, 4, g, g);
    for (int i = 0; i < 8; i++) {
      TEST_ASSERT_EQUAL_INT32(scaled(in[i], g), out[i]);
    }
    TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);
  }
}

// Extra: every constant gain there is, on the samples most likely to expose
// a carry or sign mistake. Below and above half gain are separate loops.
static void test_every_constant_gain_scales_exactly(void) {
  const int32_t in[12] = {
      INT32_MIN,  INT32_MAX,     0,           -1,
      1,          0x12345678,    -0x12345678, (int32_t)0xFFFF0000,
      0x7FFF0000, INT32_MIN + 1, 0x00008000,  -0x00008000};
  for (int32_t g = 0; g <= UNITY_GAIN; g++) {
    memcpy(out, in, sizeof(in));
    pcm_apply_gain(out, 6, g, g);
    for (int i = 0; i < 12; i++) {
      if (out[i] != scaled(in[i], g)) {
        char where[48];
        snprintf(where, sizeof(where), "gain %d, sample %d", (int)g,
                 (int)in[i]);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(scaled(in[i], g), out[i], where);
      }
    }
  }
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[12]);
}

// Extra: a ramp and a constant gain round the same way, so a block that ends
// a ramp and the block after it agree. Each frame of a ramp must equal the
// reference at the gain that frame got.
static void test_ramp_frames_scale_exactly(void) {
  const int32_t l = -0x12345679, r = 0x7FFFFFFF;
  enum { FRAMES = 16 };
  for (int f = 0; f < FRAMES; f++) {
    out[2 * f] = l;
    out[2 * f + 1] = r;
  }
  pcm_apply_gain(out, FRAMES, UNITY_GAIN, 0); // gains 32768, 30720, ... 2048
  for (int f = 0; f < FRAMES; f++) {
    int32_t g = UNITY_GAIN - f * (UNITY_GAIN / FRAMES);
    TEST_ASSERT_EQUAL_INT32(scaled(l, g), out[2 * f]);
    TEST_ASSERT_EQUAL_INT32(scaled(r, g), out[2 * f + 1]);
  }
}

// Extra: nothing to do, nothing touched (and no division by zero).
static void test_zero_frames_touches_nothing(void) {
  pcm_apply_gain(out, 0, 0, UNITY_GAIN);
  pcm_apply_gain(out, 0, 0, 0);
  pcm_apply_gain(out, 0, 1000, 1000);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[0]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[1]);
}

// Extra: a one-frame ramp is that frame at `from`.
static void test_one_frame_ramp_uses_from(void) {
  out[0] = 0x40000000;
  out[1] = -0x40000000;
  pcm_apply_gain(out, 1, 8192, UNITY_GAIN);
  TEST_ASSERT_EQUAL_HEX32(0x10000000, out[0]);
  TEST_ASSERT_EQUAL_INT32(-0x10000000, out[1]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[2]);

  out[0] = 0x40000000;
  out[1] = -0x40000000;
  pcm_apply_gain(out, 1, UNITY_GAIN, 0); // the steepest step there is
  TEST_ASSERT_EQUAL_HEX32(0x40000000, out[0]);
  TEST_ASSERT_EQUAL_INT32(-0x40000000, out[1]);
}

// Extra: a fade-out is as exact as a fade-in.
static void test_falling_ramp_mirrors_the_rising_one(void) {
  const int32_t x = 0x40000000;
  for (int i = 0; i < 8; i++) {
    out[i] = x;
  }
  pcm_apply_gain(out, 4, UNITY_GAIN, 0);
  TEST_ASSERT_EQUAL_HEX32(0x40000000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x30000000, out[2]);
  TEST_ASSERT_EQUAL_HEX32(0x20000000, out[4]);
  TEST_ASSERT_EQUAL_HEX32(0x10000000, out[6]);
  TEST_ASSERT_EQUAL_HEX32(0x10000000, out[7]);
  TEST_ASSERT_EQUAL_HEX32(SENTINEL, out[8]);
}

// With an input of 2^30 the output is gain * 2^15 exactly, so the gain each
// frame got can be read back. It must be the nearest Q15 value to the straight
// line: |g * frames - (from * frames + (to - from) * f)| <= frames / 2, with a
// little slack for the fixed-point step (under frames / 4 at 8192 frames).
static void check_ramp_is_on_the_line(size_t frames, int32_t from, int32_t to) {
  fill_big(frames, 0x40000000);
  pcm_apply_gain(big, frames, from, to);
  for (size_t f = 0; f < frames; f++) {
    TEST_ASSERT_EQUAL_INT32(big[2 * f], big[2 * f + 1]);
    TEST_ASSERT_EQUAL_INT32(0, big[2 * f] & 0x7FFF);
    int64_t g = big[2 * f] >> 15;
    int64_t line =
        (int64_t)from * (int64_t)frames + ((int64_t)to - from) * (int64_t)f;
    int64_t err = g * (int64_t)frames - line;
    if (err < 0) {
      err = -err;
    }
    TEST_ASSERT_TRUE_MESSAGE(err * 4 <= (int64_t)frames * 3, "off the line");
    // Never outside the two end gains.
    TEST_ASSERT_TRUE(g >= (from < to ? from : to));
    TEST_ASSERT_TRUE(g <= (from < to ? to : from));
  }
  TEST_ASSERT_EQUAL_INT32((int64_t)from << 15, big[0]);
}

// Extra: the longest block and the largest gain difference, both directions,
// plus lengths that do not divide the difference evenly.
static void test_long_and_awkward_ramps_stay_on_the_line(void) {
  check_ramp_is_on_the_line(LONG_FRAMES, 0, UNITY_GAIN);
  check_ramp_is_on_the_line(LONG_FRAMES, UNITY_GAIN, 0);
  check_ramp_is_on_the_line(LONG_FRAMES, 3277, 3278);
  check_ramp_is_on_the_line(LONG_FRAMES, 3278, 3277);
  check_ramp_is_on_the_line(400, 0, UNITY_GAIN);
  check_ramp_is_on_the_line(400, UNITY_GAIN, 0);
  check_ramp_is_on_the_line(400, 3277, 0);
  check_ramp_is_on_the_line(3, 0, UNITY_GAIN);
  check_ramp_is_on_the_line(3, UNITY_GAIN, 0);
  check_ramp_is_on_the_line(7, 1, 2);
  check_ramp_is_on_the_line(4801, 26029, 413);
  check_ramp_is_on_the_line(2, UNITY_GAIN, 0);
}

// Extra: this function only attenuates. A gain outside 0..unity is clamped,
// so a caller's mistake cannot wrap the output round.
static void test_out_of_range_gains_are_clamped(void) {
  out[0] = INT32_MAX;
  out[1] = INT32_MIN;
  pcm_apply_gain(out, 1, 70000, 70000);
  TEST_ASSERT_EQUAL_INT32(INT32_MAX, out[0]);
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, out[1]);

  pcm_apply_gain(out, 1, -5, -5);
  TEST_ASSERT_EQUAL_INT32(0, out[0]);
  TEST_ASSERT_EQUAL_INT32(0, out[1]);

  for (int i = 0; i < 8; i++) {
    out[i] = 0x40000000;
  }
  pcm_apply_gain(out, 4, -32768, 65536); // behaves as 0 to unity
  TEST_ASSERT_EQUAL_HEX32(0x00000000, out[0]);
  TEST_ASSERT_EQUAL_HEX32(0x10000000, out[2]);
  TEST_ASSERT_EQUAL_HEX32(0x20000000, out[4]);
  TEST_ASSERT_EQUAL_HEX32(0x30000000, out[6]);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_bytes_per_sample);
  RUN_TEST(test_s16_stereo);
  RUN_TEST(test_s16_mono_is_copied_to_both_channels);
  RUN_TEST(test_s16_extremes);
  RUN_TEST(test_s24_packed);
  RUN_TEST(test_s24_in_s32);
  RUN_TEST(test_s32_passes_through);
  RUN_TEST(test_partial_trailing_frame_is_ignored);
  RUN_TEST(test_output_limit_is_respected);
  RUN_TEST(test_zero_input);
  RUN_TEST(test_mono_is_copied_for_every_format);
  RUN_TEST(test_frames_keep_their_order_at_any_alignment);
  RUN_TEST(test_partial_frame_and_limit_for_other_formats);
  RUN_TEST(test_bad_channels_or_format_writes_nothing);
  RUN_TEST(test_volume_table);
  RUN_TEST(test_volume_is_clamped);
  RUN_TEST(test_unity_gain_changes_nothing);
  RUN_TEST(test_zero_gain_gives_exact_silence);
  RUN_TEST(test_half_gain);
  RUN_TEST(test_ramp_starts_at_from_and_approaches_to);
  RUN_TEST(test_ramps_join_without_a_step);
  RUN_TEST(test_both_channels_get_the_same_gain);
  RUN_TEST(test_full_scale_does_not_overflow_under_ramp);
  RUN_TEST(test_products_are_rounded_down);
  RUN_TEST(test_every_volume_level_scales_exactly);
  RUN_TEST(test_every_constant_gain_scales_exactly);
  RUN_TEST(test_ramp_frames_scale_exactly);
  RUN_TEST(test_zero_frames_touches_nothing);
  RUN_TEST(test_one_frame_ramp_uses_from);
  RUN_TEST(test_falling_ramp_mirrors_the_rising_one);
  RUN_TEST(test_long_and_awkward_ramps_stay_on_the_line);
  RUN_TEST(test_out_of_range_gains_are_clamped);
  return UNITY_END();
}
