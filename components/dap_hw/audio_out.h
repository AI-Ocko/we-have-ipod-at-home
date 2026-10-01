#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// I2S output to the PCM5102A DAC. The link always carries 32-bit stereo
// frames in 32-bit slots, whatever the source file holds: the caller converts
// (see pcm_to_s32_stereo) and only the sample rate changes between tracks.

#define AUDIO_OUT_RATE_MIN 8000
#define AUDIO_OUT_RATE_MAX 96000

// The link's DMA ring: AUDIO_OUT_BLOCKS buffers of AUDIO_OUT_BLOCK_FRAMES
// frames each. The DMA goes round the ring for ever, and a write fills the
// buffer that was sent longest ago, so a frame written now leaves for the DAC
// up to one whole ring (AUDIO_OUT_BLOCKS x AUDIO_OUT_BLOCK_FRAMES frames)
// later. Two things follow for the caller:
//
// - Before the link is stopped at the end of a track, write one ring's worth
//   of silence after the last real frame. A write waits for buffers to be
//   sent, so once that silence has been accepted every real frame has gone.
// - Writing whole blocks (a multiple of AUDIO_OUT_BLOCK_FRAMES frames at a
//   time) keeps each write lined up with one DMA buffer, so no buffer is ever
//   left half filled while the writer waits for more data.
#define AUDIO_OUT_BLOCK_FRAMES 500
#define AUDIO_OUT_BLOCKS 16

typedef struct {
  int bck;  // bit clock
  int lrck; // left/right (word select) clock
  int dout; // data, to the DAC's DIN
} audio_out_pins_t;

// Record which pins to use. Call once before anything else.
void audio_out_init(const audio_out_pins_t *pins);

// Start output at `sample_rate` (8000..96000). The format is always 32-bit
// stereo. Returns ESP_ERR_NOT_SUPPORTED for a rate outside the range and
// ESP_ERR_INVALID_STATE if output is already running; call audio_out_stop()
// first. After any failure, a later start can still succeed.
esp_err_t audio_out_start(uint32_t sample_rate);

// Queue `frames` stereo frames (2 * frames int32_t values). Blocks until all
// are queued.
esp_err_t audio_out_write(const int32_t *samples, size_t frames);

// Shut the link down: a little silence, a short wait, then the clocks stop.
// Frames still waiting in the DMA ring are NOT played (see above for how to
// let them out first). Does nothing if output is not running.
void audio_out_stop(void);
