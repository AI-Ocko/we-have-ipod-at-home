#include "standby.h"
#include "unity.h"
#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

static standby_t s;

// Any clock will do; not 0, so that nothing works only by accident.
#define T0 ((int64_t)1000000)
#define MINUTE ((int64_t)60000)

static const int64_t TIMEOUT_MS[] = {15000, 30000, 60000};

static void press(char key, int64_t now_ms) {
  TEST_ASSERT_TRUE(standby_key(&s, key, KEY_PRESS, now_ms));
}

static void release(char key, int64_t now_ms) {
  TEST_ASSERT_TRUE(standby_key(&s, key, KEY_RELEASE, now_ms));
}

// Backlight on for good, the sleep timer `sleep` chosen at T0, and time
// moved on to its expiry with a track playing: the fade has begun at the
// returned time.
static int64_t start_fade(void) {
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  int64_t expiry = T0 + 15 * MINUTE;
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, expiry - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE, standby_tick(&s, true, expiry));
  return expiry;
}

// The backlight goes dark at T0 + 30 s (the default, no key since T0).
static int64_t go_dark(void) {
  standby_init(&s, STANDBY_BACKLIGHT_30S, T0);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 30000));
  TEST_ASSERT_FALSE(s.lit);
  return T0 + 30000;
}

// --- The backlight -----------------------------------------------------------

void test_backlight_goes_off_after_each_timeout(void) {
  static const standby_backlight_t SETTINGS[] = {
      STANDBY_BACKLIGHT_15S, STANDBY_BACKLIGHT_30S, STANDBY_BACKLIGHT_60S};
  for (int i = 0; i < 3; i++) {
    standby_init(&s, SETTINGS[i], T0);
    TEST_ASSERT_TRUE(s.lit);
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0));
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + TIMEOUT_MS[i] - 1));
    TEST_ASSERT_TRUE(s.lit);
    TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                             standby_tick(&s, true, T0 + TIMEOUT_MS[i]));
    TEST_ASSERT_FALSE(s.lit);
    // Once.
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + TIMEOUT_MS[i]));
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + 100 * MINUTE));
  }
  // A late tick still turns it off.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 10 * MINUTE));
}

void test_always_on_never_goes_off(void) {
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + 1000 * MINUTE));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, INT64_MAX));
  TEST_ASSERT_TRUE(s.lit);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, standby_deadline(&s, 10, T0));
}

void test_a_key_keeps_it_lit(void) {
  standby_init(&s, STANDBY_BACKLIGHT_30S, T0);
  press('2', T0 + 20000);
  release('2', T0 + 20100);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 30000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 50100 - 1));
  TEST_ASSERT_TRUE(s.lit);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 50100));

  // A key held down (scrolling, fast-forward) keeps it lit for as long as
  // its repeats come.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  press('8', T0);
  TEST_ASSERT_TRUE(standby_key(&s, '8', KEY_LONG, T0 + 600));
  int64_t t = T0 + 600;
  for (; t < T0 + 2 * MINUTE; t += 150) {
    TEST_ASSERT_TRUE(standby_key(&s, '8', KEY_REPEAT, t));
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, t));
  }
  release('8', t);
  TEST_ASSERT_TRUE(s.lit);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, t + 15000 - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, t + 15000));
}

void test_the_waking_press_and_its_events_are_ignored(void) {
  static const char KEYS[] = {'6', '4', 'B', 'A', 'C', 'D', '#'};
  for (size_t k = 0; k < sizeof(KEYS); k++) {
    char key = KEYS[k];
    int64_t t = go_dark();
    TEST_ASSERT_FALSE(standby_key(&s, key, KEY_PRESS, t + 1000));
    TEST_ASSERT_TRUE(s.lit);
    TEST_ASSERT_FALSE(standby_key(&s, key, KEY_LONG, t + 1600));
    for (int r = 0; r < 30; r++) {
      TEST_ASSERT_FALSE(standby_key(&s, key, KEY_REPEAT, t + 1750 + r * 150));
    }
    TEST_ASSERT_FALSE(standby_key(&s, key, KEY_RELEASE, t + 9000));
    TEST_ASSERT_TRUE(s.lit);
    // The next press of the same key is acted on, and all its events.
    press(key, t + 9500);
    TEST_ASSERT_TRUE(standby_key(&s, key, KEY_LONG, t + 10100));
    TEST_ASSERT_TRUE(standby_key(&s, key, KEY_REPEAT, t + 10250));
    release(key, t + 10300);
  }

  // A tap: press and release only.
  int64_t t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1));
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_RELEASE, t + 80));
  press('6', t + 200);
  release('6', t + 280);

  // The press that woke it counts as activity: the light stays on for the
  // whole timeout after it.
  t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '5', KEY_PRESS, t + 1000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, t + 1000 + 30000 - 1));
  TEST_ASSERT_TRUE(s.lit);
}

void test_a_different_key_after_waking_is_acted_on(void) {
  int64_t t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1000));
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_RELEASE, t + 1100));
  press('2', t + 1200);
  release('2', t + 1300);

  // Rolling from the waking key to another: the keypad sends the first
  // key's release before the second's press.
  t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1000));
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_LONG, t + 1600));
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_RELEASE, t + 1700));
  press('8', t + 1710);
  TEST_ASSERT_TRUE(standby_key(&s, '8', KEY_LONG, t + 2310));
  release('8', t + 2400);

  // The waking key's release never came (a lost event): the next press of
  // any key, the same one too, is acted on.
  t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1000));
  press('4', t + 2000);
  t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1000));
  press('6', t + 2000);
  release('6', t + 2100);

  // An event of another key while the waking press is held cannot come from
  // the keypad (one key at a time); it is not the waking press's, so it is
  // passed on, and the waking press stays swallowed.
  t = go_dark();
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_PRESS, t + 1000));
  release('2', t + 1050);
  TEST_ASSERT_FALSE(standby_key(&s, '6', KEY_RELEASE, t + 1100));

  // A release with no press seen while lit is passed on.
  standby_init(&s, STANDBY_BACKLIGHT_30S, T0);
  release('6', T0 + 10);
}

// --- The sleep timer ---------------------------------------------------------

void test_sleep_counts_from_when_chosen(void) {
  static const standby_sleep_t VALUES[] = {STANDBY_SLEEP_15, STANDBY_SLEEP_30,
                                           STANDBY_SLEEP_60};
  static const int64_t MINUTES[] = {15, 30, 60};
  for (int i = 0; i < 3; i++) {
    standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
    int64_t chosen = T0 + 7 * MINUTE + 123;
    standby_set_sleep(&s, VALUES[i], chosen);
    TEST_ASSERT_EQUAL_INT(VALUES[i], s.sleep);
    int64_t expiry = chosen + MINUTES[i] * MINUTE;
    TEST_ASSERT_EQUAL_INT64(expiry, s.sleep_at_ms);
    TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, expiry - 1));
    TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE, standby_tick(&s, true, expiry));
  }
  // Choosing again counts from the new time; choosing off stops it.
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0 + 10 * MINUTE);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + 15 * MINUTE));
  standby_set_sleep(&s, STANDBY_SLEEP_OFF, T0 + 20 * MINUTE);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + 1000 * MINUTE));
  // Keys do not move it.
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  press('2', T0 + 14 * MINUTE);
  release('2', T0 + 14 * MINUTE + 100);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE,
                           standby_tick(&s, true, T0 + 15 * MINUTE));
}

void test_sleep_minutes_round_up(void) {
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, T0));
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  TEST_ASSERT_EQUAL_UINT32(15, standby_sleep_minutes(&s, T0));
  TEST_ASSERT_EQUAL_UINT32(15, standby_sleep_minutes(&s, T0 + 1));
  TEST_ASSERT_EQUAL_UINT32(15, standby_sleep_minutes(&s, T0 + MINUTE - 1));
  TEST_ASSERT_EQUAL_UINT32(14, standby_sleep_minutes(&s, T0 + MINUTE));
  TEST_ASSERT_EQUAL_UINT32(14, standby_sleep_minutes(&s, T0 + MINUTE + 1));
  TEST_ASSERT_EQUAL_UINT32(1, standby_sleep_minutes(&s, T0 + 14 * MINUTE));
  TEST_ASSERT_EQUAL_UINT32(1, standby_sleep_minutes(&s, T0 + 15 * MINUTE - 1));
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, T0 + 15 * MINUTE));
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, INT64_MAX));
  standby_set_sleep(&s, STANDBY_SLEEP_60, T0);
  TEST_ASSERT_EQUAL_UINT32(60, standby_sleep_minutes(&s, T0));
  TEST_ASSERT_EQUAL_UINT32(33, standby_sleep_minutes(&s, T0 + 27 * MINUTE));
  // A clock before the choice (it cannot go back, but nothing breaks).
  TEST_ASSERT_EQUAL_UINT32(61, standby_sleep_minutes(&s, T0 - 1));
  // During the fade: 0.
  int64_t f = start_fade();
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, f));
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, f + 5000));
}

void test_next_sleep_cycles_after_the_last_chosen(void) {
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, s.sleep);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_15, standby_next_sleep(&s));
  standby_set_sleep(&s, standby_next_sleep(&s), T0);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_30, standby_next_sleep(&s));
  standby_set_sleep(&s, standby_next_sleep(&s), T0);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_60, standby_next_sleep(&s));
  standby_set_sleep(&s, standby_next_sleep(&s), T0);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, standby_next_sleep(&s));
  standby_set_sleep(&s, standby_next_sleep(&s), T0);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_15, standby_next_sleep(&s));

  // What the row shows does not matter: 30 chosen, 10 minutes left, the
  // next is 60, counted from now.
  standby_set_sleep(&s, STANDBY_SLEEP_30, T0);
  TEST_ASSERT_EQUAL_UINT32(10, standby_sleep_minutes(&s, T0 + 20 * MINUTE));
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_60, standby_next_sleep(&s));
  standby_set_sleep(&s, standby_next_sleep(&s), T0 + 20 * MINUTE);
  TEST_ASSERT_EQUAL_UINT32(60, standby_sleep_minutes(&s, T0 + 20 * MINUTE));
}

void test_expiry_while_playing_fades_then_stops(void) {
  int64_t f = start_fade();
  TEST_ASSERT_EQUAL_INT64(f, s.fade_from_ms);
  TEST_ASSERT_TRUE(s.lit); // the fade does not touch the light
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, f));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + STANDBY_FADE_MS - 1));
  TEST_ASSERT_EQUAL_INT(1, standby_fade_level(&s, 10, f + STANDBY_FADE_MS - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP,
                           standby_tick(&s, true, f + STANDBY_FADE_MS));
  TEST_ASSERT_FALSE(s.lit);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
  // The volume setting again, and nothing more to do.
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, f + STANDBY_FADE_MS));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + STANDBY_FADE_MS));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, standby_deadline(&s, 10, f + 99999));

  // A late tick past the end still stops, once.
  f = start_fade();
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP,
                           standby_tick(&s, true, f + 10 * MINUTE));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + 10 * MINUTE));

  // The fade over a 15 s backlight: the light goes off on its own time
  // during the fade, and the stop does not turn it off twice.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  int64_t e = T0 + 15 * MINUTE;
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, true, T0 + 15000));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE, standby_tick(&s, true, e));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP,
                           standby_tick(&s, true, e + STANDBY_FADE_MS));
  TEST_ASSERT_FALSE(s.lit);

  // A tick that sees nothing playing during the fade (a gap between two
  // tracks) does not cut it short: it ends only at its end.
  f = start_fade();
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, f + 3000));
  TEST_ASSERT_EQUAL_INT64(f, s.fade_from_ms);
  TEST_ASSERT_EQUAL_INT(7, standby_fade_level(&s, 10, f + 3000));
  TEST_ASSERT_EQUAL_INT64(f + 4000, standby_deadline(&s, 10, f + 3000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, f + STANDBY_FADE_MS - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP,
                           standby_tick(&s, false, f + STANDBY_FADE_MS));
}

void test_expiry_with_nothing_playing_only_turns_the_light_off(void) {
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_30, T0);
  int64_t e = T0 + 30 * MINUTE;
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF, standby_tick(&s, false, e));
  TEST_ASSERT_FALSE(s.lit);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, e + 1000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, e + STANDBY_FADE_MS));

  // Already dark: nothing to do at all.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 15000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 15 * MINUTE));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
}

void test_fade_level_is_a_straight_line_to_zero(void) {
  int64_t f = start_fade();
  // Volume 20: one step every 500 ms, 0 only at the very end.
  TEST_ASSERT_EQUAL_INT(20, standby_fade_level(&s, 20, f));
  TEST_ASSERT_EQUAL_INT(20, standby_fade_level(&s, 20, f + 499));
  TEST_ASSERT_EQUAL_INT(19, standby_fade_level(&s, 20, f + 500));
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 20, f + 5000));
  TEST_ASSERT_EQUAL_INT(1, standby_fade_level(&s, 20, f + 9500));
  TEST_ASSERT_EQUAL_INT(1, standby_fade_level(&s, 20, f + 9999));
  TEST_ASSERT_EQUAL_INT(0, standby_fade_level(&s, 20, f + 10000));
  TEST_ASSERT_EQUAL_INT(0, standby_fade_level(&s, 20, f + 99999));
  TEST_ASSERT_EQUAL_INT(0, standby_fade_level(&s, 20, INT64_MAX));
  // Before the fade began (a clock read early): the full volume.
  TEST_ASSERT_EQUAL_INT(20, standby_fade_level(&s, 20, f - 5));
  TEST_ASSERT_EQUAL_INT(20, standby_fade_level(&s, 20, INT64_MIN));

  // Every volume, every millisecond: the level is v * remaining / FADE
  // rounded up, never rises, and is 0 only at the end.
  for (int v = 0; v <= 20; v++) {
    int prev = v;
    for (int64_t e = 0; e <= STANDBY_FADE_MS; e++) {
      int level = standby_fade_level(&s, v, f + e);
      int64_t exact = (int64_t)v * (STANDBY_FADE_MS - e); // level * FADE
      TEST_ASSERT_TRUE((int64_t)level * STANDBY_FADE_MS >= exact);
      TEST_ASSERT_TRUE((int64_t)(level - 1) * STANDBY_FADE_MS < exact ||
                       level == 0);
      TEST_ASSERT_TRUE(level <= prev && level >= 0);
      TEST_ASSERT_EQUAL(e == STANDBY_FADE_MS || v == 0, level == 0);
      prev = level;
    }
  }
}

void test_a_key_during_the_fade_cancels_it(void) {
  int64_t f = start_fade();
  press('5', f + 3000);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, f + 3000));
  // The timer is off.
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, f + 3000));
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, s.sleep);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_15, standby_next_sleep(&s));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + STANDBY_FADE_MS));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + 100 * MINUTE));
  TEST_ASSERT_TRUE(s.lit);

  // With the light off during the fade: the press wakes it, is swallowed,
  // and still cancels the fade.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  int64_t e = T0 + 15 * MINUTE;
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, true, T0 + 15000));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE, standby_tick(&s, true, e));
  TEST_ASSERT_FALSE(standby_key(&s, 'B', KEY_PRESS, e + 9999));
  TEST_ASSERT_TRUE(s.lit);
  TEST_ASSERT_EQUAL_INT(7, standby_fade_level(&s, 7, e + 9999));
  TEST_ASSERT_FALSE(standby_key(&s, 'B', KEY_RELEASE, e + 10100));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, e + STANDBY_FADE_MS));

  // Releases and repeats are not presses: they do not cancel it.
  f = start_fade();
  TEST_ASSERT_TRUE(standby_key(&s, '8', KEY_REPEAT, f + 100));
  TEST_ASSERT_TRUE(standby_key(&s, '8', KEY_RELEASE, f + 200));
  TEST_ASSERT_EQUAL_INT64(f, s.fade_from_ms);
}

// The UI ends the fade itself when playback is paused, stopped or runs out:
// choosing off ends it, and the volume setting is back.
void test_choosing_off_ends_a_fade(void) {
  int64_t f = start_fade();
  standby_set_sleep(&s, STANDBY_SLEEP_OFF, f + 4000);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, s.sleep);
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, f + 4000));
  TEST_ASSERT_EQUAL_INT(20, standby_fade_level(&s, 20, f + 9999));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, standby_deadline(&s, 10, f + 4000));
  // No stop follows, and the light is left as it was.
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + STANDBY_FADE_MS));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, f + 100 * MINUTE));
  TEST_ASSERT_TRUE(s.lit);
  // Choosing a value during a fade also ends it, and counts from now.
  f = start_fade();
  standby_set_sleep(&s, STANDBY_SLEEP_30, f + 1000);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.fade_from_ms);
  TEST_ASSERT_EQUAL_INT(10, standby_fade_level(&s, 10, f + 1000));
  TEST_ASSERT_EQUAL_INT64(f + 1000 + 30 * MINUTE, s.sleep_at_ms);
  // `playing` still decides fade or light-off at expiry.
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, f + 1000 + 30 * MINUTE));
}

void test_timer_is_off_after_it_expires(void) {
  int64_t f = start_fade();
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP,
                           standby_tick(&s, true, f + STANDBY_FADE_MS));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, s.sleep);
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_15, standby_next_sleep(&s));
  TEST_ASSERT_EQUAL_UINT32(0, standby_sleep_minutes(&s, f + STANDBY_FADE_MS));
  // Nothing happens again 15 minutes on.
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + 15 * MINUTE));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, f + 30 * MINUTE));

  // The same with nothing playing.
  standby_init(&s, STANDBY_BACKLIGHT_ON, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_60, T0);
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 60 * MINUTE));
  TEST_ASSERT_EQUAL_INT(STANDBY_SLEEP_OFF, s.sleep);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, T0 + 120 * MINUTE));
}

// Walk a fade by its deadlines at volume `v`: every deadline the level is
// one lower than just before it, and the last one is the stop.
static void walk_fade(int v) {
  int64_t f = start_fade();
  int64_t now = f;
  int steps = 0;
  for (;;) {
    int64_t d = standby_deadline(&s, v, now);
    TEST_ASSERT_TRUE(d > now);
    TEST_ASSERT_TRUE(d <= f + STANDBY_FADE_MS);
    int before = standby_fade_level(&s, v, d - 1);
    TEST_ASSERT_EQUAL_INT(standby_fade_level(&s, v, now), before);
    uint32_t act = standby_tick(&s, true, d);
    if (d == f + STANDBY_FADE_MS) {
      TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_STOP, act);
      TEST_ASSERT_TRUE(before <= 1);
      break;
    }
    TEST_ASSERT_EQUAL_UINT32(0, act);
    TEST_ASSERT_EQUAL_INT(before - 1, standby_fade_level(&s, v, d));
    now = d;
    steps++;
  }
  // One wake per step down to 1; the last step, to 0, is the stop.
  TEST_ASSERT_EQUAL_INT(v > 0 ? v - 1 : 0, steps);
}

void test_deadline_wakes_for_each_fade_step_and_each_timeout(void) {
  // The backlight timeout, moved on by a key.
  standby_init(&s, STANDBY_BACKLIGHT_30S, T0);
  TEST_ASSERT_EQUAL_INT64(T0 + 30000, standby_deadline(&s, 10, T0));
  press('2', T0 + 1000);
  TEST_ASSERT_EQUAL_INT64(T0 + 31000, standby_deadline(&s, 10, T0 + 1000));
  // The sleep timer, when it is sooner or the light is already off.
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0 + 2000);
  TEST_ASSERT_EQUAL_INT64(T0 + 31000, standby_deadline(&s, 10, T0 + 2000));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, true, T0 + 31000));
  TEST_ASSERT_EQUAL_INT64(T0 + 2000 + 15 * MINUTE,
                          standby_deadline(&s, 10, T0 + 31000));
  standby_set_backlight(&s, STANDBY_BACKLIGHT_ON, T0 + 40000);
  TEST_ASSERT_EQUAL_INT64(T0 + 2000 + 15 * MINUTE,
                          standby_deadline(&s, 10, T0 + 40000));
  standby_set_sleep(&s, STANDBY_SLEEP_OFF, T0 + 40000);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, standby_deadline(&s, 10, T0 + 40000));

  // The fade: volume 20 wakes every 500 ms, volume 10 every 1000 ms.
  int64_t f = start_fade();
  TEST_ASSERT_EQUAL_INT64(f + 500, standby_deadline(&s, 20, f));
  TEST_ASSERT_EQUAL_INT64(f + 500, standby_deadline(&s, 20, f + 499));
  TEST_ASSERT_EQUAL_INT64(f + 1000, standby_deadline(&s, 20, f + 500));
  TEST_ASSERT_EQUAL_INT64(f + 1000, standby_deadline(&s, 10, f));
  TEST_ASSERT_EQUAL_INT64(f + 10000, standby_deadline(&s, 10, f + 9000));
  // Volume 0 and 1: no step on the way, only the end.
  TEST_ASSERT_EQUAL_INT64(f + STANDBY_FADE_MS, standby_deadline(&s, 0, f));
  TEST_ASSERT_EQUAL_INT64(f + STANDBY_FADE_MS, standby_deadline(&s, 1, f));
  // Past the end: due now.
  TEST_ASSERT_EQUAL_INT64(f + STANDBY_FADE_MS,
                          standby_deadline(&s, 20, f + 20000));
  for (int v = 0; v <= 20; v++) {
    walk_fade(v);
  }
  walk_fade(7); // steps that do not divide the fade evenly
  walk_fade(3);

  // During a fade the backlight's own timeout still counts, if sooner.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  standby_set_sleep(&s, STANDBY_SLEEP_15, T0);
  int64_t e = T0 + 15 * MINUTE;
  TEST_ASSERT_TRUE(standby_key(&s, '2', KEY_PRESS, e - 14900));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_FADE, standby_tick(&s, true, e));
  TEST_ASSERT_EQUAL_INT64(e + 100, standby_deadline(&s, 1, e));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, true, e + 100));
  TEST_ASSERT_EQUAL_INT64(e + STANDBY_FADE_MS,
                          standby_deadline(&s, 1, e + 100));

  // Times at the far end of the clock do not wrap.
  standby_init(&s, STANDBY_BACKLIGHT_60S, INT64_MAX - 10);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, standby_deadline(&s, 10, INT64_MAX - 10));
  standby_set_sleep(&s, STANDBY_SLEEP_60, INT64_MAX - 10);
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, s.sleep_at_ms);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, true, INT64_MAX - 1));
}

void test_changing_the_backlight_setting_counts_from_now(void) {
  standby_init(&s, STANDBY_BACKLIGHT_30S, T0);
  standby_set_backlight(&s, STANDBY_BACKLIGHT_15S, T0 + 20000);
  TEST_ASSERT_EQUAL_INT(STANDBY_BACKLIGHT_15S, s.backlight);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 30000));
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 35000 - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 35000));

  // To always on and back.
  standby_init(&s, STANDBY_BACKLIGHT_15S, T0);
  standby_set_backlight(&s, STANDBY_BACKLIGHT_ON, T0 + 10000);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, T0 + 100 * MINUTE));
  standby_set_backlight(&s, STANDBY_BACKLIGHT_60S, T0 + 100 * MINUTE);
  TEST_ASSERT_EQUAL_UINT32(
      0, standby_tick(&s, false, T0 + 100 * MINUTE + 60000 - 1));
  TEST_ASSERT_EQUAL_UINT32(STANDBY_ACT_LIGHT_OFF,
                           standby_tick(&s, false, T0 + 100 * MINUTE + 60000));

  // Changed while dark (not from a key): it stays dark.
  int64_t t = go_dark();
  standby_set_backlight(&s, STANDBY_BACKLIGHT_ON, t + 5);
  TEST_ASSERT_FALSE(s.lit);
  TEST_ASSERT_EQUAL_UINT32(0, standby_tick(&s, false, t + 100000));
  TEST_ASSERT_FALSE(s.lit);
  TEST_ASSERT_FALSE(standby_key(&s, '2', KEY_PRESS, t + 200000));
  TEST_ASSERT_TRUE(s.lit);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_backlight_goes_off_after_each_timeout);
  RUN_TEST(test_always_on_never_goes_off);
  RUN_TEST(test_a_key_keeps_it_lit);
  RUN_TEST(test_the_waking_press_and_its_events_are_ignored);
  RUN_TEST(test_a_different_key_after_waking_is_acted_on);
  RUN_TEST(test_sleep_counts_from_when_chosen);
  RUN_TEST(test_sleep_minutes_round_up);
  RUN_TEST(test_next_sleep_cycles_after_the_last_chosen);
  RUN_TEST(test_expiry_while_playing_fades_then_stops);
  RUN_TEST(test_expiry_with_nothing_playing_only_turns_the_light_off);
  RUN_TEST(test_fade_level_is_a_straight_line_to_zero);
  RUN_TEST(test_a_key_during_the_fade_cancels_it);
  RUN_TEST(test_choosing_off_ends_a_fade);
  RUN_TEST(test_timer_is_off_after_it_expires);
  RUN_TEST(test_deadline_wakes_for_each_fade_step_and_each_timeout);
  RUN_TEST(test_changing_the_backlight_setting_counts_from_now);
  return UNITY_END();
}
