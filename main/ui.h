#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "keytest.h"
#include <stdbool.h>
#include <stdint.h>

// The user interface: the task that owns the LCD and acts on the keys. How it
// is put together is described at the top of ui.c.

// The queues the UI task reads, for those who post to them.
typedef struct {
  QueueHandle_t keys;           // key_event_t: the keypad and the console
  QueueHandle_t player_events;  // player_event_t: the player
  QueueHandle_t library_status; // library_status_t, length one: the library
} ui_queues_t;

// Set up the LCD and its user-defined characters, create the queues and the
// UI task. Call once, before the keypad, the player, the console and the
// library task are started: they are given the queues. Running without an
// LCD is not an error. ESP_ERR_NO_MEM if a queue or the task cannot be made;
// there is then no user interface at all.
esp_err_t ui_start(ui_queues_t *queues);

// Tell the UI whether start-up is still waiting for a card that mounts.
// Called by the task that mounts the card: once with true if the first
// attempt fails, once with false when a later one works.
void ui_card_waiting(bool waiting);

// Stop playback, as a user of the player would want it stopped: the player
// stops, the track stays on Now Playing as the one D starts again, and the
// rest of its list does not follow. For the console's `stop`. (player_stop
// alone leaves the UI believing the track is still playing: the player
// reports no event for a stop.) Returns at once; from any task.
void ui_stop(void);

// The LCD's user-defined characters, as the codes that go in a screen cell.
// The play triangle is the key test screen's glyph, so that screen needs no
// character of its own. Five of the eight are used; stage 6 has three more
// (shuffle, loop all, loop one).
#define UI_GLYPH_PLAY KEYTEST_GLYPH_CODE // 8
#define UI_GLYPH_PAUSE 9
#define UI_GLYPH_BAR_FULL 10  // a cell of the volume bar: two levels
#define UI_GLYPH_BAR_HALF 11  // one level
#define UI_GLYPH_BAR_EMPTY 12 // none

// How the UI task is doing, for the console's `stat`.
typedef struct {
  uint32_t stack_free;  // bytes of the task's stack never used
  uint32_t passes;      // times round its loop: each is an event or a timeout
  uint32_t lcd_updates; // times the LCD was sent something
} ui_stats_t;

void ui_get_stats(ui_stats_t *out);
