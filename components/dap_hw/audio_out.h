#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// I2S output to the PCM5102A DAC. The link always carries 32-bit stereo
// frames in 32-bit slots, whatever the source file holds: the caller converts
// (see pcm_to_s32_stereo) and only the sample rate changes between tracks.

#define AUDIO_OUT_RATE_MIN 8000
#define AUDIO_OUT_RATE_MAX 96000

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

// Play out what is queued, then shut the link down. Does nothing if output is
// not running.
void audio_out_stop(void);
