#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fast-forward and rewind by holding 6 or 4: where the target time has got
// to. Times in milliseconds.

#define SCRUB_STEP_MS 2000       // per repeat at first
#define SCRUB_FAST_STEP_MS 5000  // per repeat once held SCRUB_FAST_AFTER_MS
#define SCRUB_FAST_AFTER_MS 2000 // after the long press
#define SCRUB_END_MARGIN_MS 1000 // the target stops this far before the end

typedef struct {
  bool active;
  int direction;        // +1 forward, -1 back
  uint32_t target_ms;
  uint32_t duration_ms; // 0: unknown, no end limit
  int64_t since_ms;     // when the long press came
} scrub_t;

// The long press: start from where playback is.
void scrub_start(scrub_t *s, int direction, uint32_t elapsed_ms,
                 uint32_t duration_ms, int64_t now_ms);

// One key repeat.
void scrub_repeat(scrub_t *s, int64_t now_ms);

// The release: the time to jump to. The scrub is then over.
uint32_t scrub_finish(scrub_t *s);
