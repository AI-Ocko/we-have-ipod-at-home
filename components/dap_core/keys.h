#pragma once

#include <stdbool.h>
#include <stdint.h>

// Turns raw keypad scans into key events. Hardware-free: the keypad driver
// feeds it one 16-bit snapshot per scan, and it handles debouncing, long
// presses and auto-repeat.

#define KEYS_COUNT 16

#define KEYS_DEBOUNCE_SCANS 3     // identical scans before a change is believed
#define KEYS_LONG_MS 600          // hold time before a long press
#define KEYS_REPEAT_MS 150        // repeat interval after a long press
#define KEYS_FAST_REPEAT_MS 50    // repeat interval once held for a while
#define KEYS_FAST_AFTER_MS 2000   // hold time before repeats speed up

typedef enum {
  KEY_PRESS,   // key went down
  KEY_LONG,    // key has been held for KEYS_LONG_MS (sent once)
  KEY_REPEAT,  // key is still held; sent at the repeat interval after KEY_LONG
  KEY_RELEASE, // key came up
} key_event_type_t;

typedef struct {
  key_event_type_t type;
  char key; // one of "123A456B789C*0#D"
} key_event_t;

typedef struct {
  uint16_t candidate; // most recent raw snapshot
  uint8_t count;      // how many scans in a row it has been seen
  uint16_t stable;    // last debounced snapshot
  int8_t active;      // index of the key being tracked, or -1
  bool blocked;       // several keys went down at once; wait for all to lift
  bool long_sent;
  uint32_t pressed_at;
  uint32_t last_fire; // time of the last KEY_LONG or KEY_REPEAT
} keys_t;

void keys_init(keys_t *k);

// The character printed on key `index`, where index = row * 4 + column.
// Returns 0 for an index outside 0..15.
char keys_char(int index);

// Feed one scan. Bit `index` of `raw` is set while that key is down, and
// `now_ms` is a millisecond clock that may wrap around. Returns true and
// fills `event` if something happened; at most one event per call.
//
// Only one key is recognised at a time. A second key pressed while one is
// held is ignored until the first is released. Several keys going down
// together from rest produce nothing until every key has been released.
bool keys_update(keys_t *k, uint16_t raw, uint32_t now_ms, key_event_t *event);
