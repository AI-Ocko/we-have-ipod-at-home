#include "pcm.h"

#include <string.h>

// Two things here are left to the compiler by the C standard, and both are
// what every compiler this builds with does (GCC documents them): converting
// an out-of-range unsigned value to a signed type wraps, and `>>` on a
// negative value copies the sign bit down. Refuse to build anywhere else
// rather than produce noise.
_Static_assert((-1 >> 1) == -1, "pcm.c needs an arithmetic right shift");
_Static_assert((int32_t)0x80000000u == INT32_MIN,
               "pcm.c needs wrapping unsigned-to-signed conversion");

// ---- conversion -------------------------------------------------------------
//
// The decoder's bytes can start at any address and the ESP32 traps on an
// unaligned word load, so samples are put together from single bytes. Each
// byte is shifted straight to where it ends up in the left-justified result,
// in unsigned arithmetic: there is no separate sign-extension or scaling step
// and no shift of a negative value.

static inline int32_t load_s16(const uint8_t *p) {
  return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24));
}

// Also reads 24-in-32: the fourth byte only repeats the sign and is skipped.
static inline int32_t load_s24(const uint8_t *p) {
  return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 24));
}

static inline int32_t load_s32(const uint8_t *p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// One converter per input format, so the format is decided once per block and
// each loop body is only loads, shifts and stores. BYTES is a constant, which
// turns the division for the frame count into a shift or a multiply.
#define DEFINE_CONVERTER(NAME, LOAD, BYTES)                                    \
  static size_t NAME(const uint8_t *in, size_t in_bytes, int channels,         \
                     int32_t *out, size_t out_frames_max) {                    \
    if (channels == 2) {                                                       \
      size_t frames = in_bytes / (2 * (BYTES));                                \
      if (frames > out_frames_max) {                                           \
        frames = out_frames_max;                                               \
      }                                                                        \
      for (int32_t *end = out + 2 * frames; out != end; out += 2) {            \
        out[0] = LOAD(in);                                                     \
        out[1] = LOAD(in + (BYTES));                                           \
        in += 2 * (BYTES);                                                     \
      }                                                                        \
      return frames;                                                           \
    }                                                                          \
    size_t frames = in_bytes / (BYTES);                                        \
    if (frames > out_frames_max) {                                             \
      frames = out_frames_max;                                                 \
    }                                                                          \
    for (int32_t *end = out + 2 * frames; out != end; out += 2) {              \
      int32_t s = LOAD(in);                                                    \
      in += (BYTES);                                                           \
      out[0] = s;                                                              \
      out[1] = s;                                                              \
    }                                                                          \
    return frames;                                                             \
  }

DEFINE_CONVERTER(convert_s16, load_s16, 2)
DEFINE_CONVERTER(convert_s24_packed, load_s24, 3)
DEFINE_CONVERTER(convert_s24_in_s32, load_s24, 4)
DEFINE_CONVERTER(convert_s32, load_s32, 4)

size_t pcm_bytes_per_sample(pcm_format_t format) {
  switch (format) {
  case PCM_FMT_S16:
    return 2;
  case PCM_FMT_S24_PACKED:
    return 3;
  case PCM_FMT_S24_IN_S32:
  case PCM_FMT_S32:
    return 4;
  }
  return 0;
}

size_t pcm_to_s32_stereo(const uint8_t *in, size_t in_bytes,
                         pcm_format_t format, int channels, int32_t *out,
                         size_t out_frames_max) {
  if (channels != 1 && channels != 2) {
    return 0;
  }
  switch (format) {
  case PCM_FMT_S16:
    return convert_s16(in, in_bytes, channels, out, out_frames_max);
  case PCM_FMT_S24_PACKED:
    return convert_s24_packed(in, in_bytes, channels, out, out_frames_max);
  case PCM_FMT_S24_IN_S32:
    return convert_s24_in_s32(in, in_bytes, channels, out, out_frames_max);
  case PCM_FMT_S32:
    return convert_s32(in, in_bytes, channels, out, out_frames_max);
  }
  return 0;
}

// ---- volume -----------------------------------------------------------------

// round(32768 * 10^(-2 * (20 - level) / 20)), with level 0 forced to silence.
// Worked out ahead of time: there is no floating point in the firmware's
// audio path.
static const uint16_t VOLUME_GAIN[PCM_VOLUME_MAX + 1] = {
    0,    413,  519,  654,  823,   1036,  1305,  1642,  2068,  2603, 3277,
    4125, 5193, 6538, 8231, 10362, 13045, 16423, 20675, 26029, 32768};

int32_t pcm_volume_gain(int level) {
  if (level < 0) {
    level = 0;
  } else if (level > PCM_VOLUME_MAX) {
    level = PCM_VOLUME_MAX;
  }
  return VOLUME_GAIN[level];
}

// ---- gain -------------------------------------------------------------------
//
// Every result here is sample * gain / 32768 rounded down (towards minus
// infinity), computed exactly from the full 64-bit product. With the gain no
// more than PCM_GAIN_UNITY the result is never larger than the sample, so it
// always fits back into 32 bits.

// The general form, for a gain that changes from frame to frame. On the ESP32
// this is the two halves of the product (one instruction each) and a funnel
// shift.
static inline int32_t scale(int32_t sample, int32_t gain) {
  return (int32_t)(((int64_t)sample * gain) >> 15);
}

// The top half of a 32x32 signed product: one instruction on the ESP32.
static inline int32_t mul_high(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * b) >> 32);
}

static inline int32_t clamp_gain(int32_t gain) {
  if (gain < 0) {
    return 0;
  }
  return gain > PCM_GAIN_UNITY ? PCM_GAIN_UNITY : gain;
}

// A constant gain strictly between silence and unity: what runs for every
// sample at any volume setting below the top one.
//
// Moving the gain up 17 bits beforehand puts the answer in the top half of
// the product, so no shift is needed per sample: sample * (gain << 17) / 2^32
// is sample * gain / 2^15, and taking the top half rounds down. From half
// gain upwards gain << 17 no longer fits a signed 32-bit multiplier; it is
// used as the signed value that is 2^32 less, and adding the sample back
// makes up for it: sample * (m - 2^32) / 2^32 + sample = sample * m / 2^32.
// (sample * 2^32 is a multiple of 2^32, so the rounding is unaffected.)
static void scale_constant(int32_t *samples, int32_t *end, int32_t gain) {
  const int32_t m = (int32_t)((uint32_t)gain << 17);
  if (gain < PCM_GAIN_UNITY / 2) {
    for (; samples != end; samples += 2) {
      samples[0] = mul_high(samples[0], m);
      samples[1] = mul_high(samples[1], m);
    }
  } else {
    for (; samples != end; samples += 2) {
      int32_t l = samples[0];
      int32_t r = samples[1];
      samples[0] = mul_high(l, m) + l;
      samples[1] = mul_high(r, m) + r;
    }
  }
}

void pcm_apply_gain(int32_t *samples, size_t frames, int32_t gain_from,
                    int32_t gain_to) {
  gain_from = clamp_gain(gain_from);
  gain_to = clamp_gain(gain_to);
  if (frames == 0) {
    return;
  }
  int32_t *const end = samples + 2 * frames;

  if (gain_from == gain_to) {
    // Full volume costs nothing, and silence is exact and needs no multiply.
    if (gain_from == PCM_GAIN_UNITY) {
      return;
    }
    if (gain_from == 0) {
      memset(samples, 0, 2 * frames * sizeof(samples[0]));
      return;
    }
    scale_constant(samples, end, gain_from);
    return;
  }

  // A ramp. The gain is kept with 16 extra fraction bits and moved by a fixed
  // step each frame, so the one division happens here, once per block.
  //
  // Everything is unsigned so that a falling ramp is the same arithmetic as a
  // rising one: a negative step is its two's complement and the additions
  // wrap, which unsigned arithmetic defines. The values that matter never
  // wrap: unity is 2^31 in this scale and so is the largest possible step
  // (silence to unity in one frame), and both fit in 32 bits.
  //
  // The step is rounded towards zero, so the gain falls behind the true line
  // by up to frames / 65536 of one Q15 unit by the end of the block: an
  // eighth of a unit at 8192 frames. Starting half a unit up makes the
  // shift below round to the nearest unit instead of always down. The gain
  // stays between the two end gains, so it never exceeds unity.
  uint32_t distance = (uint32_t)(gain_to > gain_from ? gain_to - gain_from
                                                     : gain_from - gain_to);
  uint32_t step = (uint32_t)((distance << 16) / frames);
  if (gain_to < gain_from) {
    step = 0u - step;
  }
  uint32_t acc = ((uint32_t)gain_from << 16) + 0x8000u;
  for (; samples != end; samples += 2) {
    int32_t gain = (int32_t)(acc >> 16);
    acc += step;
    samples[0] = scale(samples[0], gain);
    samples[1] = scale(samples[1], gain);
  }
}
