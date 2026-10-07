#include "standby.h"

// Design notes
//
// There is no timer here. The caller asks standby_deadline when to wake and
// calls standby_tick then (and after anything that changes `playing`); keys
// go through standby_key as they come. Every call is a few comparisons.
//
// THE KEY THAT WAKES THE LIGHT
//
// A press while dark lights it and records the key in `swallowing`. Every
// later long press, repeat and release of that key is swallowed; its release
// ends the swallowing. The keypad recognises one key at a time and always
// sends a key's release before the next key's press, so the next press, of
// any key, also ends it: a lost release cannot swallow a later press. Events
// of another key are not the waking press's and are passed on.
//
// Every key event counts as activity, not only presses: a key held down to
// scroll or fast-forward keeps the light on for as long as it is held.
//
// THE SLEEP TIMER
//
// `sleep_at_ms` is INT64_MAX when the timer is off, `fade_from_ms` when no
// fade is running; at most one of them is set. When the timer runs out it
// goes off: `sleep` is then STANDBY_SLEEP_OFF too ("it then resets to off",
// spec section 9), so the Settings row and the next value agree with what
// the user sees. The same when a key cancels the fade, or the caller does
// with standby_set_sleep (OFF when playback is paused, stopped or runs out).
//
// THE FADE
//
// The level at `e` ms into the fade is v * (FADE - e) / FADE rounded up:
// level v until FADE / v, ..., level 1 for the last FADE / v, and 0 only at
// the end, which is when standby_tick says STOP. The level drops from L to
// L - 1 at the first e with v * (FADE - e) <= (L - 1) * FADE, which is
// e = FADE - floor((L - 1) * FADE / v); standby_deadline gives that, so the
// caller wakes once per step. With volume 0 or 1 there is no step on the way,
// only the end.
//
// Times are added with saturation: a time at the far end of the clock gives
// INT64_MAX ("never") instead of wrapping.

static const int64_t BACKLIGHT_MS[STANDBY_BACKLIGHT_COUNT] = {15000, 30000,
                                                              60000, INT64_MAX};

static const int64_t SLEEP_MS[STANDBY_SLEEP_COUNT] = {0, 15 * 60000, 30 * 60000,
                                                      60 * 60000};

// a + b for b >= 0, INT64_MAX if that is past the end of the clock.
static int64_t add_ms(int64_t a, int64_t b) {
  return a > INT64_MAX - b ? INT64_MAX : a + b;
}

static bool fading(const standby_t *s) { return s->fade_from_ms != INT64_MAX; }

static void timer_off(standby_t *s) {
  s->sleep = STANDBY_SLEEP_OFF;
  s->sleep_at_ms = INT64_MAX;
  s->fade_from_ms = INT64_MAX;
}

// When the light goes off by itself, or INT64_MAX: not lit, or always on.
static int64_t light_off_at(const standby_t *s) {
  if (!s->lit || (unsigned)s->backlight >= STANDBY_BACKLIGHT_ON) {
    return INT64_MAX;
  }
  return add_ms(s->last_key_ms, BACKLIGHT_MS[s->backlight]);
}

static int64_t fade_end(const standby_t *s) {
  return add_ms(s->fade_from_ms, STANDBY_FADE_MS);
}

// How far into the fade `now_ms` is: 0 .. STANDBY_FADE_MS.
static int64_t fade_elapsed(const standby_t *s, int64_t now_ms) {
  if (now_ms <= s->fade_from_ms) {
    return 0;
  }
  if (now_ms >= fade_end(s)) {
    return STANDBY_FADE_MS;
  }
  return now_ms - s->fade_from_ms; // between the two: no overflow
}

static int64_t clamp_volume(int volume) { return volume > 0 ? volume : 0; }

void standby_init(standby_t *s, standby_backlight_t backlight, int64_t now_ms) {
  s->backlight = backlight;
  s->lit = true;
  s->last_key_ms = now_ms;
  s->swallowing = 0;
  timer_off(s);
}

bool standby_key(standby_t *s, char key, key_event_type_t type,
                 int64_t now_ms) {
  s->last_key_ms = now_ms;
  if (type == KEY_PRESS) {
    if (fading(s)) {
      timer_off(s); // the caller puts the volume back
    }
    if (!s->lit) {
      s->lit = true;
      s->swallowing = key;
      return false;
    }
    s->swallowing = 0;
    return true;
  }
  if (s->swallowing != 0 && key == s->swallowing) {
    if (type == KEY_RELEASE) {
      s->swallowing = 0;
    }
    return false;
  }
  return true;
}

void standby_set_backlight(standby_t *s, standby_backlight_t backlight,
                           int64_t now_ms) {
  // It does not light a dark screen: only a key press does.
  s->backlight = backlight;
  s->last_key_ms = now_ms;
}

void standby_set_sleep(standby_t *s, standby_sleep_t sleep, int64_t now_ms) {
  timer_off(s);
  if ((unsigned)sleep < STANDBY_SLEEP_COUNT && sleep != STANDBY_SLEEP_OFF) {
    s->sleep = sleep;
    s->sleep_at_ms = add_ms(now_ms, SLEEP_MS[sleep]);
  }
}

standby_sleep_t standby_next_sleep(const standby_t *s) {
  unsigned next = (unsigned)s->sleep + 1;
  return next < STANDBY_SLEEP_COUNT ? (standby_sleep_t)next : STANDBY_SLEEP_OFF;
}

uint32_t standby_sleep_minutes(const standby_t *s, int64_t now_ms) {
  if (s->sleep_at_ms == INT64_MAX || now_ms >= s->sleep_at_ms) {
    return 0;
  }
  // The difference of two int64_t, taken unsigned so it cannot overflow.
  uint64_t left = (uint64_t)s->sleep_at_ms - (uint64_t)now_ms;
  uint64_t minutes = (left + 59999) / 60000;
  return minutes < UINT32_MAX ? (uint32_t)minutes : UINT32_MAX;
}

uint32_t standby_tick(standby_t *s, bool playing, int64_t now_ms) {
  uint32_t acts = 0;
  if (fading(s)) {
    // Over only at its end (or when cancelled: a key press, or
    // standby_set_sleep). `playing` decides nothing here: a gap between two
    // tracks must not cut it short.
    if (now_ms >= fade_end(s)) {
      timer_off(s);
      s->lit = false;
      return STANDBY_ACT_STOP;
    }
  } else if (s->sleep_at_ms != INT64_MAX && now_ms >= s->sleep_at_ms) {
    timer_off(s);
    if (playing) {
      // INT64_MAX means "not fading"; a fade cannot begin there.
      s->fade_from_ms = now_ms < INT64_MAX ? now_ms : INT64_MAX - 1;
      acts |= STANDBY_ACT_FADE;
    } else if (s->lit) {
      s->lit = false;
      acts |= STANDBY_ACT_LIGHT_OFF;
    }
  }
  int64_t off_at = light_off_at(s);
  if (off_at != INT64_MAX && now_ms >= off_at) { // INT64_MAX is never
    s->lit = false;
    acts |= STANDBY_ACT_LIGHT_OFF;
  }
  return acts;
}

int standby_fade_level(const standby_t *s, int volume, int64_t now_ms) {
  if (!fading(s)) {
    return volume;
  }
  int64_t v = clamp_volume(volume);
  int64_t left = STANDBY_FADE_MS - fade_elapsed(s, now_ms);
  return (int)((v * left + STANDBY_FADE_MS - 1) / STANDBY_FADE_MS);
}

int64_t standby_deadline(const standby_t *s, int volume, int64_t now_ms) {
  int64_t deadline = light_off_at(s);
  int64_t timer = s->sleep_at_ms;
  if (fading(s)) {
    timer = fade_end(s);
    int64_t v = clamp_volume(volume);
    int64_t level = standby_fade_level(s, volume, now_ms);
    if (level >= 2) { // the next step is before the end
      timer = add_ms(s->fade_from_ms,
                     STANDBY_FADE_MS - (level - 1) * STANDBY_FADE_MS / v);
    }
  }
  return timer < deadline ? timer : deadline;
}
