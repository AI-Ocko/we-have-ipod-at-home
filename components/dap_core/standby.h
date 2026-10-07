#pragma once

#include "keys.h"
#include <stdbool.h>
#include <stdint.h>

// When the backlight goes off, which key press only wakes it, and the sleep
// timer with its fade. Times are milliseconds from any one clock.

#define STANDBY_FADE_MS 10000

typedef enum {
  STANDBY_BACKLIGHT_15S,
  STANDBY_BACKLIGHT_30S, // the default
  STANDBY_BACKLIGHT_60S,
  STANDBY_BACKLIGHT_ON,  // never goes off
  STANDBY_BACKLIGHT_COUNT,
} standby_backlight_t;

typedef enum {
  STANDBY_SLEEP_OFF,
  STANDBY_SLEEP_15,
  STANDBY_SLEEP_30,
  STANDBY_SLEEP_60,
  STANDBY_SLEEP_COUNT,
} standby_sleep_t;

typedef struct {
  standby_backlight_t backlight;
  bool lit;
  int64_t last_key_ms;
  char swallowing;        // the key whose press woke the backlight, or 0
  standby_sleep_t sleep;  // the value last chosen
  int64_t sleep_at_ms;    // when the timer expires; INT64_MAX when off
  int64_t fade_from_ms;   // when the fade began; INT64_MAX when not fading
} standby_t;

// Backlight on, timer off.
void standby_init(standby_t *s, standby_backlight_t backlight, int64_t now_ms);

// Every key event goes through here first. True if the UI should act on
// it; false if it is the press that woke the backlight or an event of that
// same press (long, repeat, release). Any press also cancels a fade (the
// timer is then off) and counts as activity.
bool standby_key(standby_t *s, char key, key_event_type_t type,
                 int64_t now_ms);

void standby_set_backlight(standby_t *s, standby_backlight_t backlight,
                           int64_t now_ms);

// Choose a value; anything but OFF counts from now. It also ends a fade in
// progress (standby_fade_level then gives the volume setting again): the UI
// calls it with STANDBY_SLEEP_OFF when playback is paused, stopped or runs
// out during the fade.
void standby_set_sleep(standby_t *s, standby_sleep_t sleep, int64_t now_ms);

// The value after the one last chosen, for the Settings row: off, 15, 30,
// 60, off, ...
standby_sleep_t standby_next_sleep(const standby_t *s);

// Minutes left, rounded up; 0 when the timer is off (or fading).
uint32_t standby_sleep_minutes(const standby_t *s, int64_t now_ms);

typedef enum {
  STANDBY_ACT_LIGHT_OFF = 1, // turn the backlight off
  STANDBY_ACT_FADE = 2,      // the fade has begun: apply standby_fade_level
  STANDBY_ACT_STOP = 4,      // the fade is over: stop playback, restore the
                             // volume setting, backlight off
} standby_act_t;

// Move time on. `playing` says whether a track is playing (a timer that
// expires with nothing playing just turns the backlight off). Returns a set
// of standby_act_t to carry out.
uint32_t standby_tick(standby_t *s, bool playing, int64_t now_ms);

// During the fade: the volume to play at, from `volume` down to 0 in a
// straight line over STANDBY_FADE_MS. `volume` when not fading.
int standby_fade_level(const standby_t *s, int volume, int64_t now_ms);

// The next time standby_tick has something to do (including each step of
// the fade's volume), or INT64_MAX.
int64_t standby_deadline(const standby_t *s, int volume, int64_t now_ms);
