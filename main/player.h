#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

// The player: a decoder task and an output task with a buffer between them.
// It is driven by the commands below and reports what happens as events. How
// it works inside is described at the top of player.c. What the player can be
// asked about itself (state, position, buffer fill) is in player_status.h.

typedef enum {
  PLAYER_EVENT_STARTED,  // a track began: duration_ms, sample_rate, bits, channels
  PLAYER_EVENT_POSITION, // every 500 ms of audio played: position_ms
  PLAYER_EVENT_ENDED,    // the track played to its end
  PLAYER_EVENT_ERROR,    // the track could not be played: error
} player_event_type_t;

typedef enum {
  PLAYER_ERROR_NOT_FOUND,
  PLAYER_ERROR_UNSUPPORTED,
  PLAYER_ERROR_DECODE,
  PLAYER_ERROR_IO,
  PLAYER_ERROR_NO_MEM,
} player_error_t;

typedef struct {
  player_event_type_t type;
  uint32_t play_id; // which player_play call this event belongs to
  uint32_t position_ms;
  uint32_t duration_ms; // 0 if unknown
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;
  player_error_t error;
} player_event_t;

// Create the decoder and output tasks and the buffer between them. Events are
// posted to `events`; if that queue is full the event is dropped. Call once.
esp_err_t player_start(QueueHandle_t events);

// Each of these queues a command and returns at once. They are safe to call
// in any state and from any task.
//
// A command that cannot be queued (eight are already waiting) is refused with
// ESP_ERR_TIMEOUT and has no effect.
//
// Every event carries the number of the `play` it is about (`play_id`). The
// player posts events of a track that is being replaced or stopped until it
// has taken the new `play` or the `stop` off its queue, and none after that;
// but the events are queued too, so those already posted are read later
// still. After a `play` or a `stop`, then, events of the earlier track can
// still be read: at most as many as the event queue holds, and all of them
// before the first event of the new track. They carry the earlier track's
// number. Whoever reads the events compares `play_id` with the number of the
// track it is waiting for and ignores the rest; after a `stop` it is waiting
// for none.

// Queue a track; it replaces whatever is playing. `play_id`, if not NULL,
// receives the number that every event about this track will carry. Numbers
// are never 0 and never repeat within a run. (They start at 1 and go up by
// one per call, from whichever task; the counter is 32 bits, so "never" is
// four thousand million tracks.) The number is set before the command is
// queued, so the caller has it before any event about the track can arrive.
// If the call fails, no event will ever carry its number and `*play_id` is 0.
esp_err_t player_play(const char *path, uint32_t start_ms, uint32_t *play_id);
esp_err_t player_pause(void);
esp_err_t player_resume(void);
esp_err_t player_stop(void);
esp_err_t player_seek_rel(int32_t delta_ms); // implemented in Task 8; until then, ignored
esp_err_t player_set_volume(int level);      // 0..20, clamped
