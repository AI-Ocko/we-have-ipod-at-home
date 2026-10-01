// The player: two tasks on core 1 with a buffer between them.
//
//   commands ─► decoder task ─► ring (PSRAM, native PCM) ─► output task ─► I2S
//                    │                                          │
//                    └──────────────── events ◄─────────────────┘
//
// DECODER TASK (priority 10, 16 KB stack). The controller. It takes the
// commands, owns the track (file, decoder, input and output buffers), decides
// the player's state and tells the output task what to do. It decodes a block
// whenever the ring has room for one.
//
// OUTPUT TASK (priority 20, 4 KB stack). It owns the I2S link. While a stream
// is playing it takes one block at a time from the ring into internal RAM,
// converts it to 32-bit stereo, applies the gain and writes it to I2S; the
// write is what paces it. It counts the frames it has handed over, which is
// the position.
//
// States
// ------
// The player is idle, playing or paused (player_state_t): idle when no track
// is loaded, otherwise paused or playing. The decoder task keeps that state
// (`active` and `paused` in dec_t); the output task has its own smaller one:
//
//   STOPPED  no stream. If the link is still running it is being emptied (one
//            ring of silence, then audio_out_stop); after that the task sleeps.
//   PLAYING  blocks go from the ring to I2S.
//   PAUSED   the stream is held where it stopped. The link is emptied and
//            shut down exactly as in STOPPED; resuming starts it again.
//
// Inside "playing or paused" the decoder task's track is in one of two
// phases: decoding (the file is open and blocks are still being produced) and
// draining (the file is finished and closed; the ring is playing out).
//
// Who owns what
// -------------
// Decoder task only: `s_dec` (the track, file, decoder handle, both decoder
//   buffers), the generation counter, posting STARTED, ENDED and ERROR.
// Output task only: `out_t` (its state, the gain, the frame count), the two
//   block buffers `s_out_raw` and `s_out_pcm`, every audio_out_* call,
//   posting POSITION, resetting the ring.
// The ring: written only by the decoder task, read only by the output task.
// The path of the current track: written by the decoder task, copied out by
//   player_get_path(); a mutex guards it, held only for a pointer swap or a
//   string copy, never across a blocking call and never by the output task.
// Everything else that more than one task touches is a single 32-bit word
//   read and written atomically; each is listed with its writer where it is
//   declared below ("Shared words").
//
// Messages
// --------
// Callers ──► decoder task (`s_cmd_q`, 8 deep, never waited on by the sender):
//   PLAY(path)   stop what is playing, open the file, start decoding
//   PAUSE        playing -> paused; otherwise ignored
//   RESUME       paused -> playing; otherwise ignored
//   STOP         stop what is playing, go idle
//   SEEK         ignored until Task 8
//   STALL(ms)    test aid: the decoder task does nothing for that long
// Volume is not a message: player_set_volume() stores the level in a shared
// word and the output task reads it once per block, so it takes effect in the
// next block (11 ms at 44.1 kHz) whatever the decoder task is doing.
//
// Output task ──► decoder task (same queue):
//   WAKE         "look again": carries nothing. What there is to see is in the
//                shared words (room in the ring, the stream finished, the link
//                failed), so a WAKE that cannot be queued loses nothing: the
//                queue is only full when the decoder task is awake and about
//                to look anyway.
//
// Decoder task ──► output task (`s_out_q`, 8 deep):
//   START(gen, rate, format, channels, paused, eos)
//                a new stream is in the ring; play it (or hold it, if paused)
//   EOS(gen)     the last block of the stream is in the ring
//   PAUSE        fade out and hold
//   RESUME       fade in and continue
//   FLUSH        fade out, throw away what is in the ring, forget the stream;
//                answered through the semaphore `s_flushed`
//   WAKE         "look again": there is new data in the ring
//
// How each thing flows
// --------------------
// play:    decoder: FLUSH and wait for the answer if a track is loaded; open
//          the file; decode until the ring holds 250 ms of audio (or the file
//          ends); post STARTED; send START. The output task starts the link
//          at the stream's rate, first emptying and stopping it if it was
//          running at another rate, and fades in over 400 frames.
// pause:   decoder: state = paused, send PAUSE. Output: the next 400 frames
//          from the ring are played with the gain running down to zero; they
//          count as played, so nothing is lost or repeated. The decoder keeps
//          filling the ring while paused.
// resume:  decoder: state = playing, send RESUME. Output: carries on from the
//          next frame in the ring with the gain running up from zero.
// stop, and play while something is loaded:
//          decoder: close the file, send FLUSH, wait for `s_flushed`. Output:
//          fade out as for pause, reset the ring, give `s_flushed`.
// end of track:
//          decoder: when the file's last byte has been decoded, close the
//          file and send EOS. Output: when fewer than a whole block is left in
//          the ring and EOS has arrived, play what is left, then publish
//          `s_out_done = gen` and WAKE the decoder task, which posts ENDED.
//          The output task then empties the link (16 blocks of silence push
//          the track's last frames out of the DMA ring) before stopping it.
// decode or read error part-way:
//          as end of track, but the decoder task posts ERROR instead of ENDED.
// error before anything was buffered (missing file, not a FLAC, ...):
//          the decoder task posts ERROR at once; the output task is not
//          involved.
// underrun:
//          output: the ring holds less than a block and there is no EOS. It
//          counts one underrun, raises `s_out_starving` and sleeps on its
//          queue; I2S sends silence by itself once its own ring is empty. The
//          decoder task sees the flag after its next write to the ring and
//          sends WAKE. Playing continues once the ring holds 250 ms again,
//          with a fade-in if the silence was long enough to be heard.
// ring full:
//          decoder: a decoded block is sent only when all of it fits. If it
//          does not, the decoder task publishes how much room it needs in
//          `s_dec_need` and sleeps on its command queue, so commands are
//          served at once while it waits. The output task sends WAKE when
//          that much room has appeared.
// Both "tell me when" flags are raised and then the condition is checked once
// more before sleeping, so a wake-up cannot fall between the check and the
// sleep.
//
// Flushing the ring safely
// ------------------------
// A stream buffer may only be reset while no task is blocked on it, and not
// while the other side is in the middle of using it. Neither task ever blocks
// on the ring: every send and receive has a zero timeout and the waiting is
// done on the queues as described above. The reset is done by the output
// task (the reader) while it handles FLUSH, and the decoder task (the writer)
// is at that moment blocked on `s_flushed`, so it cannot be writing.
//
// Whole frames, whole blocks
// --------------------------
// The decoder task writes whole decoded blocks and the output task reads
// whole frames, so the ring's contents always start on a frame boundary.
// Every write to I2S is exactly one DMA buffer (AUDIO_OUT_BLOCK_FRAMES
// frames, padded with silence after a fade-out or at the end of a track), so
// the writer never leaves a DMA buffer half filled while it waits for data.
//
// Generations
// -----------
// Each `play` gets a new generation number. `s_out_done` and `s_out_failed`
// hold the generation they refer to, so a late report about a stream that has
// since been replaced is not mistaken for news of the current one.
//
// Not in the audio loops: logging, allocation (buffers are made at start-up;
// the decoder's output buffer is replaced only if a block does not fit),
// floating point, per-sample division or calls. STARTED, POSITION, ENDED and
// ERROR are logged by whoever reads the events.

#include "player.h"

#include "audio_out.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec_reg.h"
#include "esp_flac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flac_header.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "pcm.h"
#include "player_status.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define RING_BYTES 786432 // native PCM: 4.4 s at 44.1 kHz 16-bit stereo
#define IN_CHUNK 32768    // compressed input; must exceed the largest frame
#define OUT_INIT 32768    // decoder output: 4096 frames of 24-bit stereo fit

// The output task moves one DMA buffer's worth at a time.
#define BLOCK_FRAMES AUDIO_OUT_BLOCK_FRAMES
#define MAX_FRAME_BYTES 6 // 24-bit stereo
#define FADE_FRAMES 400
// Audio in the ring before a track starts to sound, and before it carries on
// after an underrun. It must be at least one block at the lowest sample rate.
#define PREFILL_MS 250
#define POSITION_INTERVAL_MS 500

#define DEFAULT_VOLUME 10

#define DECODER_TASK_STACK 16384 // the FLAC decoder overflows smaller stacks
#define DECODER_TASK_PRIORITY 10
#define OUTPUT_TASK_STACK 4096
#define OUTPUT_TASK_PRIORITY 20
#define AUDIO_CORE 1

#define NO_ERROR (-1) // in a variable that otherwise holds a player_error_t

_Static_assert(FADE_FRAMES <= BLOCK_FRAMES, "a fade must fit in one block");
_Static_assert(AUDIO_OUT_RATE_MIN / (1000 / PREFILL_MS) >= BLOCK_FRAMES,
               "the prefill must be at least one block");

static const char *TAG = "player";

// ---- messages ---------------------------------------------------------------

typedef enum {
  CMD_PLAY,
  CMD_PAUSE,
  CMD_RESUME,
  CMD_STOP,
  CMD_SEEK,
  CMD_STALL, // test aid: see player_debug_stall()
  CMD_WAKE,  // from the output task
} cmd_type_t;

typedef struct {
  cmd_type_t type;
  char *path; // CMD_PLAY: heap copy, owned by whoever holds the message
  int32_t ms; // PLAY: start position; SEEK: distance; STALL: how long
} cmd_t;

typedef enum {
  OMSG_START,
  OMSG_EOS,
  OMSG_PAUSE,
  OMSG_RESUME,
  OMSG_FLUSH,
  OMSG_WAKE,
} omsg_type_t;

typedef struct {
  omsg_type_t type;
  uint32_t gen;
  // OMSG_START only:
  uint32_t rate;
  pcm_format_t format;
  uint8_t channels;
  bool paused; // hold the stream instead of playing it
  bool eos;    // the whole stream is already in the ring
} omsg_t;

// ---- things made once at start-up -------------------------------------------

static QueueHandle_t s_events; // the caller's queue
static QueueHandle_t s_cmd_q;
static QueueHandle_t s_out_q;
static SemaphoreHandle_t s_flushed;   // the output task's answer to OMSG_FLUSH
static SemaphoreHandle_t s_path_lock; // guards s_path
static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_control;
static TaskHandle_t s_decoder_task;
static TaskHandle_t s_output_task;

static char *s_path; // the current track's path (heap); see s_path_lock

// ---- shared words -----------------------------------------------------------
//
// Each is one 32-bit word. "Writer" is the only task that stores to it,
// unless two are named with when each may.

// Writer: any task, in player_set_volume(). Read by the output task per block.
static atomic_int s_volume = DEFAULT_VOLUME;

// Writer: any task, in the command functions: 1 after `pause`, 0 after
// `play`, `resume` or `stop`. It exists so that the reported state follows a
// command at once, before the decoder task has got to it; the audio path
// does not read it (the decoder task acts on the commands themselves).
static atomic_uint s_pause_wanted;

// Writer: decoder task.
static atomic_uint s_loaded;         // a track is loaded (playing or paused)
static atomic_uint s_rate;           // of the current track; 0 until known
static atomic_uint s_ring_tracked;   // the ring has been full once this track
static atomic_uint s_ended_frames;   // of the last track that reached its end
static atomic_uint s_ended_expected; //
static atomic_uint s_dec_busy_us;    // this track
static atomic_uint s_track_start_ms; // time since boot
static atomic_uint s_track_end_ms;   // 0 while a track is loaded

// Writer: decoder task (sets the number of bytes it is waiting for); the
// output task swaps it back to 0 when it sends the WAKE.
static atomic_uint s_dec_need;

// Writer: output task (sets it before sleeping for want of data); the decoder
// task swaps it back to 0 when it sends the WAKE.
static atomic_uint s_out_starving;

// Writer: output task while it has a stream. The decoder task resets these
// when it opens a track, which is after a FLUSH has been answered or the
// stream has finished: the output task has no stream then and leaves them
// alone until the next START.
static atomic_uint s_frames_played;
static atomic_uint s_ring_low;
static atomic_uint s_out_busy_us;

// Writer: output task.
static atomic_uint s_out_done;   // generation of the stream it has played out
static atomic_uint s_out_failed; // generation of the stream the link refused
static atomic_uint s_underruns;  // since start-up

// ---- small helpers ----------------------------------------------------------

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void post_event(const player_event_t *event) {
  xQueueSend(s_events, event, 0); // a full queue drops the event, by design
}

// Tell the decoder task to look at the shared words again. May fail when its
// queue is full, which is harmless: see "Messages" at the top.
static void wake_decoder(void) {
  const cmd_t wake = {.type = CMD_WAKE};
  xQueueSend(s_cmd_q, &wake, 0);
}

// ---- the corrected FLAC parser ----------------------------------------------

// The codec library's own FLAC frame parser. It is exported by the library
// but declared in none of its headers; the signature is the documented
// esp_es_parse_func_t.
esp_es_parse_err_t esp_flac_dec_parse_frame(esp_es_parse_raw_t *in,
                                            esp_es_parse_frame_info_t *info);

// True if p[0..len) ends with the CRC-16 of everything before it, as a
// complete FLAC frame does (polynomial 0x8005, most significant bit first,
// stored big-endian). Bit by bit: this runs once per track, on one frame.
static bool flac_frame_crc_ok(const uint8_t *p, uint32_t len) {
  if (len < 4) {
    return false;
  }
  uint16_t crc = 0;
  for (uint32_t i = 0; i < len - 2; i++) {
    crc ^= (uint16_t)(p[i] << 8);
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005)
                           : (uint16_t)(crc << 1);
    }
  }
  return crc == (uint16_t)((p[len - 2] << 8) | p[len - 1]);
}

// The library's FLAC parser with one decision corrected: what to do with the
// last frame of a stream.
//
// A FLAC frame does not state its length; it ends where the next frame's
// header starts. The last frame has no next header, so at end of stream the
// library takes all that is left as the frame. But if the two bytes that start
// a header (0xFF 0xF8 or 0xFF 0xF9) happen to occur inside that last frame, it
// instead skips everything before them, finds nothing valid after, logs "Not
// found frame" and drops the frame: the track's final block is never played.
// Such byte pairs turn up by chance (about one time in eight in 4 KB of
// compressed music) and in nearly every frame of very quiet audio.
//
// So when the library answers "skip" at end of stream, check what it did not:
// if all that is left is one frame with a correct CRC-16, that is the frame.
static esp_es_parse_err_t flac_parse_frame(esp_es_parse_raw_t *in,
                                           esp_es_parse_frame_info_t *info) {
  esp_es_parse_err_t err = esp_flac_dec_parse_frame(in, info);
  if (err == ESP_ES_PARSE_ERR_OK && in->eos && !in->bos &&
      info->frame_size == 0 && info->skipped_size > 0 &&
      flac_frame_crc_ok(in->buffer, in->len)) {
    info->frame_size = in->len;
    info->skipped_size = 0;
  }
  return err;
}

// Register the decoders. Never undone: the library's unregister call would
// take the FLAC decoder away along with our parser.
static void register_codecs(void) {
  esp_audio_dec_register_default();        // the codecs themselves
  esp_audio_simple_dec_register_default(); // the container parsers on top

  // Same FLAC decoder, our corrected parser in front of it.
  esp_audio_simple_dec_reg_info_t flac = {
      .decoder_ops = ESP_FLAC_DEC_DEFAULT_OPS(),
      .parser = flac_parse_frame,
  };
  if (esp_audio_simple_dec_register(ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC, &flac) !=
      ESP_AUDIO_ERR_OK) {
    // Playback still works with the library's parser; some tracks will end
    // one block early.
    ESP_LOGE(TAG, "Cannot register the FLAC parser");
  }
}

// ============================================================================
// Output task
// ============================================================================

typedef enum { OUT_STOPPED, OUT_PLAYING, OUT_PAUSED } out_state_t;

typedef struct {
  out_state_t state;

  // The stream, as START described it.
  uint32_t gen;
  uint32_t rate;
  pcm_format_t format;
  uint8_t channels;
  uint32_t frame_bytes; // one frame in the ring
  uint32_t block_bytes; // BLOCK_FRAMES of them
  bool eos;             // everything the stream will ever hold is in the ring

  // An underrun lasts from the ring running dry until it holds
  // `refill_bytes` again (or the stream ends), so that one stall is one
  // underrun and not a stutter of them while the decoder catches up.
  bool starved;
  uint32_t refill_bytes;
  int64_t last_write_us; // when I2S last took a block
  int64_t dry_after_us;  // after this long with no write, the DMA ring is empty

  bool waiting; // nothing to do until a message arrives
  int32_t gain; // where the last block written left the gain (Q15)

  uint32_t frames; // of this stream handed to I2S
  uint32_t next_position_ms;
  uint32_t next_position_frames;
  uint32_t busy_us;

  // The I2S link.
  bool running;
  uint32_t running_rate;
  uint32_t silence_owed; // blocks of silence still to write before every
                         // real frame has left the DMA ring
} out_t;

// One block on its way from the ring to I2S. Internal RAM: converting in
// PSRAM costs ten times as much.
static uint8_t s_out_raw[BLOCK_FRAMES * MAX_FRAME_BYTES];
static int32_t s_out_pcm[BLOCK_FRAMES * 2];

// The stream cannot be played any further (the link would not start, or
// refused a write). Leave it to the decoder task to clear up.
static void out_fail(out_t *o) {
  o->state = OUT_STOPPED;
  atomic_store(&s_out_failed, o->gen);
  wake_decoder();
}

// Push the last real frames out of the DMA ring and stop the link. Blocks for
// as long as that takes (up to one ring: 181 ms at 44.1 kHz).
static void out_close_link(out_t *o) {
  memset(s_out_pcm, 0, sizeof(s_out_pcm));
  while (o->silence_owed > 0) {
    if (audio_out_write(s_out_pcm, BLOCK_FRAMES) != ESP_OK) {
      break;
    }
    o->silence_owed--;
  }
  audio_out_stop();
  o->running = false;
  o->silence_owed = 0;
}

// Make sure the link is running at the stream's rate. Must be called before
// a block is put together, because closing the link uses s_out_pcm.
static bool out_open_link(out_t *o) {
  if (o->running && o->running_rate == o->rate) {
    return true;
  }
  if (o->running) {
    out_close_link(o); // another rate: let the old track's tail out first
  }
  if (audio_out_start(o->rate) != ESP_OK) {
    return false;
  }
  o->running = true;
  o->running_rate = o->rate;
  o->silence_owed = 0;
  return true;
}

// Take `bytes` (whole frames, known to be there) from the ring and convert
// them into s_out_pcm. Returns the number of frames.
static size_t out_fetch(out_t *o, size_t bytes) {
  size_t got = xStreamBufferReceive(s_ring, s_out_raw, bytes, 0);

  // Room has appeared: is it what the decoder task is waiting for?
  size_t space = xStreamBufferSpacesAvailable(s_ring);
  uint32_t need = atomic_load(&s_dec_need);
  if (need != 0 && space >= need && atomic_exchange(&s_dec_need, 0) != 0) {
    wake_decoder();
  }

  // The reserve at its lowest, while the decoder is meant to be keeping the
  // ring full.
  if (!o->eos && atomic_load(&s_ring_tracked)) {
    uint32_t fill = (uint32_t)(RING_BYTES - space);
    if (fill < atomic_load(&s_ring_low)) {
      atomic_store(&s_ring_low, fill);
    }
  }

  return pcm_to_s32_stereo(s_out_raw, got, o->format, o->channels, s_out_pcm,
                           BLOCK_FRAMES);
}

// Scale the `frames` frames in s_out_pcm, moving the gain from where the last
// block left it to `gain_to` over the first FADE_FRAMES, and fill the rest of
// the block with silence.
static void out_shape(out_t *o, size_t frames, int32_t gain_to) {
  if (o->gain == gain_to) {
    pcm_apply_gain(s_out_pcm, frames, gain_to, gain_to);
  } else {
    size_t ramp = frames < FADE_FRAMES ? frames : FADE_FRAMES;
    pcm_apply_gain(s_out_pcm, ramp, o->gain, gain_to);
    pcm_apply_gain(s_out_pcm + 2 * ramp, frames - ramp, gain_to, gain_to);
    o->gain = gain_to;
  }
  if (frames < BLOCK_FRAMES) {
    memset(s_out_pcm + 2 * frames, 0,
           (BLOCK_FRAMES - frames) * 2 * sizeof(s_out_pcm[0]));
  }
}

// Write the block in s_out_pcm, of which the first `frames` frames are the
// stream's. Waits for I2S to take it. False if the link failed.
static bool out_write(out_t *o, size_t frames) {
  if (audio_out_write(s_out_pcm, BLOCK_FRAMES) != ESP_OK) {
    out_fail(o);
    return false;
  }
  o->last_write_us = esp_timer_get_time();
  o->silence_owed = AUDIO_OUT_BLOCKS;
  o->frames += frames;
  atomic_store(&s_frames_played, o->frames);

  if (o->frames >= o->next_position_frames) {
    const player_event_t event = {
        .type = PLAYER_EVENT_POSITION,
        .position_ms = o->next_position_ms,
    };
    post_event(&event);
    o->next_position_ms += POSITION_INTERVAL_MS;
    o->next_position_frames =
        (uint32_t)((uint64_t)o->next_position_ms * o->rate / 1000);
  }
  return true;
}

// Bring the sound down to nothing using the next frames in the ring. Those
// frames are played, quietly, so a later resume continues after them. Does
// nothing if nothing is sounding.
static void out_fade_out(out_t *o) {
  if (o->gain == 0 || !o->running) {
    return;
  }
  size_t bytes = xStreamBufferBytesAvailable(s_ring);
  if (bytes > FADE_FRAMES * o->frame_bytes) {
    bytes = FADE_FRAMES * o->frame_bytes;
  }
  if (bytes == 0) {
    o->gain = 0; // the ring is dry: the sound has already stopped
    return;
  }
  size_t frames = out_fetch(o, bytes);
  out_shape(o, frames, 0);
  out_write(o, frames);
}

// Move one block of the stream from the ring to I2S, or work out why not.
static void out_play(out_t *o) {
  if (!out_open_link(o)) {
    out_fail(o);
    return;
  }

  size_t available = xStreamBufferBytesAvailable(s_ring);
  size_t wanted = o->starved ? o->refill_bytes : o->block_bytes;
  if (available < wanted && !o->eos) {
    // Underrun. Ask to be woken when there is data, then look once more in
    // case it arrived while we were asking.
    if (!o->starved) {
      o->starved = true;
      atomic_fetch_add(&s_underruns, 1);
    }
    atomic_store(&s_out_starving, 1);
    if (xStreamBufferBytesAvailable(s_ring) < o->refill_bytes) {
      o->waiting = true;
    }
    return;
  }
  if (o->starved) {
    o->starved = false;
    // If the stall outlasted what I2S had queued, the sound has stopped:
    // come back in with a fade. If not, nobody heard anything and the next
    // block simply follows the last.
    if (esp_timer_get_time() - o->last_write_us >= o->dry_after_us) {
      o->gain = 0;
    }
  }
  size_t bytes = o->block_bytes;
  if (available < bytes) {
    bytes = available; // the end of the stream: less than a block is left
  }

  if (bytes > 0) {
    int64_t t0 = esp_timer_get_time();
    size_t frames = out_fetch(o, bytes);
    out_shape(o, frames, pcm_volume_gain(atomic_load(&s_volume)));
    o->busy_us += (uint32_t)(esp_timer_get_time() - t0);
    atomic_store(&s_out_busy_us, o->busy_us);
    if (!out_write(o, frames)) {
      return;
    }
  }

  if (bytes < o->block_bytes) {
    // That was the last of it. The link is emptied by the main loop.
    o->state = OUT_STOPPED;
    atomic_store(&s_out_done, o->gen);
    wake_decoder();
  }
}

// No stream is playing but the link is still running: write the silence that
// pushes the last real frames out, one block per call so that messages are
// still served, then stop the link.
static void out_wind_down(out_t *o) {
  if (o->silence_owed > 0) {
    memset(s_out_pcm, 0, sizeof(s_out_pcm));
    if (audio_out_write(s_out_pcm, BLOCK_FRAMES) == ESP_OK) {
      o->silence_owed--;
      return;
    }
  }
  out_close_link(o);
}

static void out_handle(out_t *o, const omsg_t *m) {
  switch (m->type) {
  case OMSG_START:
    o->gen = m->gen;
    o->rate = m->rate;
    o->format = m->format;
    o->channels = m->channels;
    o->frame_bytes = (uint32_t)pcm_bytes_per_sample(m->format) * m->channels;
    o->block_bytes = BLOCK_FRAMES * o->frame_bytes;
    o->refill_bytes = m->rate / (1000 / PREFILL_MS) * o->frame_bytes;
    o->dry_after_us =
        (int64_t)(AUDIO_OUT_BLOCKS - 1) * BLOCK_FRAMES * 1000000 / m->rate;
    o->eos = m->eos;
    o->starved = false;
    o->gain = 0; // fade in
    o->frames = 0;
    o->busy_us = 0;
    o->next_position_ms = POSITION_INTERVAL_MS;
    o->next_position_frames =
        (uint32_t)((uint64_t)POSITION_INTERVAL_MS * m->rate / 1000);
    o->state = m->paused ? OUT_PAUSED : OUT_PLAYING;
    break;

  case OMSG_EOS:
    if (m->gen == o->gen) {
      o->eos = true;
    }
    break;

  case OMSG_PAUSE:
    if (o->state == OUT_PLAYING) {
      out_fade_out(o);
      if (o->state == OUT_PLAYING) { // unless the link failed in the fade
        o->state = OUT_PAUSED;
      }
    }
    break;

  case OMSG_RESUME:
    if (o->state == OUT_PAUSED) {
      o->gain = 0; // fade in
      o->state = OUT_PLAYING;
    }
    break;

  case OMSG_FLUSH:
    if (o->state == OUT_PLAYING) {
      out_fade_out(o);
    }
    // Safe: no task ever blocks on the ring, and the writer is waiting for
    // s_flushed.
    xStreamBufferReset(s_ring);
    atomic_store(&s_out_starving, 0);
    o->state = OUT_STOPPED;
    o->eos = false;
    xSemaphoreGive(s_flushed);
    break;

  case OMSG_WAKE:
    break;
  }
}

static void output_task(void *arg) {
  (void)arg;
  out_t o = {.state = OUT_STOPPED};

  for (;;) {
    // Messages first. Sleep on the queue when there is nothing else to do:
    // no stream and the link shut down, or a stream with nothing to play.
    bool work = (o.state == OUT_PLAYING) ? !o.waiting : o.running;
    omsg_t m;
    if (xQueueReceive(s_out_q, &m, work ? 0 : portMAX_DELAY) == pdTRUE) {
      o.waiting = false;
      out_handle(&o, &m);
      continue;
    }
    if (o.state == OUT_PLAYING) {
      out_play(&o);
    } else {
      out_wind_down(&o);
    }
  }
}

// ============================================================================
// Decoder task
// ============================================================================

typedef struct {
  bool active;      // a track is loaded: the player is playing or paused
  bool decoding;    // its file is open and still being decoded
  bool out_started; // START has been sent for it
  bool paused;
  uint32_t gen;

  FILE *file;
  uint32_t file_left; // bytes not yet read
  esp_audio_simple_dec_handle_t dec;
  esp_audio_simple_dec_raw_t raw; // the input not yet consumed

  // Known from the first decoded block on.
  bool format_known;
  uint32_t rate;
  uint8_t bits;
  uint8_t channels;
  pcm_format_t format;
  uint32_t frame_bytes;
  uint32_t prefill_bytes;

  uint32_t pending;  // bytes at the start of `out` waiting for room in the ring
  uint32_t sent;     // bytes put in the ring so far
  uint32_t expected; // frames, from the file's header; 0 if it does not say
  uint32_t duration_ms;
  int end_error; // what to report when the ring has played out
  uint32_t busy_us;

  // Made once and kept from track to track.
  uint8_t *in;  // IN_CHUNK bytes, internal DMA-capable RAM
  uint8_t *out; // decoded PCM, `out_len` bytes
  uint32_t out_len;
  uint32_t gen_counter;
} dec_t;

static dec_t s_dec;

static void out_send(omsg_type_t type) {
  const omsg_t m = {.type = type, .gen = s_dec.gen};
  xQueueSend(s_out_q, &m, portMAX_DELAY);
}

static void dec_close_source(void) {
  dec_t *d = &s_dec;
  if (d->dec) {
    esp_audio_simple_dec_close(d->dec);
    d->dec = NULL;
  }
  if (d->file) {
    fclose(d->file);
    d->file = NULL;
  }
  d->decoding = false;
  d->pending = 0;
  d->raw.len = 0;
  atomic_store(&s_dec_need, 0);
}

// The track is over, however it ended. Back to idle.
static void dec_unload(void) {
  s_dec.active = false;
  s_dec.paused = false;
  atomic_store(&s_track_end_ms, now_ms() | 1); // never 0: 0 means "loaded"
  atomic_store(&s_loaded, 0);
}

static void dec_post_error(player_error_t error) {
  const player_event_t event = {.type = PLAYER_EVENT_ERROR, .error = error};
  post_event(&event);
}

// Stop the loaded track, if there is one: no more decoding, the sound faded
// out, the ring emptied. Posts nothing.
static void dec_stop_track(void) {
  if (!s_dec.active) {
    return;
  }
  dec_close_source();
  // Always through the output task, even if it was never given this stream:
  // the ring may hold the start of it, and only the output task resets the
  // ring.
  out_send(OMSG_FLUSH);
  xSemaphoreTake(s_flushed, portMAX_DELAY);
  dec_unload();
}

// Hand the stream in the ring to the output task and announce the track.
static void dec_start_output(bool eos) {
  dec_t *d = &s_dec;
  d->out_started = true;

  const player_event_t event = {
      .type = PLAYER_EVENT_STARTED,
      .duration_ms = d->duration_ms,
      .sample_rate = d->rate,
      .bits = d->bits,
      .channels = d->channels,
  };
  post_event(&event);

  const omsg_t m = {
      .type = OMSG_START,
      .gen = d->gen,
      .rate = d->rate,
      .format = d->format,
      .channels = d->channels,
      .paused = d->paused,
      .eos = eos,
  };
  xQueueSend(s_out_q, &m, portMAX_DELAY);
}

// Decoding is over: the file ended (`error` is NO_ERROR) or something went
// wrong. What is already in the ring is still played.
static void dec_finish(int error) {
  dec_t *d = &s_dec;
  dec_close_source();
  if (d->sent == 0) {
    // Nothing was ever buffered, so there is nothing to play out. A file
    // that decodes to no audio at all counts as undecodable.
    dec_post_error(error == NO_ERROR ? PLAYER_ERROR_DECODE
                                     : (player_error_t)error);
    dec_unload();
    return;
  }
  d->end_error = error;
  if (d->out_started) {
    out_send(OMSG_EOS);
  } else {
    dec_start_output(true); // a track shorter than the prefill
  }
}

// What the output task has to report about the loaded track.
static void dec_check_output(void) {
  dec_t *d = &s_dec;
  if (!d->active || !d->out_started) {
    return;
  }
  if (atomic_load(&s_out_failed) == d->gen) {
    ESP_LOGE(TAG, "The audio link failed");
    dec_stop_track();
    dec_post_error(PLAYER_ERROR_IO);
    return;
  }
  if (!d->decoding && atomic_load(&s_out_done) == d->gen) {
    uint32_t frames = atomic_load(&s_frames_played);
    if (d->end_error == NO_ERROR) {
      atomic_store(&s_ended_frames, frames);
      atomic_store(&s_ended_expected, d->expected);
      const player_event_t event = {
          .type = PLAYER_EVENT_ENDED,
          .position_ms = (uint32_t)((uint64_t)frames * 1000 / d->rate),
          .duration_ms = d->duration_ms,
          .sample_rate = d->rate,
          .bits = d->bits,
          .channels = d->channels,
      };
      post_event(&event);
    } else {
      dec_post_error((player_error_t)d->end_error);
    }
    dec_unload();
  }
}

static bool rate_supported(uint32_t rate) {
  return rate >= AUDIO_OUT_RATE_MIN && rate <= AUDIO_OUT_RATE_MAX;
}

// Open `path` and get ready to decode it. Returns NO_ERROR or the reason it
// cannot be played; on failure nothing is left open.
static int dec_open(const char *path) {
  dec_t *d = &s_dec;

  const char *dot = strrchr(path, '.');
  if (!dot || strcasecmp(dot, ".flac") != 0) {
    return PLAYER_ERROR_UNSUPPORTED;
  }

  d->file = fopen(path, "rb");
  if (!d->file) {
    return errno == ENOENT ? PLAYER_ERROR_NOT_FOUND : PLAYER_ERROR_IO;
  }

  // The size says which read carries the file's last bytes: the decoder must
  // be told (`eos`), or it keeps the final block back.
  struct stat st;
  if (fstat(fileno(d->file), &st) != 0 || st.st_size < 0) {
    dec_close_source();
    return PLAYER_ERROR_IO;
  }
  d->file_left = (uint32_t)st.st_size;

  // The header gives the length (for ENDED and the duration) and lets a file
  // that cannot be played be refused before any audio is touched. The decoder
  // parses the file from its first byte, so go back there afterwards.
  flac_header_t header;
  if (flac_header_read(d->file, &header) != FLAC_HEADER_OK) {
    dec_close_source();
    return PLAYER_ERROR_DECODE;
  }
  if (!rate_supported(header.sample_rate) ||
      (header.bits_per_sample != 16 && header.bits_per_sample != 24) ||
      (header.channels != 1 && header.channels != 2)) {
    dec_close_source();
    return PLAYER_ERROR_UNSUPPORTED;
  }
  if (fseek(d->file, 0, SEEK_SET) != 0) {
    dec_close_source();
    return PLAYER_ERROR_IO;
  }
  // The frame count is kept in 32 bits (over 12 hours at 96 kHz). A longer
  // file plays, with its length treated as unknown.
  d->expected =
      header.total_frames <= UINT32_MAX ? (uint32_t)header.total_frames : 0;
  d->duration_ms = d->expected ? flac_header_duration_ms(&header) : 0;

  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
      .dec_cfg = NULL,
      .cfg_size = 0,
      .use_frame_dec = false,
  };
  esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &d->dec);
  if (err != ESP_AUDIO_ERR_OK) {
    d->dec = NULL;
    dec_close_source();
    return err == ESP_AUDIO_ERR_MEM_LACK ? PLAYER_ERROR_NO_MEM
                                         : PLAYER_ERROR_DECODE;
  }
  return NO_ERROR;
}

// The PLAY command. Takes ownership of `path`.
static void dec_play(char *path) {
  dec_t *d = &s_dec;
  dec_stop_track();

  xSemaphoreTake(s_path_lock, portMAX_DELAY);
  free(s_path);
  s_path = path;
  xSemaphoreGive(s_path_lock);

  d->gen = ++d->gen_counter;
  d->out_started = false;
  d->paused = false;
  d->format_known = false;
  d->pending = 0;
  d->sent = 0;
  d->end_error = NO_ERROR;
  d->busy_us = 0;
  d->raw = (esp_audio_simple_dec_raw_t){0};

  // The output task has no stream now, so its counters are ours to reset.
  atomic_store(&s_frames_played, 0);
  atomic_store(&s_ring_low, RING_BYTES);
  atomic_store(&s_out_busy_us, 0);
  atomic_store(&s_ring_tracked, 0);
  atomic_store(&s_dec_busy_us, 0);
  atomic_store(&s_rate, 0);
  atomic_store(&s_track_start_ms, now_ms());
  atomic_store(&s_track_end_ms, 0);

  d->active = true;
  atomic_store(&s_loaded, 1);

  int error = dec_open(path);
  if (error != NO_ERROR) {
    dec_post_error((player_error_t)error);
    dec_unload();
    return;
  }
  d->decoding = true;
}

// The first block has been decoded: the decoder now knows the stream's real
// format. Returns NO_ERROR or why it cannot be played.
static int dec_learn_format(void) {
  dec_t *d = &s_dec;
  esp_audio_simple_dec_info_t info;
  if (esp_audio_simple_dec_get_info(d->dec, &info) != ESP_AUDIO_ERR_OK) {
    return PLAYER_ERROR_DECODE;
  }
  // The decoder packs samples: 2 bytes for 16-bit, 3 for 24-bit.
  if (info.bits_per_sample == 16) {
    d->format = PCM_FMT_S16;
  } else if (info.bits_per_sample == 24) {
    d->format = PCM_FMT_S24_PACKED;
  } else {
    return PLAYER_ERROR_UNSUPPORTED;
  }
  if ((info.channel != 1 && info.channel != 2) ||
      !rate_supported(info.sample_rate)) {
    return PLAYER_ERROR_UNSUPPORTED;
  }
  d->rate = info.sample_rate;
  d->bits = info.bits_per_sample;
  d->channels = info.channel;
  d->frame_bytes = (uint32_t)pcm_bytes_per_sample(d->format) * d->channels;
  d->prefill_bytes = d->rate / (1000 / PREFILL_MS) * d->frame_bytes;
  d->format_known = true;
  atomic_store(&s_rate, d->rate);
  return NO_ERROR;
}

// The decoder wants `needed` bytes for one block. Internal RAM if there is
// any, because that is what keeps decoding fast; PSRAM rather than failing.
static bool dec_grow_out(uint32_t needed) {
  dec_t *d = &s_dec;
  uint8_t *bigger =
      heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!bigger) {
    bigger = heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!bigger) {
    return false;
  }
  heap_caps_free(d->out);
  d->out = bigger;
  d->out_len = needed;
  return true;
}

// Is there room in the ring for `bytes`? If not, arrange to be woken when
// there is.
static bool dec_ring_has_room(uint32_t bytes) {
  if (xStreamBufferSpacesAvailable(s_ring) >= bytes) {
    return true;
  }
  // The ring is as full as it gets: from here on its lowest fill says how
  // much reserve the track had.
  atomic_store(&s_ring_tracked, 1);
  atomic_store(&s_dec_need, bytes);
  if (xStreamBufferSpacesAvailable(s_ring) >= bytes) {
    atomic_store(&s_dec_need, 0); // it appeared while we were asking
    return true;
  }
  return false;
}

// Do the next piece of work on the loaded track: send a decoded block to the
// ring, read from the file, or decode. Returns false when there is nothing to
// do until a message arrives.
static bool dec_step(void) {
  dec_t *d = &s_dec;
  if (!d->decoding) {
    return false;
  }

  if (d->pending > 0) {
    if (!dec_ring_has_room(d->pending)) {
      return false;
    }
    xStreamBufferSend(s_ring, d->out, d->pending, 0);
    d->sent += d->pending;
    d->pending = 0;
    if (atomic_exchange(&s_out_starving, 0) != 0) {
      const omsg_t wake = {.type = OMSG_WAKE};
      xQueueSend(s_out_q, &wake, 0); // if full, it is awake anyway
    }
    if (!d->out_started && d->sent >= d->prefill_bytes) {
      dec_start_output(false);
    }
    return true;
  }

  if (d->raw.len == 0) {
    if (d->file_left == 0) {
      dec_finish(NO_ERROR);
      return true;
    }
    // Only now may the input buffer be overwritten: the decoder keeps
    // pointers into it for as long as it has unconsumed input.
    size_t want = d->file_left < IN_CHUNK ? d->file_left : IN_CHUNK;
    size_t got = fread(d->in, 1, want, d->file);
    if (got != want) {
      dec_finish(PLAYER_ERROR_IO);
      return true;
    }
    d->file_left -= (uint32_t)got;
    d->raw.buffer = d->in;
    d->raw.len = (uint32_t)got;
    // FLAC needs this to give up its last frame.
    d->raw.eos = (d->file_left == 0);
    return true;
  }

  esp_audio_simple_dec_out_t frame = {.buffer = d->out, .len = d->out_len};
  esp_audio_err_t err = esp_audio_simple_dec_process(d->dec, &d->raw, &frame);
  if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
    // A block bigger than the buffer: replace the buffer and ask again.
    if (frame.needed_size <= d->out_len || frame.needed_size > RING_BYTES) {
      dec_finish(PLAYER_ERROR_DECODE);
    } else if (!dec_grow_out(frame.needed_size)) {
      dec_finish(PLAYER_ERROR_NO_MEM);
    }
    return true;
  }
  if (err != ESP_AUDIO_ERR_OK) {
    dec_finish(PLAYER_ERROR_DECODE);
    return true;
  }

  if (frame.decoded_size > 0) {
    if (!d->format_known) {
      int error = dec_learn_format();
      if (error != NO_ERROR) {
        dec_finish(error);
        return true;
      }
    }
    // Whole frames only, or everything after would be out of step.
    d->pending = frame.decoded_size - frame.decoded_size % d->frame_bytes;
  }

  // Progress is input taken OR a block returned: the parser often holds
  // several frames and hands them out one per call while taking no new
  // input. Only when neither happens has the decoder stalled; give up on
  // this chunk then, so that the loop cannot spin.
  if (d->raw.consumed == 0 && frame.decoded_size == 0) {
    d->raw.len = 0;
  } else {
    d->raw.buffer += d->raw.consumed;
    d->raw.len -= d->raw.consumed;
  }
  return true;
}

static void dec_handle(cmd_t *c) {
  dec_t *d = &s_dec;
  switch (c->type) {
  case CMD_PLAY:
    dec_play(c->path); // start position: Task 8
    break;

  case CMD_PAUSE:
    if (d->active && !d->paused) {
      d->paused = true;
      if (d->out_started) {
        out_send(OMSG_PAUSE);
      } // otherwise START will say so
    }
    break;

  case CMD_RESUME:
    if (d->active && d->paused) {
      d->paused = false;
      if (d->out_started) {
        out_send(OMSG_RESUME);
      }
    }
    break;

  case CMD_STOP:
    dec_stop_track();
    break;

  case CMD_STALL:
    // Stand in for a card that has stopped answering: nothing is decoded and
    // no command is served until the time is up.
    vTaskDelay(pdMS_TO_TICKS(c->ms));
    break;

  case CMD_SEEK: // Task 8
  case CMD_WAKE:
    break;
  }
}

static void decoder_task(void *arg) {
  (void)arg;
  dec_t *d = &s_dec;
  bool busy = false;

  for (;;) {
    // Commands first, and all of them, before any more decoding. Sleep on
    // the queue when the track has nothing to do (or there is no track).
    cmd_t c;
    if (xQueueReceive(s_cmd_q, &c, busy ? 0 : portMAX_DELAY) == pdTRUE) {
      dec_check_output(); // a track that has just ended ends before the command
      dec_handle(&c);
      busy = true;
      continue;
    }
    dec_check_output();

    int64_t t0 = esp_timer_get_time();
    busy = dec_step();
    if (busy) {
      d->busy_us += (uint32_t)(esp_timer_get_time() - t0);
      atomic_store(&s_dec_busy_us, d->busy_us);
    }
  }
}

// ============================================================================
// Public interface
// ============================================================================

esp_err_t player_start(QueueHandle_t events) {
  if (!events) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_cmd_q) {
    return ESP_ERR_INVALID_STATE;
  }
  dec_t *d = &s_dec;
  s_events = events;
  register_codecs();

  // The ring's storage is in PSRAM. One byte more than the capacity: a stream
  // buffer always keeps one byte free.
  uint8_t *ring =
      heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // The input buffer receives data straight from the SD card, so it must be
  // internal DMA-capable RAM: the ESP32's SDMMC DMA cannot reach PSRAM.
  d->in = heap_caps_malloc(IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  d->out = heap_caps_malloc(OUT_INIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  d->out_len = OUT_INIT;
  s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
  s_out_q = xQueueCreate(8, sizeof(omsg_t));
  s_flushed = xSemaphoreCreateBinary();
  s_path_lock = xSemaphoreCreateMutex();
  if (!ring || !d->in || !d->out || !s_cmd_q || !s_out_q || !s_flushed ||
      !s_path_lock) {
    ESP_LOGE(TAG, "Out of memory for the player");
    return ESP_ERR_NO_MEM;
  }
  s_ring = xStreamBufferCreateStatic(RING_BYTES + 1, 1, ring, &s_ring_control);

  if (xTaskCreatePinnedToCore(output_task, "output", OUTPUT_TASK_STACK, NULL,
                              OUTPUT_TASK_PRIORITY, &s_output_task,
                              AUDIO_CORE) != pdPASS ||
      xTaskCreatePinnedToCore(decoder_task, "decoder", DECODER_TASK_STACK, NULL,
                              DECODER_TASK_PRIORITY, &s_decoder_task,
                              AUDIO_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Cannot create the player tasks");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

// Queue a command. `pause_wanted` is what the command makes of the pause
// request (see s_pause_wanted), or -1 to leave it alone.
static esp_err_t send_command(const cmd_t *c, int pause_wanted) {
  if (!s_cmd_q) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xQueueSend(s_cmd_q, c, 0) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (pause_wanted >= 0) {
    atomic_store(&s_pause_wanted, (unsigned)pause_wanted);
  }
  return ESP_OK;
}

esp_err_t player_play(const char *path, uint32_t start_ms) {
  if (!path || path[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  cmd_t c = {.type = CMD_PLAY, .path = strdup(path), .ms = (int32_t)start_ms};
  if (!c.path) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = send_command(&c, 0);
  if (err != ESP_OK) {
    free(c.path);
  }
  return err;
}

esp_err_t player_pause(void) {
  const cmd_t c = {.type = CMD_PAUSE};
  return send_command(&c, 1);
}

esp_err_t player_resume(void) {
  const cmd_t c = {.type = CMD_RESUME};
  return send_command(&c, 0);
}

esp_err_t player_stop(void) {
  const cmd_t c = {.type = CMD_STOP};
  return send_command(&c, 0);
}

esp_err_t player_seek_rel(int32_t delta_ms) {
  const cmd_t c = {.type = CMD_SEEK, .ms = delta_ms};
  return send_command(&c, -1);
}

esp_err_t player_debug_stall(uint32_t ms) {
  if (ms > PLAYER_DEBUG_STALL_MAX_MS) {
    return ESP_ERR_INVALID_ARG;
  }
  const cmd_t c = {.type = CMD_STALL, .ms = (int32_t)ms};
  return send_command(&c, -1);
}

esp_err_t player_set_volume(int level) {
  if (level < 0) {
    level = 0;
  } else if (level > PCM_VOLUME_MAX) {
    level = PCM_VOLUME_MAX;
  }
  atomic_store(&s_volume, level);
  return ESP_OK;
}

void player_get_status(player_status_t *out) {
  memset(out, 0, sizeof(*out));
  if (!atomic_load(&s_loaded)) {
    out->state = PLAYER_STATE_IDLE;
  } else {
    out->state = atomic_load(&s_pause_wanted) ? PLAYER_STATE_PAUSED
                                               : PLAYER_STATE_PLAYING;
  }
  out->volume = atomic_load(&s_volume);
  uint32_t rate = atomic_load(&s_rate);
  if (rate != 0) {
    out->position_ms =
        (uint32_t)((uint64_t)atomic_load(&s_frames_played) * 1000 / rate);
  }
  out->ring_size = RING_BYTES;
  out->ring_fill = s_ring ? xStreamBufferBytesAvailable(s_ring) : 0;
  out->ring_low =
      atomic_load(&s_ring_tracked) ? (int32_t)atomic_load(&s_ring_low) : -1;
  out->underruns = atomic_load(&s_underruns);
  out->ended_frames = atomic_load(&s_ended_frames);
  out->ended_expected = atomic_load(&s_ended_expected);

  uint32_t end = atomic_load(&s_track_end_ms);
  out->wall_ms = (end != 0 ? end : now_ms()) - atomic_load(&s_track_start_ms);
  out->decoder_busy_ms = atomic_load(&s_dec_busy_us) / 1000;
  out->output_busy_ms = atomic_load(&s_out_busy_us) / 1000;
  if (s_decoder_task) {
    out->decoder_stack_free = uxTaskGetStackHighWaterMark(s_decoder_task);
    out->output_stack_free = uxTaskGetStackHighWaterMark(s_output_task);
  }
}

void player_get_path(char *out, size_t out_size) {
  if (out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!s_path_lock) {
    return;
  }
  xSemaphoreTake(s_path_lock, portMAX_DELAY);
  if (s_path) {
    strlcpy(out, s_path, out_size);
  }
  xSemaphoreGive(s_path_lock);
}
