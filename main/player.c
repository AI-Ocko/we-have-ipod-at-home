#include "player.h"

#include "audio_out.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "flac_header.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define FLAC_IN_CHUNK 32768       // must exceed the largest FLAC frame (~10KB here)
#define FLAC_OUT_INIT (32 * 1024) // starting PCM buffer; grown if a block needs more

static const char *TAG = "player";

void player_init(void) {
  esp_audio_dec_register_default();        // the codecs themselves
  esp_audio_simple_dec_register_default(); // the container parsers on top
}

// Read the file in chunks, hand them to the decoder, push the PCM it returns
// straight at the DAC. The I2S link is set up lazily, once the decoder has
// told us the file's real sample rate and channel count.
esp_err_t player_play_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot open %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  // The header says how many frames the file holds, so the end-of-track log
  // can show whether all of them were played. The decoder parses the file
  // from its first byte, so go back there afterwards.
  flac_header_t header;
  uint64_t expected = 0; // 0: the file does not say, or has no usable header
  if (flac_header_read(f, &header) == FLAC_HEADER_OK) {
    expected = header.total_frames;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Cannot rewind %s", path);
    fclose(f);
    return ESP_FAIL;
  }

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
    return ESP_FAIL;
  }

  // Everything the cleanup block touches must be declared before the first
  // goto, or it reads an uninitialised value.
  esp_err_t result = ESP_FAIL;
  bool started = false;
  bool eof = false;
  uint64_t bytes_out = 0;   // PCM bytes sent to the DAC
  uint32_t frame_bytes = 0; // bytes in one PCM frame, known once playing

  // The input buffer receives data straight from the SD card, so it must be
  // internal DMA-capable RAM: the ESP32's SDMMC DMA cannot reach PSRAM.
  // The decoded PCM never touches DMA, so that one can live in PSRAM.
  uint8_t *in =
      heap_caps_malloc(FLAC_IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  uint32_t out_len = FLAC_OUT_INIT;
  uint8_t *out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!in || !out) {
    ESP_LOGE(TAG, "Out of memory for decode buffers");
    result = ESP_ERR_NO_MEM;
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
          result = ESP_ERR_NO_MEM;
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
          esp_err_t start_err = audio_out_start(info.sample_rate, info.channel,
                                                info.bits_per_sample);
          if (start_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start audio output (%s)",
                     esp_err_to_name(start_err));
            result = start_err;
            goto done;
          }
          // The decoder packs samples: 2 bytes for 16-bit, 3 for 24-bit.
          frame_bytes = (uint32_t)info.channel * (info.bits_per_sample / 8);
          started = true;
        }
        esp_err_t write_err = audio_out_write(out, frame.decoded_size);
        if (write_err != ESP_OK) {
          ESP_LOGE(TAG, "Audio write failed (%s)", esp_err_to_name(write_err));
          result = write_err;
          goto done;
        }
        bytes_out += frame.decoded_size;
      }

      // Guard against spinning forever if the decoder stops making progress.
      // Progress is input taken OR a block returned: when the frames are much
      // smaller than the file's largest, the parser already holds several, and
      // it hands them out one per call while taking no new input. Treating
      // that as a stall threw away the rest of the chunk and skipped audio.
      if (raw.consumed == 0 && frame.decoded_size == 0) {
        break;
      }
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
    }
  }
  ESP_LOGI(TAG, "ENDED frames=%" PRIu64 " expected=%" PRIu64,
           frame_bytes ? bytes_out / frame_bytes : 0, expected);
  result = ESP_OK;

done:
  if (started) {
    audio_out_stop();
  }
  esp_audio_simple_dec_close(dec);
  heap_caps_free(in);
  heap_caps_free(out);
  fclose(f);
  return result;
}
