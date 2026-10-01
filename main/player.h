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
// ESP_ERR_TIMEOUT and has no effect. Events of a track that is being replaced
// can still arrive until the player has taken the new `play` off its queue;
// after that, none do.
esp_err_t player_play(const char *path, uint32_t start_ms); // replaces whatever is playing
esp_err_t player_pause(void);
esp_err_t player_resume(void);
esp_err_t player_stop(void);
esp_err_t player_seek_rel(int32_t delta_ms); // implemented in Task 8; until then, ignored
esp_err_t player_set_volume(int level);      // 0..20, clamped
