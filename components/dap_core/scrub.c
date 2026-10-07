#include "scrub.h"

// Design notes
//
// The target stops at 0 and at the limit: it does not move past them, but
// it is never pulled back by them either. A forward scrub never moves the
// target below where playback was, so one started in the last second (or
// past a length that was only an estimate) stays where it started; a rewind
// never moves it above where playback was, and one started past the limit
// begins from the limit. The limit is SCRUB_END_MARGIN_MS before the end
// when the length is known (0 for a track shorter than that), and the
// largest time the type holds when it is not. Sums are made in 64 bits, so
// nothing wraps.
//
// The step is chosen by how long ago the long press came, not by counting
// repeats: the keypad's repeat interval changes while the key is held, and
// a lost repeat must not shift the change of speed.

static uint32_t limit_of(const scrub_t *s) {
  if (s->duration_ms == 0) {
    return UINT32_MAX;
  }
  return s->duration_ms > SCRUB_END_MARGIN_MS
             ? s->duration_ms - SCRUB_END_MARGIN_MS
             : 0;
}

static uint32_t clamp(const scrub_t *s, uint64_t ms) {
  uint32_t limit = limit_of(s);
  return ms < limit ? (uint32_t)ms : limit;
}

void scrub_start(scrub_t *s, int direction, uint32_t elapsed_ms,
                 uint32_t duration_ms, int64_t now_ms) {
  s->active = true;
  s->direction = direction >= 0 ? +1 : -1;
  s->duration_ms = duration_ms;
  s->since_ms = now_ms;
  s->target_ms = s->direction > 0 ? elapsed_ms : clamp(s, elapsed_ms);
}

void scrub_repeat(scrub_t *s, int64_t now_ms) {
  if (!s->active) {
    return;
  }
  bool fast = s->since_ms <= INT64_MAX - SCRUB_FAST_AFTER_MS &&
              now_ms >= s->since_ms + SCRUB_FAST_AFTER_MS;
  uint32_t step = fast ? SCRUB_FAST_STEP_MS : SCRUB_STEP_MS;
  if (s->direction > 0) {
    if (s->target_ms < limit_of(s)) { // at or past the limit it stays
      s->target_ms = clamp(s, (uint64_t)s->target_ms + step);
    }
  } else {
    s->target_ms = s->target_ms > step ? s->target_ms - step : 0;
  }
}

uint32_t scrub_finish(scrub_t *s) {
  s->active = false;
  return s->target_ms;
}
