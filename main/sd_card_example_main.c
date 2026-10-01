/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// This example uses SDMMC peripheral to communicate with SD card.

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sd_test_io.h"
#include "sdmmc_cmd.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include <dirent.h>
#include <inttypes.h>

#define EXAMPLE_MAX_CHAR_SIZE 64
#define MOUNT_POINT "/sdcard"

static const char *TAG = "example";

static void audio_stop(void); // defined below, used by play_flac

// --christopher: audio out via the PCM5102A I2S DAC.
// Pin names match Freenove's own pinout diagram for this board.
#define AUDIO_BCK GPIO_NUM_26  // labelled I2S_BCK
#define AUDIO_LRCK GPIO_NUM_25 // labelled I2S_LCK
#define AUDIO_DIN GPIO_NUM_22  // labelled I2S_DIN
#define AUDIO_RATE 44100       // CD sample rate
#define AUDIO_AMPLITUDE 6000   // out of 32767; deliberately modest
#define AUDIO_FADE 400         // frames of fade-in/out, ~9ms
#define FLAC_IN_CHUNK 32768     // must exceed the largest FLAC frame (~10KB here)
#define FLAC_OUT_INIT (32 * 1024) // starting PCM buffer; grown if a block needs more

#define TWO_PI_F 6.28318530718f

static i2s_chan_handle_t s_tx;

// Set up the I2S link to the DAC. The sample rate, width and channel count
// come from the file being played, not from us.
static void audio_init(uint32_t rate, int channels, int bits) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // The default (6 x 240 frames) is only ~32ms of audio — about as long as one
  // SD card read, so the buffer runs dry between reads and every gap clicks.
  // 8 x 1000 frames gives ~180ms to coast on. 1000 is near the per-descriptor
  // ceiling: a descriptor holds at most 4092 bytes, and a 16-bit stereo frame
  // is 4 bytes.
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 1000;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

  i2s_data_bit_width_t width = (bits == 24)   ? I2S_DATA_BIT_WIDTH_24BIT
                               : (bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT
                                              : I2S_DATA_BIT_WIDTH_16BIT;

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          width,
          (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED, // PCM5102A makes its own clock
              .bclk = AUDIO_BCK,
              .ws = AUDIO_LRCK,
              .dout = AUDIO_DIN,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {0},
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

// Generate a sine wave and push it to the DAC, same on both channels.
// Ramps the volume up and down over AUDIO_FADE frames at each end: starting
// or stopping a waveform abruptly puts a step change through the speaker,
// which you hear as a click.
static void play_tone(float freq_hz, uint32_t ms) {
  static int16_t buf[512 * 2]; // static: too big for the main task stack
  const int frames = 512;
  float phase = 0.0f;
  float step = TWO_PI_F * freq_hz / AUDIO_RATE;
  uint32_t total = (uint32_t)((uint64_t)AUDIO_RATE * ms / 1000);
  uint32_t done = 0;
  size_t written;

  while (done < total) {
    int n = (total - done < (uint32_t)frames) ? (int)(total - done) : frames;
    for (int i = 0; i < n; i++) {
      uint32_t pos = done + (uint32_t)i;
      float gain = 1.0f;
      if (pos < AUDIO_FADE) {
        gain = (float)pos / AUDIO_FADE;
      } else if (pos + AUDIO_FADE > total) {
        gain = (float)(total - pos) / AUDIO_FADE;
      }
      int16_t s = (int16_t)(sinf(phase) * AUDIO_AMPLITUDE * gain);
      buf[2 * i] = s;     // left
      buf[2 * i + 1] = s; // right
      phase += step;
      if (phase > TWO_PI_F) {
        phase -= TWO_PI_F;
      }
    }
    ESP_ERROR_CHECK(i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t),
                                      &written, portMAX_DELAY));
    done += (uint32_t)n;
  }
}

// Find the first .flac in one directory.
static bool find_flac_in(const char *dir_path, char *out, size_t out_len) {
  DIR *dir = opendir(dir_path);
  if (!dir) {
    return false;
  }
  struct dirent *ent;
  bool found = false;
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (dot && strcasecmp(dot, ".flac") == 0) {
      snprintf(out, out_len, "%s/%s", dir_path, ent->d_name);
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Look in the music folder first, then the top level of the card.
static bool find_flac(char *out, size_t out_len) {
  return find_flac_in(MOUNT_POINT "/music", out, out_len) ||
         find_flac_in(MOUNT_POINT, out, out_len);
}

// Read the file in chunks, hand them to the decoder, push the PCM it returns
// straight at the DAC. The I2S link is set up lazily, once the decoder has
// told us the file's real sample rate and channel count.
static void play_flac(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot open %s", path);
    return;
  }

  esp_audio_dec_register_default();        // the codecs themselves
  esp_audio_simple_dec_register_default(); // the container parsers on top
  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
      .dec_cfg = NULL,
      .cfg_size = 0,
      .use_frame_dec = false,
  };
  esp_audio_simple_dec_handle_t dec = NULL;
  if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open the FLAC decoder");
    fclose(f);
    return;
  }

  // Everything the cleanup block touches must be declared before the first
  // goto, or it reads an uninitialised value.
  bool started = false;
  bool eof = false;
  uint32_t frames_out = 0;
  size_t written;

  // The input buffer receives data straight from the SD card, so it must be
  // internal DMA-capable RAM: the ESP32's SDMMC DMA cannot reach PSRAM.
  // The decoded PCM never touches DMA, so that one can live in PSRAM.
  uint8_t *in = heap_caps_malloc(FLAC_IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  uint32_t out_len = FLAC_OUT_INIT;
  uint8_t *out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!in || !out) {
    ESP_LOGE(TAG, "Out of memory for decode buffers");
    goto done;
  }

  // Follow Espressif's reference loop: read a chunk, feed it until fully
  // consumed, then read the next. The decoder caches partial frames itself,
  // and it keeps pointers into this buffer between calls — so nothing may
  // move or overwrite the buffer while raw.len is still non-zero.
  while (!eof) {
    size_t n = fread(in, 1, FLAC_IN_CHUNK, f);
    if (n == 0) {
      break;
    }
    eof = (n < FLAC_IN_CHUNK);
    esp_audio_simple_dec_raw_t raw = {
        .buffer = in,
        .len = (uint32_t)n,
        .eos = eof, // FLAC needs this to flush its last frame
    };

    while (raw.len > 0) {
      esp_audio_simple_dec_out_t frame = {.buffer = out, .len = out_len};
      esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &frame);

      if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        // A FLAC block was bigger than our buffer; grow and retry.
        heap_caps_free(out);
        out_len = frame.needed_size;
        out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
          ESP_LOGE(TAG, "Out of memory growing to %" PRIu32, out_len);
          goto done;
        }
        continue;
      }
      if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decode failed: %d at offset %ld (unconsumed=%u)",
                 (int)err, ftell(f), (unsigned)raw.len);
        goto done;
      }

      if (frame.decoded_size > 0) {
        if (!started) {
          esp_audio_simple_dec_info_t info;
          esp_audio_simple_dec_get_info(dec, &info);
          ESP_LOGI(TAG, "FLAC: %" PRIu32 " Hz, %d channels, %d bits",
                   info.sample_rate, info.channel, info.bits_per_sample);
          audio_init(info.sample_rate, info.channel, info.bits_per_sample);
          started = true;
        }
        ESP_ERROR_CHECK(i2s_channel_write(s_tx, out, frame.decoded_size,
                                          &written, portMAX_DELAY));
        frames_out++;
      }

      // Guard against spinning forever if the decoder stops making progress.
      if (raw.consumed == 0) {
        break;
      }
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
    }
  }
  ESP_LOGI(TAG, "Finished: %" PRIu32 " blocks decoded", frames_out);

done:
  if (started) {
    audio_stop();
  }
  esp_audio_simple_dec_close(dec);
  esp_audio_simple_dec_unregister_default();
  esp_audio_dec_unregister_default();
  heap_caps_free(in);
  heap_caps_free(out);
  fclose(f);
}

// Stop cleanly. Leaving the channel enabled with nothing left to send makes
// the DMA replay stale buffers, which is audible as a continuous buzz.
static void audio_stop(void) {
  static int16_t silence[256 * 2] = {0};
  size_t written;
  for (int i = 0; i < 4; i++) {
    ESP_ERROR_CHECK(i2s_channel_write(s_tx, silence, sizeof(silence), &written,
                                      portMAX_DELAY));
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // let the last buffers drain
  ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
}

#define EXAMPLE_IS_UHS1                                                        \
  (CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR50 ||                                   \
   CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_DDR50 ||                                   \
   CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR104)

#ifdef CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS
const char *names[] = {"CLK", "CMD", "D0", "D1", "D2", "D3"};
const int pins[] = {CONFIG_EXAMPLE_PIN_CLK,
                    CONFIG_EXAMPLE_PIN_CMD,
                    CONFIG_EXAMPLE_PIN_D0
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
                    ,
                    CONFIG_EXAMPLE_PIN_D1,
                    CONFIG_EXAMPLE_PIN_D2,
                    CONFIG_EXAMPLE_PIN_D3
#endif
};

const int pin_count = sizeof(pins) / sizeof(pins[0]);

#if CONFIG_EXAMPLE_ENABLE_ADC_FEATURE
const int adc_channels[] = {CONFIG_EXAMPLE_ADC_PIN_CLK,
                            CONFIG_EXAMPLE_ADC_PIN_CMD,
                            CONFIG_EXAMPLE_ADC_PIN_D0
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
                            ,
                            CONFIG_EXAMPLE_ADC_PIN_D1,
                            CONFIG_EXAMPLE_ADC_PIN_D2,
                            CONFIG_EXAMPLE_ADC_PIN_D3
#endif
};
#endif // CONFIG_EXAMPLE_ENABLE_ADC_FEATURE

pin_configuration_t config = {
    .names = names,
    .pins = pins,
#if CONFIG_EXAMPLE_ENABLE_ADC_FEATURE
    .adc_channels = adc_channels,
#endif
};
#endif // CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS

static esp_err_t s_example_write_file(const char *path, char *data) {
  ESP_LOGI(TAG, "Opening file %s", path);
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return ESP_FAIL;
  }
  fprintf(f, data);
  fclose(f);
  ESP_LOGI(TAG, "File written");

  return ESP_OK;
}

static esp_err_t s_example_read_file(const char *path) {
  ESP_LOGI(TAG, "Reading file %s", path);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return ESP_FAIL;
  }
  char line[EXAMPLE_MAX_CHAR_SIZE];
  fgets(line, sizeof(line), f);
  fclose(f);

  // strip newline
  char *pos = strchr(line, '\n');
  if (pos) {
    *pos = '\0';
  }
  ESP_LOGI(TAG, "Read from file: '%s'", line);

  return ESP_OK;
}

#if CONFIG_EXAMPLE_PIN_CARD_POWER_RESET
static esp_err_t s_example_reset_card_power(void) {
  esp_err_t ret = ESP_FAIL;
  gpio_config_t io_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = (1ULL << CONFIG_EXAMPLE_PIN_CARD_POWER_RESET),
  };
  ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to config GPIO");
    return ret;
  }

  ret = gpio_set_level(CONFIG_EXAMPLE_PIN_CARD_POWER_RESET, 1);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set GPIO level");
    return ret;
  }

  vTaskDelay(100 / portTICK_PERIOD_MS);

  ret = gpio_set_level(CONFIG_EXAMPLE_PIN_CARD_POWER_RESET, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set GPIO level");
    return ret;
  }

  return ESP_OK;
}
#endif // CONFIG_EXAMPLE_PIN_CARD_POWER_RESET

#include <dirent.h>
#include <sys/stat.h>

static void list_dir(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    ESP_LOGE(TAG, "opendir(%s) failed", path);
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    char full[300];
    snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
    struct stat st;
    if (stat(full, &st) == 0) {
      ESP_LOGI(TAG, "%-8s %10ld  %s", S_ISDIR(st.st_mode) ? "<dir>" : "",
               (long)st.st_size, ent->d_name);
    }
  }
  closedir(dir);
}
void app_main(void) {
  esp_err_t ret;

  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
      .format_if_mount_failed = true,
#else
      .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};
  sdmmc_card_t *card;
  const char mount_point[] = MOUNT_POINT;
  ESP_LOGI(TAG, "Initializing SD card");

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
  // Please check its source code and implement error recovery when developing
  // production applications.

  ESP_LOGI(TAG, "Using SDMMC peripheral");

  // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
  // For setting a specific frequency, use host.max_freq_khz (range 400kHz -
  // 40MHz for SDMMC) Example: for fixed frequency of 10MHz, use
  // host.max_freq_khz = 10000;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.unaligned_multi_block_rw_max_chunk_size = 8;
#if CONFIG_EXAMPLE_SDMMC_SPEED_HS
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#elif CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR50
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_SDR50;
  host.flags &= ~SDMMC_HOST_FLAG_DDR;
#elif CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_DDR50
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_DDR50;
#elif CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR104
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_SDR104;
  host.flags &= ~SDMMC_HOST_FLAG_DDR;
#endif

  // For SoCs where the SD power can be supplied both via an internal or
  // external (e.g. on-board LDO) power supply. When using specific IO pins
  // (which can be used for ultra high-speed SDMMC) to connect to the SD card
  // and the internal LDO power supply, we need to initialize the power supply
  // first.
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
  sd_pwr_ctrl_ldo_config_t ldo_config = {
      .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
  };
  sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

  ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
    return;
  }
  host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

#if CONFIG_EXAMPLE_PIN_CARD_POWER_RESET
  ESP_ERROR_CHECK(s_example_reset_card_power());
#endif

  // This initializes the slot without card detect (CD) and write protect (WP)
  // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
  // has these signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
#if EXAMPLE_IS_UHS1
  slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
#endif

  // Set bus width to use:
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
  slot_config.width = 4;
#else
  slot_config.width = 1;
#endif

  // On chips where the GPIOs used for SD card can be configured, set them in
  // the slot_config structure:
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
  slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
  slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
  slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
  slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
  slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
  slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
#endif // CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
#endif // CONFIG_SOC_SDMMC_USE_GPIO_MATRIX

  // Enable internal pullups on enabled pins. The internal pullups
  // are insufficient however, please make sure 10k external pullups are
  // connected on the bus. This is for debug / example purpose only.
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config,
                                &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem. "
                    "If you want the card to be formatted, set the "
                    "EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    } else {
      ESP_LOGE(TAG,
               "Failed to initialize the card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
#ifdef CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS
      check_sd_card_pins(&config, pin_count);
#endif
    }
    return;
  }
  ESP_LOGI(TAG, "Filesystem mounted");
  list_dir(mount_point);

  // Card has been initialized, print its properties
  sdmmc_card_print_info(stdout, card);

  // Use POSIX and C standard library functions to work with files:

  // First create a file.
  //
  // No --christopher
  // const char *file_hello = MOUNT_POINT "/hello.txt";
  // char data[EXAMPLE_MAX_CHAR_SIZE];
  // snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Hello", card->cid.name);
  // ret = s_example_write_file(file_hello, data);
  // if (ret != ESP_OK) {
  //   return;
  // }

  // Also no --christohper
  // const char *file_foo = MOUNT_POINT "/foo.txt";
  // // Check if destination file exists before renaming
  // struct stat st;
  // if (stat(file_foo, &st) == 0) {
  //   // Delete it if it exists
  //   unlink(file_foo);
  // }
  //
  // Rename original file
  // ESP_LOGI(TAG, "Renaming file %s to %s", file_hello, file_foo);
  // if (rename(file_hello, file_foo) != 0) {
  //   ESP_LOGE(TAG, "Rename failed");
  //   return;
  // }
  //
  // ret = s_example_read_file(file_foo);
  // if (ret != ESP_OK) {
  //   return;
  // }

  // Format FATFS
#ifdef CONFIG_EXAMPLE_FORMAT_SD_CARD
  ret = esp_vfs_fat_sdcard_format(mount_point, card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to format FATFS (%s)", esp_err_to_name(ret));
    return;
  }

  if (stat(file_foo, &st) == 0) {
    ESP_LOGI(TAG, "file still exists");
    return;
  } else {
    ESP_LOGI(TAG, "file doesn't exist, formatting done");
  }
#endif // CONFIG_EXAMPLE_FORMAT_SD_CARD

  const char *file_nihao = MOUNT_POINT "/nihao.txt";
  char data[EXAMPLE_MAX_CHAR_SIZE];
  memset(data, 0, EXAMPLE_MAX_CHAR_SIZE);
  snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Nihao", card->cid.name);
  ret = s_example_write_file(file_nihao, data);
  if (ret != ESP_OK) {
    return;
  }

  // Open file for reading
  ret = s_example_read_file(file_nihao);
  if (ret != ESP_OK) {
    return;
  }

  // Play the first FLAC we find in /sdcard/music.
  char track[320];
  if (find_flac(track, sizeof(track))) {
    ESP_LOGI(TAG, "Playing %s", track);
    play_flac(track);
  } else {
    ESP_LOGW(TAG, "No .flac found in " MOUNT_POINT "/music");
  }

  // All done, unmount partition and disable SDMMC peripheral
  //
  // Commented out to keep the card mounted while I experiment --christopher
  // esp_vfs_fat_sdcard_unmount(mount_point, card);
  // ESP_LOGI(TAG, "Card unmounted");

  // Deinitialize the power control driver if it was used
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
  ret = sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to delete the on-chip LDO power control driver");
    return;
  }
#endif
}
