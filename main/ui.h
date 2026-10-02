#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// The user interface: the task that owns the LCD and acts on the keys.

// The queues the UI task reads, for those who post to them.
typedef struct {
  QueueHandle_t keys;           // key_event_t: the keypad and the console
  QueueHandle_t player_events;  // player_event_t: the player
  QueueHandle_t library_status; // library_status_t, length one: the library
} ui_queues_t;

// Set up the LCD, create the queues and the UI task. Call once, before the
// keypad, the player, the console and the library task are started: they are
// given the queues. Running without an LCD is not an error.
esp_err_t ui_start(ui_queues_t *queues);

// Tell the UI whether start-up is still waiting for a card that mounts.
// Called by the task that mounts the card: once with true if the first
// attempt fails, once with false when a later one works.
void ui_card_waiting(bool waiting);
