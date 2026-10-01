#include "keys.h"
#include "unity.h"

#define SCAN_MS 10
#define KEY_5 (1u << 5) // row 1, column 1
#define KEY_6 (1u << 6)
#define KEY_D (1u << 15)

static keys_t k;
static uint32_t now;
static key_event_t ev;

void setUp(void) {
  keys_init(&k);
  now = 1000;
}
void tearDown(void) {}

// One scan, 10 ms after the previous one.
static bool scan(uint16_t raw) {
  now += SCAN_MS;
  return keys_update(&k, raw, now, &ev);
}

// Scan `raw` for `ms` milliseconds and count the events of one type.
static int count_events(uint16_t raw, uint32_t ms, key_event_type_t type) {
  int n = 0;
  for (uint32_t t = 0; t < ms; t += SCAN_MS) {
    if (scan(raw) && ev.type == type) {
      n++;
    }
  }
  return n;
}

// Hold `raw` until the press is reported. Fails the test if it never is.
static void press(uint16_t raw) {
  for (int i = 0; i < KEYS_DEBOUNCE_SCANS; i++) {
    if (scan(raw)) {
      TEST_ASSERT_EQUAL_INT(KEY_PRESS, ev.type);
      return;
    }
  }
  TEST_FAIL_MESSAGE("no KEY_PRESS within the debounce time");
}

static void test_key_characters_follow_the_keypad_layout(void) {
  TEST_ASSERT_EQUAL_CHAR('1', keys_char(0));
  TEST_ASSERT_EQUAL_CHAR('A', keys_char(3));
  TEST_ASSERT_EQUAL_CHAR('5', keys_char(5));
  TEST_ASSERT_EQUAL_CHAR('*', keys_char(12));
  TEST_ASSERT_EQUAL_CHAR('0', keys_char(13));
  TEST_ASSERT_EQUAL_CHAR('#', keys_char(14));
  TEST_ASSERT_EQUAL_CHAR('D', keys_char(15));
  TEST_ASSERT_EQUAL_CHAR(0, keys_char(-1));
  TEST_ASSERT_EQUAL_CHAR(0, keys_char(16));
}

static void test_nothing_pressed_gives_no_events(void) {
  for (int i = 0; i < 100; i++) {
    TEST_ASSERT_FALSE(scan(0));
  }
}

static void test_press_is_reported_on_the_third_matching_scan(void) {
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_TRUE(scan(KEY_5));
  TEST_ASSERT_EQUAL_INT(KEY_PRESS, ev.type);
  TEST_ASSERT_EQUAL_CHAR('5', ev.key);
  TEST_ASSERT_FALSE(scan(KEY_5)); // reported once
}

static void test_a_bounce_shorter_than_the_debounce_time_is_ignored(void) {
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(0));
}

static void test_release_is_reported_after_debounce(void) {
  press(KEY_5);
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_TRUE(scan(0));
  TEST_ASSERT_EQUAL_INT(KEY_RELEASE, ev.type);
  TEST_ASSERT_EQUAL_CHAR('5', ev.key);
  TEST_ASSERT_FALSE(scan(0));
}

static void test_a_bounce_during_a_hold_does_not_release(void) {
  press(KEY_5);
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(0));
  TEST_ASSERT_FALSE(scan(KEY_5));
  TEST_ASSERT_FALSE(scan(KEY_5));
}

static void test_long_press_fires_once_at_600_ms(void) {
  press(KEY_5);
  // Up to 590 ms after the press: nothing.
  TEST_ASSERT_EQUAL_INT(0, count_events(KEY_5, KEYS_LONG_MS - SCAN_MS, KEY_LONG));
  TEST_ASSERT_TRUE(scan(KEY_5)); // 600 ms
  TEST_ASSERT_EQUAL_INT(KEY_LONG, ev.type);
  TEST_ASSERT_EQUAL_CHAR('5', ev.key);
  TEST_ASSERT_EQUAL_INT(0, count_events(KEY_5, 3000, KEY_LONG));
}

static void test_a_short_press_gives_no_long_or_repeat(void) {
  press(KEY_5);
  int extra = 0;
  for (int i = 0; i < 20; i++) { // held 200 ms
    extra += scan(KEY_5) ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(0, extra);
}

static void test_repeat_every_150_ms_after_the_long_press(void) {
  press(KEY_5);
  count_events(KEY_5, KEYS_LONG_MS, KEY_LONG); // now 600 ms held, KEY_LONG sent
  // 140 ms later: nothing yet.
  TEST_ASSERT_EQUAL_INT(0, count_events(KEY_5, 140, KEY_REPEAT));
  TEST_ASSERT_TRUE(scan(KEY_5)); // 750 ms
  TEST_ASSERT_EQUAL_INT(KEY_REPEAT, ev.type);
  TEST_ASSERT_EQUAL_CHAR('5', ev.key);
  // 900, 1050, ... 1950 ms: eight more before the 2-second mark.
  TEST_ASSERT_EQUAL_INT(8, count_events(KEY_5, 1950 - 750, KEY_REPEAT));
}

static void test_repeat_speeds_up_after_two_seconds(void) {
  press(KEY_5);
  count_events(KEY_5, 1950, KEY_REPEAT); // held 1950 ms; last repeat at 1950
  // Still on the slow interval until the hold reaches two seconds.
  TEST_ASSERT_EQUAL_INT(0, count_events(KEY_5, 40, KEY_REPEAT));
  TEST_ASSERT_TRUE(scan(KEY_5)); // 2000 ms: fast interval, 50 ms since the last
  TEST_ASSERT_EQUAL_INT(KEY_REPEAT, ev.type);
  // Then one every 50 ms: 20 in the next second.
  TEST_ASSERT_EQUAL_INT(20, count_events(KEY_5, 1000, KEY_REPEAT));
}

static void test_release_after_a_long_hold_stops_repeats(void) {
  press(KEY_5);
  count_events(KEY_5, 1000, KEY_REPEAT);
  TEST_ASSERT_EQUAL_INT(1, count_events(0, 100, KEY_RELEASE));
  TEST_ASSERT_EQUAL_INT(0, count_events(0, 1000, KEY_REPEAT));
}

static void test_second_key_while_one_is_held_is_ignored(void) {
  press(KEY_5);
  for (int i = 0; i < 30; i++) {
    TEST_ASSERT_FALSE(scan(KEY_5 | KEY_6));
  }
}

static void test_rolling_from_one_key_to_the_next(void) {
  press(KEY_5);
  scan(KEY_5 | KEY_6);
  scan(KEY_5 | KEY_6);
  scan(KEY_5 | KEY_6);
  // First key lifts while the second is still down.
  TEST_ASSERT_FALSE(scan(KEY_6));
  TEST_ASSERT_FALSE(scan(KEY_6));
  TEST_ASSERT_TRUE(scan(KEY_6));
  TEST_ASSERT_EQUAL_INT(KEY_RELEASE, ev.type);
  TEST_ASSERT_EQUAL_CHAR('5', ev.key);
  TEST_ASSERT_TRUE(scan(KEY_6)); // one event per scan: the press comes next
  TEST_ASSERT_EQUAL_INT(KEY_PRESS, ev.type);
  TEST_ASSERT_EQUAL_CHAR('6', ev.key);
}

static void test_two_keys_together_from_rest_give_nothing(void) {
  for (int i = 0; i < 100; i++) {
    TEST_ASSERT_FALSE(scan(KEY_5 | KEY_D));
  }
  // Letting go of one does not turn the other into a press.
  for (int i = 0; i < 100; i++) {
    TEST_ASSERT_FALSE(scan(KEY_D));
  }
  // Once everything is released, keys work again.
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_FALSE(scan(0));
  }
  press(KEY_D);
  TEST_ASSERT_EQUAL_CHAR('D', ev.key);
}

static void test_timing_survives_clock_wrap_around(void) {
  now = 0xFFFFFFFFu - 300; // the clock wraps 300 ms into the hold
  press(KEY_5);
  TEST_ASSERT_EQUAL_INT(1, count_events(KEY_5, KEYS_LONG_MS + 100, KEY_LONG));
  TEST_ASSERT_TRUE(now < 1000); // the clock did wrap
  TEST_ASSERT_EQUAL_INT(6, count_events(KEY_5, 900, KEY_REPEAT));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_key_characters_follow_the_keypad_layout);
  RUN_TEST(test_nothing_pressed_gives_no_events);
  RUN_TEST(test_press_is_reported_on_the_third_matching_scan);
  RUN_TEST(test_a_bounce_shorter_than_the_debounce_time_is_ignored);
  RUN_TEST(test_release_is_reported_after_debounce);
  RUN_TEST(test_a_bounce_during_a_hold_does_not_release);
  RUN_TEST(test_long_press_fires_once_at_600_ms);
  RUN_TEST(test_a_short_press_gives_no_long_or_repeat);
  RUN_TEST(test_repeat_every_150_ms_after_the_long_press);
  RUN_TEST(test_repeat_speeds_up_after_two_seconds);
  RUN_TEST(test_release_after_a_long_hold_stops_repeats);
  RUN_TEST(test_second_key_while_one_is_held_is_ignored);
  RUN_TEST(test_rolling_from_one_key_to_the_next);
  RUN_TEST(test_two_keys_together_from_rest_give_nothing);
  RUN_TEST(test_timing_survives_clock_wrap_around);
  return UNITY_END();
}
