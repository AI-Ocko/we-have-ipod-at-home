#include "audio_out.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#define FRAME_BYTES (2 * sizeof(int32_t)) // one stereo frame on the wire

static i2s_chan_handle_t s_tx;
static audio_out_pins_t s_pins;
static bool s_have_pins;
static bool s_running;

void audio_out_init(const audio_out_pins_t *pins) {
  s_pins = *pins;
  s_have_pins = true;
}

// Create the channel at `clk_cfg`'s rate. Leaves s_tx NULL on failure.
static esp_err_t create_channel(const i2s_std_clk_config_t *clk_cfg) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // The default (6 x 240 frames) is only ~32ms of audio — about as long as
  // one SD card read, so the buffer runs dry between reads and every gap
  // clicks. 16 x 500 frames gives ~180ms to coast on at 44.1kHz (~83ms at
  // 96kHz). 500 is near the per-descriptor ceiling: a descriptor holds at
  // most 4092 bytes, and a 32-bit stereo frame is 8 bytes, so 511 frames is
  // the most one can carry. The buffers cost 16 x 4000 = 64000 bytes of
  // internal DMA-capable RAM.
  chan_cfg.dma_desc_num = AUDIO_OUT_BLOCKS;
  chan_cfg.dma_frame_num = AUDIO_OUT_BLOCK_FRAMES;
  // If the writer falls behind, send silence. Without this the DMA goes
  // round its ring again and replays stale buffers. The driver zeroes each
  // buffer once it has been sent (`auto_clear` is this field's older name).
  chan_cfg.auto_clear_after_cb = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
  if (err != ESP_OK) {
    s_tx = NULL;
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = *clk_cfg,
      // 32-bit data in 32-bit slots: the PCM5102A takes the top 24 bits of
      // each slot and ignores the rest.
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED, // PCM5102A makes its own clock
              .bclk = (gpio_num_t)s_pins.bck,
              .ws = (gpio_num_t)s_pins.lrck,
              .dout = (gpio_num_t)s_pins.dout,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {0},
          },
  };
  err = i2s_channel_init_std_mode(s_tx, &std_cfg);
  if (err != ESP_OK) {
    i2s_del_channel(s_tx);
    s_tx = NULL;
  }
  return err;
}

esp_err_t audio_out_start(uint32_t sample_rate) {
  if (!s_have_pins || s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  if (sample_rate < AUDIO_OUT_RATE_MIN || sample_rate > AUDIO_OUT_RATE_MAX) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  esp_err_t err;
  if (!s_tx) {
    // The channel is created once and kept. Its pins stay driven between
    // tracks, which keeps the DAC's inputs from floating.
    err = create_channel(&clk_cfg);
    if (err != ESP_OK) {
      return err;
    }
  } else {
    // Already created by an earlier start. The slot format never changes, so
    // only the clock needs retuning.
    err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
  }
  if (err == ESP_OK) {
    err = i2s_channel_enable(s_tx);
  }
  if (err != ESP_OK) {
    // Do not trust a channel that refused a setting: a driver call that fails
    // part-way can leave its stored configuration half changed, and then
    // every later start fails too. Throw it away; the next start builds a
    // new one.
    i2s_del_channel(s_tx);
    s_tx = NULL;
    return err;
  }
  s_running = true;
  return ESP_OK;
}

esp_err_t audio_out_write(const int32_t *samples, size_t frames) {
  if (!s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  if (frames == 0) {
    return ESP_OK;
  }
  size_t written;
  return i2s_channel_write(s_tx, samples, frames * FRAME_BYTES, &written,
                           portMAX_DELAY);
}

// Stop cleanly: end on silence so the DAC is not left holding the last
// sample, then disable the channel so nothing more is clocked out. This does
// not wait for the DMA ring to empty: a caller that wants the last frames
// heard writes a ring of silence first (see audio_out.h).
void audio_out_stop(void) {
  if (!s_running) {
    return;
  }
  static const int32_t silence[256 * 2] = {0};
  size_t written;
  for (int i = 0; i < 4; i++) {
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // let the last buffers drain
  i2s_channel_disable(s_tx);
  s_running = false;
}
