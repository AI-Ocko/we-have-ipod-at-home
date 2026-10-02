#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "keytest.h"
#include "play_queue.h"
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

// Set up the LCD and its user-defined characters, create the queues, the
// play queue's memory (32 KB of PSRAM) and the UI task. Call once, after
// state_task_start (the UI starts from the saved settings) and before the
// keypad, the player, the console and the library task are started: they
// are given the queues. Running without an LCD is not an error.
// ESP_ERR_NO_MEM if any of the memory cannot be had: the LCD then says "Out
// of memory", it is logged, and there is no user interface at all; the
// caller should stop there.
esp_err_t ui_start(ui_queues_t *queues);

// Tell the UI whether start-up is still waiting for a card that mounts.
// Called by the task that mounts the card: once with true if the first
// attempt fails, once with false when a later one works.
void ui_card_waiting(bool waiting);

// Stop playback, as a user of the player would want it stopped: the player
// stops, the track stays on Now Playing as the one D starts (a queue track
// from where it had got to, any other from its beginning), and the rest of
// the queue does not follow. For the console's `stop`. (player_stop
// alone leaves the UI believing the track is still playing: the player
// reports no event for a stop.) Returns at once; from any task.
void ui_stop(void);

// The LCD's user-defined characters, as the codes that go in a screen cell.
// The play triangle is the key test screen's glyph, so that screen needs no
// character of its own. Seven of the eight are used. (The shuffle mark is a
// plain `S`.)
#define UI_GLYPH_PLAY KEYTEST_GLYPH_CODE // 8
#define UI_GLYPH_PAUSE 9
#define UI_GLYPH_BAR_FULL 10  // a cell of the volume bar: two levels
#define UI_GLYPH_BAR_HALF 11  // one level
#define UI_GLYPH_BAR_EMPTY 12 // none
#define UI_GLYPH_LOOP_ALL 13  // Now Playing's loop mark: the whole queue
#define UI_GLYPH_LOOP_ONE 14  // the current track

// What the play queue holds, for the console's `queue`: numbers only. The
// track numbers belong to the index whose checksum (library_checksum) is
// `index`, and mean nothing in any other.
#define UI_QUEUE_REPORT_MAX 50
typedef struct {
  uint32_t count;    // tracks in the queue
  uint32_t position; // of the current track in the play order (0 if empty)
  bool shuffle;
  play_loop_t loop;
  uint32_t index;
  uint32_t from;   // the place in the play order of tracks[0]
  uint32_t listed; // how many of `tracks` are filled
  uint16_t tracks[UI_QUEUE_REPORT_MAX];
} ui_queue_report_t;

// Ask the UI task for `count` entries of the queue's play order (at most
// UI_QUEUE_REPORT_MAX) starting at place `from`; UINT32_MAX for `from` means
// the current track's place. Waits for the UI task to answer, which it does
// at the end of its next pass: for the console task, one call at a time.
// False if there is no UI task or it has not answered within a second.
bool ui_queue_report(uint32_t from, uint32_t count, ui_queue_report_t *out);

// How the UI task is doing, for the console's `stat`.
typedef struct {
  uint32_t stack_free;  // bytes of the task's stack never used
  uint32_t passes;      // times round its loop: each is an event or a timeout
  uint32_t lcd_updates; // times the LCD was sent something
} ui_stats_t;

void ui_get_stats(ui_stats_t *out);
