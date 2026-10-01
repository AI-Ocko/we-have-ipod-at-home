#pragma once

#include <stddef.h>
#include <stdint.h>

// The sample path between the decoder and the DAC: format conversion, the
// volume curve and gain with ramps. Integer arithmetic only; every loop here
// runs once per sample played.

// How the decoder hands over its samples. All are signed, little-endian,
// interleaved by channel.
typedef enum {
  PCM_FMT_S16,        // 2 bytes per sample
  PCM_FMT_S24_PACKED, // 3 bytes per sample
  PCM_FMT_S24_IN_S32, // 4 bytes per sample, value in the low 24 bits
  PCM_FMT_S32,        // 4 bytes per sample, full scale
} pcm_format_t;

// 0 for a value that is not a pcm_format_t.
size_t pcm_bytes_per_sample(pcm_format_t format);

// Convert to interleaved 32-bit stereo, left-justified (16-bit input is
// shifted up 16 bits, 24-bit input 8 bits). Mono input is copied to both
// channels. `channels` is 1 or 2. Writes at most `out_frames_max` frames and
// returns how many it wrote. Input bytes beyond the last whole frame are
// ignored.
//
// Any other `channels` or an unknown format writes nothing and returns 0.
// `in` needs no alignment. `in` and `out` must not overlap.
size_t pcm_to_s32_stereo(const uint8_t *in, size_t in_bytes,
                         pcm_format_t format, int channels, int32_t *out,
                         size_t out_frames_max);

#define PCM_VOLUME_MAX 20
#define PCM_GAIN_UNITY 32768

// Gain for a volume level as a Q15 multiplier. Level 20 is unity, each step
// down is 2 dB, level 0 is silence. Levels outside 0..20 are clamped.
int32_t pcm_volume_gain(int level);

// Scale `frames` stereo frames in place. The gain moves in a straight line
// from `gain_from` at the first frame towards `gain_to`, reaching it at the
// frame after the last one, so consecutive blocks join without a step. Equal
// gains give a constant gain. Both channels of a frame get the same gain.
//
// Gains are clamped to 0..PCM_GAIN_UNITY: this attenuates, it never
// amplifies, so the output cannot overflow. Each product is rounded down
// (towards minus infinity), which is what an arithmetic shift gives for free;
// the error is under one unit of the 32-bit output. During a ramp the gain of
// each frame is the nearest Q15 value to the straight line.
void pcm_apply_gain(int32_t *samples, size_t frames, int32_t gain_from,
                    int32_t gain_to);
