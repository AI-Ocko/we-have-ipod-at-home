#include "audio_out.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static i2s_chan_handle_t s_tx;
static audio_out_pins_t s_pins;
static bool s_have_pins;
static bool s_running;

void audio_out_init(const audio_out_pins_t *pins) {
  s_pins = *pins;
  s_have_pins = true;
}

esp_err_t audio_out_start(uint32_t sample_rate, int channels, int bits) {
  if (!s_have_pins || s_running) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_data_bit_width_t width = (bits == 24)   ? I2S_DATA_BIT_WIDTH_24BIT
                               : (bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT
                                              : I2S_DATA_BIT_WIDTH_16BIT;
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      width, (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);

  esp_err_t err;
  if (!s_tx) {
    // The channel is created once and kept. Its pins stay driven between
    // tracks, which keeps the DAC's inputs from floating.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // The default (6 x 240 frames) is only ~32ms of audio — about as long as
    // one SD card read, so the buffer runs dry between reads and every gap
    // clicks. 8 x 1000 frames gives ~180ms to coast on. 1000 is near the
    // per-descriptor ceiling: a descriptor holds at most 4092 bytes, and a
    // 16-bit stereo frame is 4 bytes.
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1000;
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
      s_tx = NULL;
      return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
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
      return err;
    }
  } else {
    // Already created by an earlier start: retune it for this stream.
    err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
    if (err != ESP_OK) {
      return err;
    }
    err = i2s_channel_reconfig_std_slot(s_tx, &slot_cfg);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = i2s_channel_enable(s_tx);
  if (err != ESP_OK) {
    return err;
  }
  s_running = true;
  return ESP_OK;
}

esp_err_t audio_out_write(const void *data, size_t len) {
  if (!s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  size_t written;
  return i2s_channel_write(s_tx, data, len, &written, portMAX_DELAY);
}

// Stop cleanly. Leaving the channel enabled with nothing left to send makes
// the DMA replay stale buffers, which is audible as a continuous buzz.
void audio_out_stop(void) {
  if (!s_running) {
    return;
  }
  static const int16_t silence[256 * 2] = {0};
  size_t written;
  for (int i = 0; i < 4; i++) {
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // let the last buffers drain
  i2s_channel_disable(s_tx);
  s_running = false;
}
