#include "player.h"

#include "audio_out.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "flac_header.h"
#include "pcm.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define FLAC_IN_CHUNK 32768       // must exceed the largest FLAC frame (~10KB here)
#define FLAC_OUT_INIT (32 * 1024) // starting PCM buffer; grown if a block needs more

// The smallest frame the decoder can hand over is one 16-bit mono sample.
// Sizing the converted buffer for that case means a decoded block always
// fits, whatever format the file turns out to have.
#define PCM_MIN_FRAME_BYTES 2

// The volume level everything plays at, 0..PCM_VOLUME_MAX. It stands in until
// the volume control arrives in the next task: 10 is the level the player
// will start at, 20 dB below full scale.
#define PLAYER_DEFAULT_VOLUME 10

static const char *TAG = "player";

// Allocate the two PCM buffers of a track: `out_len` bytes for what the
// decoder returns, and room for the same block as 32-bit stereo frames (8
// bytes each, so four times `out_len`). Neither touches DMA, so both can live
// in PSRAM. On failure both pointers are NULL.
static bool alloc_pcm_buffers(uint32_t out_len, uint8_t **out, int32_t **pcm,
                              size_t *pcm_frames_max) {
  size_t frames = out_len / PCM_MIN_FRAME_BYTES;
  *out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  *pcm = heap_caps_malloc(frames * 2 * sizeof(int32_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*out || !*pcm) {
    heap_caps_free(*out);
    heap_caps_free(*pcm);
    *out = NULL;
    *pcm = NULL;
    return false;
  }
  *pcm_frames_max = frames;
  return true;
}

void player_init(void) {
  esp_audio_dec_register_default();        // the codecs themselves
  esp_audio_simple_dec_register_default(); // the container parsers on top
}

// Read the file in chunks, hand them to the decoder, convert each block it
// returns to 32-bit stereo, scale it to the volume and push that at the DAC.
// The I2S link is set up lazily, once the decoder has told us the file's real
// sample rate and format.
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
  uint64_t frames_out = 0;           // frames sent to the DAC
  pcm_format_t format = PCM_FMT_S16; // this and `channels`: known once playing
  int channels = 0;
  const int32_t gain = pcm_volume_gain(PLAYER_DEFAULT_VOLUME);

  // The input buffer receives data straight from the SD card, so it must be
  // internal DMA-capable RAM: the ESP32's SDMMC DMA cannot reach PSRAM.
  uint8_t *in =
      heap_caps_malloc(FLAC_IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  uint32_t out_len = FLAC_OUT_INIT;
  uint8_t *out = NULL;       // what the decoder returns
  int32_t *pcm = NULL;       // the same block as 32-bit stereo frames
  size_t pcm_frames_max = 0; // frames `pcm` holds
  if (!alloc_pcm_buffers(out_len, &out, &pcm, &pcm_frames_max) || !in) {
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
        // A FLAC block was bigger than our buffer; grow and retry. The
        // converted buffer grows with it so the block still fits.
        heap_caps_free(out);
        heap_caps_free(pcm);
        out_len = frame.needed_size;
        if (!alloc_pcm_buffers(out_len, &out, &pcm, &pcm_frames_max)) {
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
          if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "The decoder did not report the stream format");
            goto done;
          }
          ESP_LOGI(TAG, "FLAC: %" PRIu32 " Hz, %d channels, %d bits",
                   info.sample_rate, info.channel, info.bits_per_sample);
          // The decoder packs samples: 2 bytes for 16-bit, 3 for 24-bit.
          if (info.bits_per_sample == 16) {
            format = PCM_FMT_S16;
          } else if (info.bits_per_sample == 24) {
            format = PCM_FMT_S24_PACKED;
          } else {
            ESP_LOGE(TAG, "Unsupported sample size: %d bits",
                     info.bits_per_sample);
            result = ESP_ERR_NOT_SUPPORTED;
            goto done;
          }
          if (info.channel != 1 && info.channel != 2) {
            ESP_LOGE(TAG, "Unsupported channel count: %d", info.channel);
            result = ESP_ERR_NOT_SUPPORTED;
            goto done;
          }
          channels = info.channel;
          esp_err_t start_err = audio_out_start(info.sample_rate);
          if (start_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start audio output (%s)",
                     esp_err_to_name(start_err));
            result = start_err;
            goto done;
          }
          started = true;
        }
        size_t frames = pcm_to_s32_stereo(out, frame.decoded_size, format,
                                          channels, pcm, pcm_frames_max);
        pcm_apply_gain(pcm, frames, gain, gain);
        esp_err_t write_err = audio_out_write(pcm, frames);
        if (write_err != ESP_OK) {
          ESP_LOGE(TAG, "Audio write failed (%s)", esp_err_to_name(write_err));
          result = write_err;
          goto done;
        }
        frames_out += frames;
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
  ESP_LOGI(TAG, "ENDED frames=%" PRIu64 " expected=%" PRIu64, frames_out,
           expected);
  result = ESP_OK;

done:
  if (started) {
    audio_out_stop();
  }
  esp_audio_simple_dec_close(dec);
  heap_caps_free(in);
  heap_caps_free(out);
  heap_caps_free(pcm);
  fclose(f);
  return result;
}
