#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

// 4x4 matrix keypad.
//
// Rows are open-drain outputs: one is pulled low at a time and the rest are
// left floating, so two keys pressed together can never short two outputs.
// Columns are inputs with external 10k pull-ups to 3.3V; a pressed key pulls
// its column low through the active row.

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 4

typedef struct {
  int rows[KEYPAD_ROWS];
  int cols[KEYPAD_COLS];
} keypad_pins_t;

// Configure the pins. Call once, before anything else here.
esp_err_t keypad_init(const keypad_pins_t *pins);

// Read every key once. Bit (row * 4 + column) is set while that key is down.
uint16_t keypad_scan(void);

// With no row driven, every column should read high. Returns a bit per column
// (bit 0 = column 1) that reads low instead: its pull-up resistor is missing
// or miswired. Only meaningful while no key is being pressed.
uint8_t keypad_low_columns(void);

// Start the task that scans every 10 ms and posts key_event_t items (see
// keys.h) to `events`. If the queue is full the event is dropped: the scan
// must never wait for a slow reader.
esp_err_t keypad_start(QueueHandle_t events);
