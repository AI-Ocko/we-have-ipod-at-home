#pragma once

#include "keys.h"
#include "screen.h"

// The keypad test screen: shows the last key event and a running count, so
// every key and the long-press and repeat timing can be checked by eye.

// LCD character code of the test glyph drawn in the bottom-right corner. It
// is user-defined character 0, so it also proves custom characters work.
#define KEYTEST_GLYPH_CODE 8

// Bitmap for that glyph (a right-pointing triangle), one byte per pixel row.
extern const unsigned char KEYTEST_GLYPH[8];

// "press", "long", "repeat" or "release".
const char *keytest_event_name(key_event_type_t type);

// Draw the screen. `last` is the most recent event, or NULL if there has not
// been one yet; `count` is how many events there have been.
void keytest_render(screen_t *s, const key_event_t *last, unsigned count);
