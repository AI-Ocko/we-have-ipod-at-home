#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// I2S output to the PCM5102A DAC.

typedef struct {
  int bck;  // bit clock
  int lrck; // left/right (word select) clock
  int dout; // data, to the DAC's DIN
} audio_out_pins_t;

// Record which pins to use. Call once before anything else.
void audio_out_init(const audio_out_pins_t *pins);

// Configure the link for a stream and start it. `bits` is 16, 24 or 32;
// `channels` is 1 or 2. Returns ESP_ERR_INVALID_STATE if output is already
// running; call audio_out_stop() first.
esp_err_t audio_out_start(uint32_t sample_rate, int channels, int bits);

// Send PCM to the DAC. Blocks until all of it has been queued.
esp_err_t audio_out_write(const void *data, size_t len);

// Play out what is queued, then shut the link down. Does nothing if output is
// not running.
void audio_out_stop(void);
