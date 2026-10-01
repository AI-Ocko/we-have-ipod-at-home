# DAP Stage 2 (LCD and Keypad) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add drivers for the 16x2 I2C LCD and the 4x4 keypad, with a keypad test screen as the temporary main screen, while the firmware keeps playing the first FLAC on the card exactly as before.

**Architecture:** Everything that can be decided without hardware lives in `dap_core` and is tested on the host: `screen` (an in-memory 16x2 frame and the diff that finds the fewest characters to send), `keys` (debounce, long press and repeat from raw scans) and `keytest` (the test screen's contents). `dap_hw` gains two thin drivers, `lcd` and `keypad`, that only move bytes and read pins. A keypad task posts key events to a queue; a UI task consumes them and redraws.

**Tech Stack:** ESP-IDF v6.0.2 (C), `i2c_master` driver, GPIO driver, FreeRTOS queues and tasks, Unity 2.6 host tests under AddressSanitizer.

**Amended during execution:** the code listings for Tasks 1 and 2 predate their review fix round (merge gap 1, `SCREEN_MAX_RUNS` 12, key timers paused while a release is being debounced). The repository is authoritative for those files. Task 3's `lcd_show` below already reflects the change: one I2C transfer per update.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read section 2, section 3 including "Performance rules", section 9 "Keys", section 11, and stage 2 of section 14).

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- Pins (all defined in `main/pins.h`, the only file that may contain a GPIO number): LCD SDA=21, SCL=27. Keypad rows=32, 33, 4, 13. Keypad columns=34, 35, 36, 39. Never drive GPIO 0 or 12. SD (14, 15, 2) and DAC (26, 25, 22) are unchanged.
- Keypad rows are open-drain outputs: driven low or left floating, never driven high. Columns are inputs with no internal pulls.
- LCD: HD44780 in 4-bit mode behind a PCF8574 at I2C address 0x27 or 0x3F, 100 kHz.
- The LCD and keypad are optional at run time. A missing LCD, a failed I2C transfer or a failed keypad start is logged and the player carries on; none may abort, restart or block audio.
- Audio behaviour does not change in this stage. `main/player.c`, `components/dap_hw/audio_out.c` and `components/dap_hw/storage.c` are not modified.
- Key timing (spec section 9): debounce = 3 consecutive matching scans, scan period 10 ms, long press at 600 ms, repeat every 150 ms after the long press, every 50 ms once held 2 seconds. Only one key at a time is recognised.
- Task settings (spec section 3): keypad task priority 6, core 0, 3 KB stack. UI task priority 5, core 0, 8 KB stack.
- `dap_core` contains pure C using only the C standard library. No ESP-IDF or FreeRTOS includes.
- Performance (spec section 3, "Performance rules"): the LCD driver sends only characters that changed; tasks block on queues or timers and never busy-wait (a microsecond-scale settle delay inside the keypad scan is the one allowed exception); no linear search inside a loop; no heap allocation in steady-state loops. Prefer the efficient algorithm when two are equally clear.
- `MINIMAL_BUILD` stays on; each component lists what it uses in `REQUIRES`/`PRIV_REQUIRES`.
- Match the existing code style: 2-space indent, `static const char *TAG`, `ESP_LOGx` logging, comments that say *why*.
- Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan. Tests must not allocate from the heap (a failing assertion that skips a `free` makes LeakSanitizer hide Unity's output).
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Known and expected in every playback log: one line `E (...) AUD_Dec_Parse: Not found frame in max frame size 10507` about 4 s into the test track. It is a pre-existing defect scheduled for stage 3. Do not fix it here.
- The playback baseline is `docs/superpowers/plans/stage1-baseline.log`: `Playing /sdcard/Clay Pigeons.flac`, `FLAC: 44100 Hz, 2 channels, 16 bits`, `Finished: 2718 blocks decoded`, about 257 s.

## Review Focus

1. **No LCD connected.** `lcd_init` must return `ESP_ERR_NOT_FOUND` quickly and everything else must still run. Pinned in Task 3: its board check runs before the LCD is wired and expects the "Running without an LCD" log line followed by normal playback.
2. **An I2C transfer fails part-way** (loose wire). The driver must not assume the LCD shows what it was sent. Pinned in Task 3 by `s_shown_valid`: a failed transfer marks the LCD's contents unknown and the next `lcd_show` resends both rows. Cannot be automated; listed for the reviewer to read.
3. **A slow or stuck UI task.** The keypad scan must never wait on its reader. Pinned in Task 4: `xQueueSend(..., 0)` drops the event when the queue is full.
4. **A missing or miswired pull-up resistor** makes a column float and produces phantom keys. Pinned in Task 4: `keypad_low_columns()` is checked at start-up and each bad column is named in the log.
5. **Keypad pins 4 and 13 sit beside the SD card's lines.** Scanning them every 10 ms must not disturb SD reads or audio. Pinned in Task 4: a full playback with the scan running must match the baseline block count.
6. **Mechanical key bounce and two keys at once.** Pinned in Task 2 by `test_a_bounce_*`, `test_second_key_while_one_is_held_is_ignored`, `test_rolling_from_one_key_to_the_next` and `test_two_keys_together_from_rest_give_nothing`.
7. **The millisecond clock wraps around** after about 49 days of uptime. Pinned in Task 2 by `test_timing_survives_clock_wrap_around`.
8. **Text longer than the row, or a position off the screen.** Pinned in Task 1 by `test_put_clips_at_right_edge`, `test_put_ignores_positions_off_screen` and `test_set_row_pads_and_clips`.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/screen.c/.h` | In-memory 16x2 frame; diff to minimal LCD writes | 1 |
| `components/dap_core/keys.c/.h` | Raw scans to key events: debounce, long press, repeat | 2 |
| `components/dap_core/keytest.c/.h` | Contents of the keypad test screen | 2 |
| `test/host/test_screen.c`, `test_keys.c`, `test_keytest.c` | Host tests | 1, 2 |
| `components/dap_hw/lcd.c/.h` | HD44780 over PCF8574; applies screen diffs | 3 |
| `docs/wiring.md` | Label-by-label wiring list | 3 |
| `components/dap_hw/keypad.c/.h` | Matrix scan and the scanning task | 4 |
| `main/app_main.c` | Starts LCD, keypad and the UI task, then plays as before | 3, 4 |

There is a **wiring pause between Task 3 and Task 4**: Task 3 is verified with nothing connected (the "no LCD" path), then the user wires the LCD and keypad, then Task 4 is verified with them connected.

---

### Task 1: `screen`, the in-memory LCD frame

**Files:**
- Create: `components/dap_core/screen.h`, `components/dap_core/screen.c`
- Test: `test/host/test_screen.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `#define SCREEN_ROWS 2`, `SCREEN_COLS 16`, `SCREEN_MAX_RUNS 12`, `SCREEN_MERGE_GAP 1` (after the fix round)
  - `typedef struct { char cells[SCREEN_ROWS][SCREEN_COLS]; } screen_t;`
  - `typedef struct { uint8_t row; uint8_t col; uint8_t len; } screen_run_t;`
  - `void screen_clear(screen_t *s);`
  - `void screen_put(screen_t *s, int row, int col, const char *text);`
  - `void screen_set_row(screen_t *s, int row, const char *text);`
  - `int screen_diff(const screen_t *shown, const screen_t *wanted, screen_run_t runs[SCREEN_MAX_RUNS]);`

Why the diff matters: sending one character to this LCD takes about 0.4 ms over I2C, so redrawing all 32 cells on every update would cost about 13 ms each time. A progress counter changes one or two cells; the diff sends only those.

- [ ] **Step 1: Write the header**

Create `components/dap_core/screen.h`:

```c
#pragma once

#include <stdint.h>

// What the 16x2 LCD should show, held in memory. Screens are drawn into a
// screen_t and handed to the LCD driver, which sends only the cells that
// differ from what is already displayed.

#define SCREEN_ROWS 2
#define SCREEN_COLS 16

// Most runs screen_diff can produce: each run is followed by at least
// SCREEN_MERGE_GAP + 1 unchanged cells, so at most four fit on a row.
#define SCREEN_MAX_RUNS 8

// Two changed stretches separated by this many unchanged cells or fewer are
// sent as one run. Moving the LCD's cursor costs as much as sending one
// character and needs its own I2C transfer, so bridging a short gap is cheaper.
#define SCREEN_MERGE_GAP 2

// Cells hold LCD character codes. Codes 8..15 show the LCD's eight
// user-defined characters (the same glyphs as codes 0..7, which cannot be
// used here because 0 ends a C string).
typedef struct {
  char cells[SCREEN_ROWS][SCREEN_COLS];
} screen_t;

// A horizontal stretch of cells to send to the LCD.
typedef struct {
  uint8_t row;
  uint8_t col;
  uint8_t len;
} screen_run_t;

// Fill with spaces.
void screen_clear(screen_t *s);

// Write `text` at (row, col), overwriting what is there. Text that would run
// past the right edge is cut off. A position outside the screen is ignored.
void screen_put(screen_t *s, int row, int col, const char *text);

// Replace a whole row: `text`, then spaces to the right edge.
void screen_set_row(screen_t *s, int row, const char *text);

// Work out what must be sent to turn `shown` into `wanted`. Fills `runs` and
// returns how many there are (0 if the screens are identical).
int screen_diff(const screen_t *shown, const screen_t *wanted,
                screen_run_t runs[SCREEN_MAX_RUNS]);
```

- [ ] **Step 2: Write a stub so the tests can link**

Create `components/dap_core/screen.c`:

```c
#include "screen.h"

void screen_clear(screen_t *s) { (void)s; }

void screen_put(screen_t *s, int row, int col, const char *text) {
  (void)s;
  (void)row;
  (void)col;
  (void)text;
}

void screen_set_row(screen_t *s, int row, const char *text) {
  (void)s;
  (void)row;
  (void)text;
}

int screen_diff(const screen_t *shown, const screen_t *wanted,
                screen_run_t runs[SCREEN_MAX_RUNS]) {
  (void)shown;
  (void)wanted;
  (void)runs;
  return 0;
}
```

- [ ] **Step 3: Write the failing tests**

Create `test/host/test_screen.c`:

```c
#include "screen.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static screen_t shown;
static screen_t wanted;
static screen_run_t runs[SCREEN_MAX_RUNS];

static void both_blank(void) {
  screen_clear(&shown);
  screen_clear(&wanted);
}

static void test_clear_fills_with_spaces(void) {
  screen_t s;
  memset(&s, 'x', sizeof(s));
  screen_clear(&s);
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[1], SCREEN_COLS);
}

static void test_put_writes_at_position(void) {
  screen_t s;
  screen_clear(&s);
  screen_put(&s, 1, 3, "abc");
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("   abc          ", s.cells[1], SCREEN_COLS);
}

static void test_put_clips_at_right_edge(void) {
  screen_t s;
  screen_clear(&s);
  screen_put(&s, 0, 14, "abcdef");
  TEST_ASSERT_EQUAL_MEMORY("              ab", s.cells[0], SCREEN_COLS);
  // Nothing spilled onto the next row.
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[1], SCREEN_COLS);
}

static void test_put_ignores_positions_off_screen(void) {
  screen_t s;
  screen_clear(&s);
  screen_put(&s, -1, 0, "x");
  screen_put(&s, 2, 0, "x");
  screen_put(&s, 0, -1, "x");
  screen_put(&s, 0, 16, "x");
  screen_put(&s, 0, 0, NULL);
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("                ", s.cells[1], SCREEN_COLS);
}

static void test_set_row_pads_and_clips(void) {
  screen_t s;
  memset(&s, 'x', sizeof(s));
  screen_set_row(&s, 0, "Main");
  TEST_ASSERT_EQUAL_MEMORY("Main            ", s.cells[0], SCREEN_COLS);
  screen_set_row(&s, 1, "A title longer than the row");
  TEST_ASSERT_EQUAL_MEMORY("A title longer t", s.cells[1], SCREEN_COLS);
  screen_set_row(&s, 5, "ignored");
  TEST_ASSERT_EQUAL_MEMORY("Main            ", s.cells[0], SCREEN_COLS);
}

static void test_diff_identical_screens_send_nothing(void) {
  both_blank();
  screen_set_row(&shown, 0, "Artists    14/87");
  screen_set_row(&wanted, 0, "Artists    14/87");
  TEST_ASSERT_EQUAL_INT(0, screen_diff(&shown, &wanted, runs));
}

static void test_diff_single_cell(void) {
  both_blank();
  screen_put(&wanted, 1, 7, "x");
  TEST_ASSERT_EQUAL_INT(1, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].row);
  TEST_ASSERT_EQUAL_UINT8(7, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].len);
}

static void test_diff_whole_row(void) {
  both_blank();
  screen_set_row(&wanted, 0, "0123456789abcdef");
  TEST_ASSERT_EQUAL_INT(1, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(0, runs[0].row);
  TEST_ASSERT_EQUAL_UINT8(0, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(16, runs[0].len);
}

static void test_diff_only_the_changed_digits_of_a_counter(void) {
  both_blank();
  screen_set_row(&shown, 1, "> 1:23/4:11  S L");
  screen_set_row(&wanted, 1, "> 1:24/4:11  S L");
  TEST_ASSERT_EQUAL_INT(1, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].row);
  TEST_ASSERT_EQUAL_UINT8(5, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].len);
}

static void test_diff_merges_across_a_short_gap(void) {
  both_blank();
  screen_put(&wanted, 0, 2, "x"); // changed at 2 and 5: two unchanged between
  screen_put(&wanted, 0, 5, "x");
  TEST_ASSERT_EQUAL_INT(1, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(2, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(4, runs[0].len);
}

static void test_diff_splits_across_a_long_gap(void) {
  both_blank();
  screen_put(&wanted, 0, 2, "x"); // changed at 2 and 6: three unchanged between
  screen_put(&wanted, 0, 6, "x");
  TEST_ASSERT_EQUAL_INT(2, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(2, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].len);
  TEST_ASSERT_EQUAL_UINT8(6, runs[1].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[1].len);
}

static void test_diff_run_never_ends_on_an_unchanged_cell(void) {
  both_blank();
  screen_put(&wanted, 0, 12, "x"); // 13 and 14 unchanged, then the edge
  TEST_ASSERT_EQUAL_INT(1, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(12, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].len);
}

static void test_diff_last_cell_of_each_row(void) {
  both_blank();
  screen_put(&wanted, 0, 15, "x");
  screen_put(&wanted, 1, 15, "y");
  TEST_ASSERT_EQUAL_INT(2, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(0, runs[0].row);
  TEST_ASSERT_EQUAL_UINT8(15, runs[0].col);
  TEST_ASSERT_EQUAL_UINT8(1, runs[0].len);
  TEST_ASSERT_EQUAL_UINT8(1, runs[1].row);
  TEST_ASSERT_EQUAL_UINT8(15, runs[1].col);
}

static void test_diff_worst_case_fits_in_max_runs(void) {
  both_blank();
  for (int row = 0; row < SCREEN_ROWS; row++) {
    for (int col = 0; col < SCREEN_COLS; col += SCREEN_MERGE_GAP + 2) {
      screen_put(&wanted, row, col, "x");
    }
  }
  TEST_ASSERT_EQUAL_INT(SCREEN_MAX_RUNS, screen_diff(&shown, &wanted, runs));
  TEST_ASSERT_EQUAL_UINT8(1, runs[SCREEN_MAX_RUNS - 1].row);
  TEST_ASSERT_EQUAL_UINT8(12, runs[SCREEN_MAX_RUNS - 1].col);
}

static void test_diff_runs_cover_every_changed_cell(void) {
  // Applying the runs to `shown` must reproduce `wanted` exactly.
  both_blank();
  screen_set_row(&shown, 0, "Kid A       3/10");
  screen_set_row(&shown, 1, ">The National A>");
  screen_set_row(&wanted, 0, "Kid A       4/10");
  screen_set_row(&wanted, 1, ">Optimistic     ");
  int n = screen_diff(&shown, &wanted, runs);
  for (int i = 0; i < n; i++) {
    memcpy(&shown.cells[runs[i].row][runs[i].col],
           &wanted.cells[runs[i].row][runs[i].col], runs[i].len);
  }
  TEST_ASSERT_EQUAL_MEMORY(wanted.cells, shown.cells, sizeof(shown.cells));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_clear_fills_with_spaces);
  RUN_TEST(test_put_writes_at_position);
  RUN_TEST(test_put_clips_at_right_edge);
  RUN_TEST(test_put_ignores_positions_off_screen);
  RUN_TEST(test_set_row_pads_and_clips);
  RUN_TEST(test_diff_identical_screens_send_nothing);
  RUN_TEST(test_diff_single_cell);
  RUN_TEST(test_diff_whole_row);
  RUN_TEST(test_diff_only_the_changed_digits_of_a_counter);
  RUN_TEST(test_diff_merges_across_a_short_gap);
  RUN_TEST(test_diff_splits_across_a_long_gap);
  RUN_TEST(test_diff_run_never_ends_on_an_unchanged_cell);
  RUN_TEST(test_diff_last_cell_of_each_row);
  RUN_TEST(test_diff_worst_case_fits_in_max_runs);
  RUN_TEST(test_diff_runs_cover_every_changed_cell);
  return UNITY_END();
}
```

In `test/host/CMakeLists.txt`, add `${CORE_DIR}/screen.c` to the `add_library(dap_core STATIC ...)` source list (one source per line, after `translit.c`), and add `dap_test(test_screen)` after `dap_test(test_translit)`.

- [ ] **Step 4: Run the tests and confirm they fail**

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `test_translit` passes; `test_screen` FAILS (for example `test_clear_fills_with_spaces` reports a memory mismatch). `test_diff_identical_screens_send_nothing` passes against the stub; that is fine.

- [ ] **Step 5: Implement `screen`**

Replace `components/dap_core/screen.c` with:

```c
#include "screen.h"

#include <string.h>

void screen_clear(screen_t *s) { memset(s->cells, ' ', sizeof(s->cells)); }

void screen_put(screen_t *s, int row, int col, const char *text) {
  if (row < 0 || row >= SCREEN_ROWS || col < 0 || !text) {
    return;
  }
  for (; col < SCREEN_COLS && *text; col++, text++) {
    s->cells[row][col] = *text;
  }
}

void screen_set_row(screen_t *s, int row, const char *text) {
  if (row < 0 || row >= SCREEN_ROWS) {
    return;
  }
  memset(s->cells[row], ' ', SCREEN_COLS);
  screen_put(s, row, 0, text);
}

int screen_diff(const screen_t *shown, const screen_t *wanted,
                screen_run_t runs[SCREEN_MAX_RUNS]) {
  int count = 0;

  for (int row = 0; row < SCREEN_ROWS; row++) {
    const char *old = shown->cells[row];
    const char *new = wanted->cells[row];
    int col = 0;

    while (col < SCREEN_COLS) {
      if (old[col] == new[col]) {
        col++;
        continue;
      }
      // A run starts at the first changed cell and keeps going while the
      // next changed cell is no more than SCREEN_MERGE_GAP cells away.
      int start = col;
      int end = col;
      for (int next = col + 1; next < SCREEN_COLS; next++) {
        if (old[next] != new[next]) {
          end = next;
        } else if (next - end > SCREEN_MERGE_GAP) {
          break;
        }
      }
      runs[count].row = (uint8_t)row;
      runs[count].col = (uint8_t)start;
      runs[count].len = (uint8_t)(end - start + 1);
      count++;
      col = end + 1;
    }
  }
  return count;
}
```

- [ ] **Step 6: Run the tests and confirm they pass**

Run: `cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `100% tests passed`. Run `test/host/build/test_screen` and expect `15 Tests 0 Failures 0 Ignored` with nothing on stderr.

- [ ] **Step 7: Add it to the firmware build**

In `components/dap_core/CMakeLists.txt`, change `SRCS "translit.c"` to `SRCS "translit.c" "screen.c"`.

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: `Project build complete.` with no warnings from `components/dap_core/`.

- [ ] **Step 8: Commit**

```bash
git add components/dap_core test/host
git commit -m "Add screen: in-memory LCD frame and minimal-write diff

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `keys` and the keypad test screen

**Files:**
- Create: `components/dap_core/keys.h`, `components/dap_core/keys.c`, `components/dap_core/keytest.h`, `components/dap_core/keytest.c`
- Test: `test/host/test_keys.c`, `test/host/test_keytest.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Consumes: `screen_t`, `screen_set_row`, `SCREEN_COLS` from Task 1.
- Produces (`keys.h`):
  - `typedef enum { KEY_PRESS, KEY_LONG, KEY_REPEAT, KEY_RELEASE } key_event_type_t;`
  - `typedef struct { key_event_type_t type; char key; } key_event_t;`
  - `typedef struct { ... } keys_t;`
  - `void keys_init(keys_t *k);`
  - `char keys_char(int index);` — index = row * 4 + column, layout `"123A456B789C*0#D"`
  - `bool keys_update(keys_t *k, uint16_t raw, uint32_t now_ms, key_event_t *event);`
- Produces (`keytest.h`):
  - `#define KEYTEST_GLYPH_CODE 8`
  - `extern const unsigned char KEYTEST_GLYPH[8];`
  - `const char *keytest_event_name(key_event_type_t type);`
  - `void keytest_render(screen_t *s, const key_event_t *last, unsigned count);`

Do the two modules one after the other, each test-first: `keys` (Steps 1–5), then `keytest` (Steps 6–9).

- [ ] **Step 1: Write the `keys` header**

Create `components/dap_core/keys.h`:

```c
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
```

- [ ] **Step 2: Write a stub**

Create `components/dap_core/keys.c`:

```c
#include "keys.h"

void keys_init(keys_t *k) {
  k->active = -1;
  k->count = 0;
}

char keys_char(int index) {
  (void)index;
  return 0;
}

bool keys_update(keys_t *k, uint16_t raw, uint32_t now_ms, key_event_t *event) {
  (void)k;
  (void)raw;
  (void)now_ms;
  (void)event;
  return false;
}
```

- [ ] **Step 3: Write the failing tests**

Create `test/host/test_keys.c`:

```c
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
```

In `test/host/CMakeLists.txt`, add `${CORE_DIR}/keys.c` to the `dap_core` source list and `dap_test(test_keys)` to the test list.

- [ ] **Step 4: Run the tests and confirm they fail**

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `test_keys` FAILS, starting with `test_key_characters_follow_the_keypad_layout` (`Expected '1' Was '\0'` or similar). Tests that expect no events pass against the stub; that is fine.

- [ ] **Step 5: Implement `keys`, then confirm the tests pass**

Replace `components/dap_core/keys.c` with:

```c
#include "keys.h"

static const char KEY_CHARS[KEYS_COUNT + 1] = "123A456B789C*0#D";

void keys_init(keys_t *k) {
  k->candidate = 0;
  k->count = 0;
  k->stable = 0;
  k->active = -1;
  k->blocked = false;
  k->long_sent = false;
  k->pressed_at = 0;
  k->last_fire = 0;
}

char keys_char(int index) {
  if (index < 0 || index >= KEYS_COUNT) {
    return 0;
  }
  return KEY_CHARS[index];
}

static bool emit(key_event_t *event, key_event_type_t type, int index) {
  event->type = type;
  event->key = KEY_CHARS[index];
  return true;
}

bool keys_update(keys_t *k, uint16_t raw, uint32_t now_ms, key_event_t *event) {
  // Debounce: a snapshot is believed once it has been seen unchanged for
  // KEYS_DEBOUNCE_SCANS scans in a row.
  if (raw == k->candidate) {
    if (k->count < KEYS_DEBOUNCE_SCANS) {
      k->count++;
    }
  } else {
    k->candidate = raw;
    k->count = 1;
  }
  if (k->count >= KEYS_DEBOUNCE_SCANS) {
    k->stable = k->candidate;
  }

  if (k->active < 0) {
    uint16_t down = k->stable;
    if (down == 0) {
      k->blocked = false;
      return false;
    }
    if (k->blocked) {
      return false;
    }
    if (down & (down - 1)) { // more than one bit set
      k->blocked = true;
      return false;
    }
    int index = 0;
    while (!(down & 1u)) {
      down >>= 1;
      index++;
    }
    k->active = (int8_t)index;
    k->pressed_at = now_ms;
    k->long_sent = false;
    return emit(event, KEY_PRESS, index);
  }

  if (!(k->stable & (1u << k->active))) {
    int index = k->active;
    k->active = -1;
    return emit(event, KEY_RELEASE, index);
  }

  // Unsigned subtraction gives the right answer across a clock wrap-around.
  uint32_t held = now_ms - k->pressed_at;
  if (!k->long_sent) {
    if (held < KEYS_LONG_MS) {
      return false;
    }
    k->long_sent = true;
    k->last_fire = now_ms;
    return emit(event, KEY_LONG, k->active);
  }

  uint32_t interval =
      (held >= KEYS_FAST_AFTER_MS) ? KEYS_FAST_REPEAT_MS : KEYS_REPEAT_MS;
  if (now_ms - k->last_fire < interval) {
    return false;
  }
  k->last_fire = now_ms;
  return emit(event, KEY_REPEAT, k->active);
}
```

Run: `cmake --build test/host/build && test/host/build/test_keys`
Expected: `15 Tests 0 Failures 0 Ignored`, nothing on stderr.

- [ ] **Step 6: Write the `keytest` header and a stub**

Create `components/dap_core/keytest.h`:

```c
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
```

Create `components/dap_core/keytest.c` as a stub:

```c
#include "keytest.h"

const unsigned char KEYTEST_GLYPH[8] = {0};

const char *keytest_event_name(key_event_type_t type) {
  (void)type;
  return "";
}

void keytest_render(screen_t *s, const key_event_t *last, unsigned count) {
  (void)s;
  (void)last;
  (void)count;
}
```

- [ ] **Step 7: Write the failing tests**

Create `test/host/test_keytest.c`:

```c
#include "keytest.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_before_any_key_it_shows_a_prompt(void) {
  screen_t s;
  memset(&s, 'x', sizeof(s));
  keytest_render(&s, NULL, 0);
  TEST_ASSERT_EQUAL_MEMORY("Key test       0", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("Press any key  \x08", s.cells[1], SCREEN_COLS);
}

static void test_it_shows_the_key_and_what_happened(void) {
  screen_t s;
  key_event_t ev = {.type = KEY_PRESS, .key = '5'};
  keytest_render(&s, &ev, 1);
  TEST_ASSERT_EQUAL_MEMORY("Key test       1", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("5 press        \x08", s.cells[1], SCREEN_COLS);

  ev = (key_event_t){.type = KEY_RELEASE, .key = '#'};
  keytest_render(&s, &ev, 42);
  TEST_ASSERT_EQUAL_MEMORY("Key test      42", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("# release      \x08", s.cells[1], SCREEN_COLS);
}

static void test_event_names(void) {
  TEST_ASSERT_EQUAL_STRING("press", keytest_event_name(KEY_PRESS));
  TEST_ASSERT_EQUAL_STRING("long", keytest_event_name(KEY_LONG));
  TEST_ASSERT_EQUAL_STRING("repeat", keytest_event_name(KEY_REPEAT));
  TEST_ASSERT_EQUAL_STRING("release", keytest_event_name(KEY_RELEASE));
}

static void test_count_rolls_over_instead_of_overflowing_the_row(void) {
  screen_t s;
  key_event_t ev = {.type = KEY_REPEAT, .key = '8'};
  keytest_render(&s, &ev, 99999);
  TEST_ASSERT_EQUAL_MEMORY("Key test   99999", s.cells[0], SCREEN_COLS);
  keytest_render(&s, &ev, 100003);
  TEST_ASSERT_EQUAL_MEMORY("Key test       3", s.cells[0], SCREEN_COLS);
  TEST_ASSERT_EQUAL_MEMORY("8 repeat       \x08", s.cells[1], SCREEN_COLS);
}

static void test_glyph_is_a_five_pixel_wide_bitmap(void) {
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL_HEX8(0, KEYTEST_GLYPH[i] & 0xE0);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_before_any_key_it_shows_a_prompt);
  RUN_TEST(test_it_shows_the_key_and_what_happened);
  RUN_TEST(test_event_names);
  RUN_TEST(test_count_rolls_over_instead_of_overflowing_the_row);
  RUN_TEST(test_glyph_is_a_five_pixel_wide_bitmap);
  return UNITY_END();
}
```

In `test/host/CMakeLists.txt`, add `${CORE_DIR}/keytest.c` to the `dap_core` source list and `dap_test(test_keytest)` to the test list.

- [ ] **Step 8: Run the tests and confirm they fail**

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `test_keytest` FAILS (`test_before_any_key_it_shows_a_prompt` reports a memory mismatch). `test_glyph_is_a_five_pixel_wide_bitmap` passes against the all-zero stub; that is fine.

- [ ] **Step 9: Implement `keytest`, then confirm everything passes**

Replace `components/dap_core/keytest.c` with:

```c
#include "keytest.h"

#include <stdio.h>

const unsigned char KEYTEST_GLYPH[8] = {
    0x10, // #....
    0x18, // ##...
    0x1C, // ###..
    0x1E, // ####.
    0x1C, // ###..
    0x18, // ##...
    0x10, // #....
    0x00, // .....
};

const char *keytest_event_name(key_event_type_t type) {
  switch (type) {
  case KEY_PRESS:
    return "press";
  case KEY_LONG:
    return "long";
  case KEY_REPEAT:
    return "repeat";
  case KEY_RELEASE:
    return "release";
  }
  return "?";
}

void keytest_render(screen_t *s, const key_event_t *last, unsigned count) {
  char line[SCREEN_COLS + 1];

  // The count is shown in five digits and rolls over rather than overflowing
  // the row.
  snprintf(line, sizeof(line), "Key test   %5u", count % 100000u);
  screen_set_row(s, 0, line);

  if (last) {
    snprintf(line, sizeof(line), "%c %s", last->key,
             keytest_event_name(last->type));
    screen_set_row(s, 1, line);
  } else {
    screen_set_row(s, 1, "Press any key");
  }
  s->cells[1][SCREEN_COLS - 1] = KEYTEST_GLYPH_CODE;
}
```

Run: `cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 4`. Run `test/host/build/test_keytest` and expect `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 10: Add both to the firmware build**

In `components/dap_core/CMakeLists.txt`, extend the list to `SRCS "translit.c" "screen.c" "keys.c" "keytest.c"`.

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: `Project build complete.` with no warnings from `components/dap_core/`.

- [ ] **Step 11: Commit**

```bash
git add components/dap_core test/host
git commit -m "Add keys (debounce, long press, repeat) and the keypad test screen

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: LCD driver, wiring guide, first screen

**Files:**
- Create: `components/dap_hw/lcd.h`, `components/dap_hw/lcd.c`, `docs/wiring.md`
- Modify: `components/dap_hw/CMakeLists.txt`, `main/app_main.c`

**Interfaces:**
- Consumes: `screen_t`, `screen_run_t`, `screen_diff`, `screen_clear`, `SCREEN_*` (Task 1); `keytest_render`, `KEYTEST_GLYPH`, `KEYTEST_GLYPH_CODE` (Task 2); `PIN_LCD_SDA`, `PIN_LCD_SCL` (already in `main/pins.h`).
- Produces (`lcd.h`):
  - `esp_err_t lcd_init(int sda_pin, int scl_pin);` — `ESP_ERR_NOT_FOUND` if no LCD answers
  - `bool lcd_is_present(void);`
  - `esp_err_t lcd_show(const screen_t *wanted);`
  - `esp_err_t lcd_define_char(int index, const uint8_t rows[8]);`
  - `esp_err_t lcd_set_backlight(bool on);`

Background for the implementer:
- The PCF8574 is an 8-bit output port on I2C: every byte written appears on its eight pins. Several bytes in one I2C transfer appear one after another. The LCD latches its data lines on the falling edge of EN, so one LCD byte in 4-bit mode is four port bytes: high half with EN set, high half with EN clear, low half with EN set, low half with EN clear.
- At 100 kHz each port byte takes 90 µs, longer than the 37 µs the LCD needs per operation, so a cursor move plus a whole run of characters can go in a single transfer with no delays.
- `FREERTOS_HZ` is 100, so one tick is 10 ms and `vTaskDelay(pdMS_TO_TICKS(5))` is a delay of **zero**. The driver's `wait_ms` helper adds one tick to guarantee the minimum.
- This task's board check is run **before the LCD is wired**, so it exercises the "no LCD" path. If an LCD happens to be connected, "LCD found" is also an acceptable result; report which you saw.

- [ ] **Step 1: Write the driver header**

Create `components/dap_hw/lcd.h`:

```c
#pragma once

#include "esp_err.h"
#include "screen.h"
#include <stdbool.h>
#include <stdint.h>

// LCD1602 (HD44780 controller) behind a PCF8574 I2C backpack.
//
// Not thread-safe: call from one task only (the UI task).

// Find the LCD and initialise it. Tries I2C address 0x27, then 0x3F.
// Returns ESP_ERR_NOT_FOUND if neither answers; the player is expected to
// carry on without a screen in that case.
esp_err_t lcd_init(int sda_pin, int scl_pin);

bool lcd_is_present(void);

// Make the LCD show `wanted`. Only the cells that differ from what is already
// displayed are sent: the I2C link costs about 0.4 ms per character, so this
// is what keeps the interface responsive.
esp_err_t lcd_show(const screen_t *wanted);

// Define one of the eight user-defined characters. `index` is 0..7 and
// `rows` holds eight 5-bit pixel rows, top first. The character is displayed
// by putting code 8 + index in a screen cell.
esp_err_t lcd_define_char(int index, const uint8_t rows[8]);

// Switch the backlight on or off. The display contents are kept.
esp_err_t lcd_set_backlight(bool on);
```

- [ ] **Step 2: Write the driver**

Create `components/dap_hw/lcd.c`:

```c
#include "lcd.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>

// How the backpack's PCF8574 outputs are wired to the LCD. The top four bits
// are the LCD's D4..D7. RW (0x02) is left low: we only ever write.
#define BIT_RS 0x01 // 0 = command, 1 = character data
#define BIT_EN 0x04 // the LCD reads the data lines when this falls
#define BIT_BACKLIGHT 0x08

#define CMD_CLEAR 0x01
#define CMD_ENTRY_MODE 0x06   // cursor moves right, display does not shift
#define CMD_DISPLAY_OFF 0x08
#define CMD_DISPLAY_ON 0x0C   // display on, no cursor, no blink
#define CMD_FUNCTION_SET 0x28 // 4-bit bus, two lines, 5x8 font
#define CMD_SET_CGRAM 0x40
#define CMD_SET_DDRAM 0x80
#define ROW_1_ADDRESS 0x40

#define I2C_SPEED_HZ 100000 // the PCF8574's limit
#define I2C_TIMEOUT_MS 50

// One LCD byte takes four expander bytes: each half is put on the data lines
// with EN high, then again with EN low.
#define BYTES_PER_LCD_BYTE 4

static const char *TAG = "lcd";

static const uint8_t ADDRESSES[] = {0x27, 0x3F};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_backlight = BIT_BACKLIGHT;

// What the LCD is currently showing, so lcd_show can send only differences.
static screen_t s_shown;
static bool s_shown_valid;

// Wait at least `ms`. vTaskDelay rounds down to whole ticks and the first
// tick may be almost over already, so add one.
static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// Append the expander bytes that send one byte to the LCD. `rs` is 0 for a
// command or BIT_RS for character data. Returns the new length of `buf`.
//
// A whole run of these goes out in one I2C transfer. Each expander byte takes
// 90 us on the wire, which is longer than the 37 us the LCD needs between
// operations, so no delays are needed inside a transfer.
static size_t append_byte(uint8_t *buf, size_t len, uint8_t value, uint8_t rs) {
  uint8_t high = (uint8_t)((value & 0xF0) | rs | s_backlight);
  uint8_t low = (uint8_t)(((value << 4) & 0xF0) | rs | s_backlight);
  buf[len++] = high | BIT_EN;
  buf[len++] = high;
  buf[len++] = low | BIT_EN;
  buf[len++] = low;
  return len;
}

static esp_err_t send_command(uint8_t command) {
  uint8_t buf[BYTES_PER_LCD_BYTE];
  size_t len = append_byte(buf, 0, command, 0);
  return i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
}

// Send only the upper half of a byte. Needed at power-up, while the LCD is
// still in 8-bit mode and reads all eight (half-connected) data lines at once.
static esp_err_t send_half(uint8_t half) {
  uint8_t value = (uint8_t)((half << 4) | s_backlight);
  uint8_t buf[2] = {value | BIT_EN, value};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// The HD44780's documented power-up sequence for a 4-bit bus.
static esp_err_t reset_controller(void) {
  esp_err_t err;
  wait_ms(50); // the LCD needs 40 ms after power-on before it listens

  // Three times "8-bit mode", which gets the LCD into a known state whichever
  // mode and half-byte it was left in, then "4-bit mode".
  for (int i = 0; i < 3; i++) {
    if ((err = send_half(0x3)) != ESP_OK) {
      return err;
    }
    wait_ms(5);
  }
  if ((err = send_half(0x2)) != ESP_OK) {
    return err;
  }
  wait_ms(1);

  if ((err = send_command(CMD_FUNCTION_SET)) != ESP_OK ||
      (err = send_command(CMD_DISPLAY_OFF)) != ESP_OK ||
      (err = send_command(CMD_CLEAR)) != ESP_OK) {
    return err;
  }
  wait_ms(2); // clearing is the one slow command: 1.5 ms
  if ((err = send_command(CMD_ENTRY_MODE)) != ESP_OK ||
      (err = send_command(CMD_DISPLAY_ON)) != ESP_OK) {
    return err;
  }
  return ESP_OK;
}

esp_err_t lcd_init(int sda_pin, int scl_pin) {
  if (s_dev) {
    return ESP_OK;
  }

  if (!s_bus) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1, // any free port
        .sda_io_num = (gpio_num_t)sda_pin,
        .scl_io_num = (gpio_num_t)scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The backpack has its own pull-ups. These weak internal ones only
        // matter when no LCD is connected: they stop the lines floating, so
        // the probe below fails quickly and cleanly.
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
      s_bus = NULL;
      return err;
    }
  }

  uint8_t address = 0;
  for (size_t i = 0; i < sizeof(ADDRESSES); i++) {
    if (i2c_master_probe(s_bus, ADDRESSES[i], I2C_TIMEOUT_MS) == ESP_OK) {
      address = ADDRESSES[i];
      break;
    }
  }
  if (address == 0) {
    ESP_LOGW(TAG, "No LCD found at 0x27 or 0x3F");
    return ESP_ERR_NOT_FOUND;
  }

  i2c_device_config_t device_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = I2C_SPEED_HZ,
  };
  esp_err_t err = i2c_master_bus_add_device(s_bus, &device_config, &s_dev);
  if (err != ESP_OK) {
    s_dev = NULL;
    return err;
  }

  err = reset_controller();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LCD at 0x%02X did not initialise (%s)", address,
             esp_err_to_name(err));
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
  }

  screen_clear(&s_shown); // matches the LCD after CMD_CLEAR
  s_shown_valid = true;
  ESP_LOGI(TAG, "LCD found at 0x%02X", address);
  return ESP_OK;
}

bool lcd_is_present(void) { return s_dev != NULL; }

esp_err_t lcd_show(const screen_t *wanted) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }

  screen_run_t runs[SCREEN_MAX_RUNS];
  int count;
  if (s_shown_valid) {
    count = screen_diff(&s_shown, wanted, runs);
    if (count == 0) {
      return ESP_OK; // nothing changed: no I2C traffic at all
    }
  } else {
    // A previous transfer failed part-way, so the LCD's contents are unknown.
    // Send both rows in full.
    for (int row = 0; row < SCREEN_ROWS; row++) {
      runs[row] = (screen_run_t){
          .row = (uint8_t)row, .col = 0, .len = SCREEN_COLS};
    }
    count = SCREEN_ROWS;
  }

  // The whole update goes out as one I2C transfer: for each run, a cursor
  // move followed by its characters. Sized for the worst case, every cell
  // plus one cursor move per run.
  uint8_t buf[BYTES_PER_LCD_BYTE *
              (SCREEN_MAX_RUNS + SCREEN_ROWS * SCREEN_COLS)];
  size_t len = 0;
  for (int i = 0; i < count; i++) {
    uint8_t address =
        (uint8_t)((runs[i].row ? ROW_1_ADDRESS : 0) + runs[i].col);
    len = append_byte(buf, len, CMD_SET_DDRAM | address, 0);
    const char *cells = &wanted->cells[runs[i].row][runs[i].col];
    for (int c = 0; c < runs[i].len; c++) {
      len = append_byte(buf, len, (uint8_t)cells[c], BIT_RS);
    }
  }

  esp_err_t err = i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    s_shown_valid = false;
    return err;
  }
  s_shown = *wanted;
  s_shown_valid = true;
  return ESP_OK;
}

esp_err_t lcd_define_char(int index, const uint8_t rows[8]) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  if (index < 0 || index > 7) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t buf[BYTES_PER_LCD_BYTE * (1 + 8)];
  size_t len = append_byte(buf, 0, (uint8_t)(CMD_SET_CGRAM | (index << 3)), 0);
  for (int i = 0; i < 8; i++) {
    len = append_byte(buf, len, rows[i] & 0x1F, BIT_RS);
  }
  // This leaves the LCD addressing its character-generator memory. That is
  // harmless: every lcd_show run starts by setting the display address.
  return i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t lcd_set_backlight(bool on) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  s_backlight = on ? BIT_BACKLIGHT : 0;
  // A byte with EN low changes the backlight and nothing else.
  uint8_t value = s_backlight;
  return i2c_master_transmit(s_dev, &value, 1, I2C_TIMEOUT_MS);
}
```

- [ ] **Step 3: Update the component's build file**

Replace `components/dap_hw/CMakeLists.txt` with:

```cmake
# Drivers for the player's hardware. Depends on ESP-IDF; not built on the host.
idf_component_register(SRCS "storage.c" "audio_out.c" "lcd.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_core
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s
                                     esp_driver_i2c)
```

(`dap_core` is a public requirement because `lcd.h` includes `screen.h`.)

- [ ] **Step 4: Show the first screen at start-up**

In `main/app_main.c`:

Add these includes, keeping the include list in alphabetical order:

```c
#include "keytest.h"
#include "lcd.h"
#include "screen.h"
```

Add this function above `find_flac_in`:

```c
// Bring up the LCD and show the first screen. The LCD is not required for
// audio, so a failure here is logged and the player carries on.
static void start_display(void) {
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
    return;
  }
  lcd_define_char(KEYTEST_GLYPH_CODE - 8, KEYTEST_GLYPH);

  screen_t screen;
  keytest_render(&screen, NULL, 0);
  lcd_show(&screen);
}
```

Make it the first thing `app_main` does, before `storage_mount()`:

```c
  // Screen first: it works with or without a card.
  start_display();
```

- [ ] **Step 5: Write the wiring guide**

Create `docs/wiring.md`:

````markdown
# Wiring

Wire by the **label printed on each part**, never by position in a table or
photo. Unplug the USB cable before adding or moving any wire.

Pin numbers below are the numbers printed beside the pins on the Freenove
ESP32-WROVER board. Two pins are labelled with names instead of numbers:
**VP is GPIO 36** and **VN is GPIO 39**.

## Already wired

| Part | Pin label on the part | ESP32 pin |
|---|---|---|
| DAC | SCK | GND |
| DAC | BCK | 26 |
| DAC | DIN | 22 |
| DAC | LCK | 25 |
| DAC | VCC | 3.3V |
| DAC | GND | GND |

The SD card slot is built into the board (pins 14, 15 and 2 internally).

## LCD (LCD1602 with I2C backpack)

| Pin label on the backpack | ESP32 pin |
|---|---|
| GND | GND |
| VCC | 5V |
| SDA | 21 |
| SCL | 27 |

SCL goes to **27**, not 22 as in Freenove's tutorial: 22 is the DAC's data
line. 5V goes to the LCD's VCC and nowhere else.

If the backlight is on but no text shows, turn the small blue screw on the
back of the LCD (the contrast adjuster) until characters appear.

## Keypad (4x4 membrane)

Hold the keypad with the keys facing you and the ribbon at the bottom. The
eight pins are numbered 1 to 8 from left to right.

| Keypad pin | What it is | ESP32 pin | Also needs |
|---|---|---|---|
| 1 | Row 1 (keys 1 2 3 A) | 32 | |
| 2 | Row 2 (keys 4 5 6 B) | 33 | |
| 3 | Row 3 (keys 7 8 9 C) | 4 | |
| 4 | Row 4 (keys * 0 # D) | 13 | |
| 5 | Column 1 (keys 1 4 7 *) | 34 | 10k resistor to 3.3V |
| 6 | Column 2 (keys 2 5 8 0) | 35 | 10k resistor to 3.3V |
| 7 | Column 3 (keys 3 6 9 #) | VP (36) | 10k resistor to 3.3V |
| 8 | Column 4 (keys A B C D) | VN (39) | 10k resistor to 3.3V |

### The four resistors

Pins 34, 35, VP and VN can only read, and have no built-in pull-up. Each of
the four column wires therefore needs a 10k resistor (brown, black, orange)
from that wire to **3.3V**. On a breadboard: put each column wire in its own
row, and bridge each of those rows to the 3.3V rail with one resistor.

**The resistors go to 3.3V, never 5V.** 5V on these pins can damage the chip.

Without the resistors the keys read as random presses. The firmware checks
for this at start-up and logs which column is affected.

### If the keys come out wrong

Getting the keypad's eight wires in the wrong order cannot damage anything.
The keypad test screen shows which key the player thinks was pressed. If
they do not match, note what each key shows; it is fixed by reordering the
pin list in `main/pins.h`, not by rewiring.

## Pins to leave alone

- **0 and 12**: used by the chip while starting up. 12 held high at reset
  stops the board booting.
- **5, 18, 19, 23**: free, reserved for future hardware.
````

- [ ] **Step 6: Build**

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: `Project build complete.` with no compiler warnings from `main/`, `components/dap_hw/` or `components/dap_core/`.

Run: `grep -rnE "GPIO_NUM_[0-9]+|\b(gpio|pin)\w* *= *[0-9]+" main components --include=*.c --include=*.h | grep -v "main/pins.h"`
Expected: no output.

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 7: Flash and check on the board**

```bash
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 60 "FLAC: " > "$LOG"; echo "capture exit: $?"' | newgrp uucp
```

Set `LOG` to a file in your temporary directory and substitute it into the command (the inner shell does not inherit your variable).

Then, with colour codes and carriage returns stripped (`tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;]*m//g'`):

- `grep -E "lcd:|Running without an LCD"` — expected with no LCD wired: `W (...) lcd: No LCD found at 0x27 or 0x3F` and `W (...) dap: Running without an LCD (ESP_ERR_NOT_FOUND)`. With an LCD wired: `I (...) lcd: LCD found at 0x27` (or `0x3F`). Either is a pass; report which.
- `grep -E "Playing|FLAC:"` — expected: `Playing /sdcard/Clay Pigeons.flac` and `FLAC: 44100 Hz, 2 channels, 16 bits`.
- `grep -cE "Guru Meditation|abort\(\)|Backtrace|i2c.*(error|fail)"` — expected: `0`. If the count is not 0, include the matching lines in your report.

Work out from the log timestamps how long start-up took from boot to the `Playing` line, and compare with the baseline's `Playing` timestamp (1585 ms). Report both numbers.

- [ ] **Step 8: Commit**

```bash
git add components/dap_hw main/app_main.c docs/wiring.md
git commit -m "Add LCD driver and wiring guide; show the key test screen at start-up

The driver keeps a copy of what the LCD shows and sends only the cells
that changed. With no LCD connected the player logs it and carries on.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Wiring pause (run by the user)

Before Task 4, with the USB cable unplugged, wire the LCD and the keypad following `docs/wiring.md`, then plug the board back in. Task 4's board check expects both to be connected and nobody touching the keypad while it runs.

---

### Task 4: Keypad driver, keypad task and UI task

**Files:**
- Create: `components/dap_hw/keypad.h`, `components/dap_hw/keypad.c`
- Modify: `components/dap_hw/CMakeLists.txt`
- Replace: `main/app_main.c`

**Interfaces:**
- Consumes: `keys_t`, `keys_init`, `keys_update`, `key_event_t`, `KEY_LONG` (Task 2); `keytest_render`, `keytest_event_name`, `KEYTEST_GLYPH`, `KEYTEST_GLYPH_CODE` (Task 2); `lcd_init`, `lcd_show`, `lcd_define_char`, `lcd_set_backlight` (Task 3); `PIN_KEYPAD_ROW_1..4`, `PIN_KEYPAD_COL_1..4`, `PIN_LCD_SDA`, `PIN_LCD_SCL` (`main/pins.h`).
- Produces (`keypad.h`):
  - `typedef struct { int rows[KEYPAD_ROWS]; int cols[KEYPAD_COLS]; } keypad_pins_t;`
  - `esp_err_t keypad_init(const keypad_pins_t *pins);`
  - `uint16_t keypad_scan(void);`
  - `uint8_t keypad_low_columns(void);`
  - `esp_err_t keypad_start(QueueHandle_t events);`

Behaviour of the temporary UI: the LCD shows the keypad test screen; every key event is also logged as `key <char> <press|long|repeat|release>`; holding `#` toggles the backlight. Audio plays as before in the main task.

- [ ] **Step 1: Write the driver header**

Create `components/dap_hw/keypad.h`:

```c
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
```

- [ ] **Step 2: Write the driver**

Create `components/dap_hw/keypad.c`:

```c
#include "keypad.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"
#include "keys.h"

#define SCAN_PERIOD_MS 10
#define TASK_STACK 3072
#define TASK_PRIORITY 6
#define TASK_CORE 0

// Time for a column to settle after its row is pulled low. The line is a few
// centimetres of wire through a 10k pull-up, which settles in about 1 us;
// this leaves a wide margin and is far too short to hand to the scheduler.
#define SETTLE_US 10

_Static_assert(pdMS_TO_TICKS(SCAN_PERIOD_MS) >= 1,
               "the scan period must be at least one tick");

static keypad_pins_t s_pins;
static QueueHandle_t s_events;

esp_err_t keypad_init(const keypad_pins_t *pins) {
  s_pins = *pins;

  uint64_t row_mask = 0;
  uint64_t col_mask = 0;
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    row_mask |= 1ULL << s_pins.rows[r];
    // Released (floating) before the pin becomes an output, so no row is
    // driven low by accident.
    gpio_set_level((gpio_num_t)s_pins.rows[r], 1);
  }
  for (int c = 0; c < KEYPAD_COLS; c++) {
    col_mask |= 1ULL << s_pins.cols[c];
  }

  gpio_config_t row_config = {
      .pin_bit_mask = row_mask,
      .mode = GPIO_MODE_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&row_config);
  if (err != ESP_OK) {
    return err;
  }

  // The column pins are input-only and have no internal pull resistors; the
  // external 10k pull-ups do that job.
  gpio_config_t col_config = {
      .pin_bit_mask = col_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&col_config);
}

uint16_t keypad_scan(void) {
  uint16_t down = 0;
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    gpio_set_level((gpio_num_t)s_pins.rows[r], 0);
    esp_rom_delay_us(SETTLE_US);
    for (int c = 0; c < KEYPAD_COLS; c++) {
      if (gpio_get_level((gpio_num_t)s_pins.cols[c]) == 0) {
        down |= (uint16_t)(1u << (r * KEYPAD_COLS + c));
      }
    }
    gpio_set_level((gpio_num_t)s_pins.rows[r], 1);
  }
  return down;
}

uint8_t keypad_low_columns(void) {
  uint8_t low = 0;
  for (int c = 0; c < KEYPAD_COLS; c++) {
    if (gpio_get_level((gpio_num_t)s_pins.cols[c]) == 0) {
      low |= (uint8_t)(1u << c);
    }
  }
  return low;
}

static void keypad_task(void *arg) {
  (void)arg;
  keys_t keys;
  keys_init(&keys);

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SCAN_PERIOD_MS));
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    key_event_t event;
    if (keys_update(&keys, keypad_scan(), now_ms, &event)) {
      xQueueSend(s_events, &event, 0); // never block the scan
    }
  }
}

esp_err_t keypad_start(QueueHandle_t events) {
  if (!events) {
    return ESP_ERR_INVALID_ARG;
  }
  s_events = events;
  BaseType_t ok = xTaskCreatePinnedToCore(keypad_task, "keypad", TASK_STACK,
                                          NULL, TASK_PRIORITY, NULL, TASK_CORE);
  return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
```

- [ ] **Step 3: Update the component's build file**

Replace `components/dap_hw/CMakeLists.txt` with:

```cmake
# Drivers for the player's hardware. Depends on ESP-IDF; not built on the host.
idf_component_register(SRCS "storage.c" "audio_out.c" "lcd.c" "keypad.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_core
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s
                                     esp_driver_i2c esp_driver_gpio)
```

- [ ] **Step 4: Replace `main/app_main.c`**

The part from `find_flac_in` to the end is unchanged apart from the call at the top of `app_main`. `start_display` from Task 3 is replaced by `start_display_and_keys`.

```c
#include "audio_out.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad.h"
#include "keys.h"
#include "keytest.h"
#include "lcd.h"
#include "pins.h"
#include "player.h"
#include "screen.h"
#include "storage.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define KEY_QUEUE_LENGTH 16
#define UI_TASK_STACK 8192
#define UI_TASK_PRIORITY 5
#define UI_TASK_CORE 0

static const char *TAG = "dap";

static QueueHandle_t s_key_events;

// Temporary main screen for stage 2: the keypad test. Holding '#' toggles the
// backlight.
static void ui_task(void *arg) {
  (void)arg;
  screen_t screen;
  unsigned count = 0;
  bool backlight = true;

  keytest_render(&screen, NULL, 0);
  lcd_show(&screen); // does nothing useful if there is no LCD; that is fine

  for (;;) {
    key_event_t event;
    if (xQueueReceive(s_key_events, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    count++;
    ESP_LOGI(TAG, "key %c %s", event.key, keytest_event_name(event.type));

    if (event.type == KEY_LONG && event.key == '#') {
      backlight = !backlight;
      lcd_set_backlight(backlight);
    }
    keytest_render(&screen, &event, count);
    lcd_show(&screen);
  }
}

// Bring up the LCD and keypad. Neither is required for audio, so every
// failure here is logged and the player carries on.
static void start_display_and_keys(void) {
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err == ESP_OK) {
    lcd_define_char(KEYTEST_GLYPH_CODE - 8, KEYTEST_GLYPH);
  } else {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
  }

  const keypad_pins_t keypad_pins = {
      .rows = {PIN_KEYPAD_ROW_1, PIN_KEYPAD_ROW_2, PIN_KEYPAD_ROW_3,
               PIN_KEYPAD_ROW_4},
      .cols = {PIN_KEYPAD_COL_1, PIN_KEYPAD_COL_2, PIN_KEYPAD_COL_3,
               PIN_KEYPAD_COL_4},
  };
  err = keypad_init(&keypad_pins);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Keypad setup failed (%s)", esp_err_to_name(err));
    return;
  }
  uint8_t low = keypad_low_columns();
  for (int c = 0; c < KEYPAD_COLS; c++) {
    if (low & (1u << c)) {
      ESP_LOGW(TAG,
               "Keypad column %d reads low with no key pressed: check its 10k "
               "pull-up to 3.3V",
               c + 1);
    }
  }
  if (low == 0) {
    ESP_LOGI(TAG, "Keypad columns all read high");
  }

  s_key_events = xQueueCreate(KEY_QUEUE_LENGTH, sizeof(key_event_t));
  if (!s_key_events) {
    ESP_LOGE(TAG, "Out of memory for the key queue");
    return;
  }
  err = keypad_start(s_key_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Keypad task failed to start (%s)", esp_err_to_name(err));
    return;
  }
  if (xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL,
                              UI_TASK_PRIORITY, NULL, UI_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "UI task failed to start");
  }
}

// Find the first .flac in one directory.
static bool find_flac_in(const char *dir_path, char *out, size_t out_len) {
  DIR *dir = opendir(dir_path);
  if (!dir) {
    return false;
  }
  struct dirent *ent;
  bool found = false;
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (dot && strcasecmp(dot, ".flac") == 0) {
      snprintf(out, out_len, "%s/%s", dir_path, ent->d_name);
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Look in the music folder first, then the top level of the card.
static bool find_flac(char *out, size_t out_len) {
  return find_flac_in(STORAGE_MOUNT_POINT "/music", out, out_len) ||
         find_flac_in(STORAGE_MOUNT_POINT, out, out_len);
}

void app_main(void) {
  // Screen and keys first: they work with or without a card.
  start_display_and_keys();

  if (storage_mount() != ESP_OK) {
    ESP_LOGE(TAG, "No usable SD card; nothing to play");
    return;
  }

  const audio_out_pins_t pins = {
      .bck = PIN_I2S_BCK,
      .lrck = PIN_I2S_LRCK,
      .dout = PIN_I2S_DOUT,
  };
  audio_out_init(&pins);
  player_init();

  // Play the first FLAC we find. The card stays mounted afterwards.
  char track[320];
  if (!find_flac(track, sizeof(track))) {
    ESP_LOGW(TAG, "No .flac found in " STORAGE_MOUNT_POINT "/music");
    return;
  }
  ESP_LOGI(TAG, "Playing %s", track);
  player_play_file(track);
}
```

- [ ] **Step 5: Build**

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: `Project build complete.` with no compiler warnings from `main/`, `components/dap_hw/` or `components/dap_core/`.

Run: `grep -rnE "GPIO_NUM_[0-9]+|\b(gpio|pin)\w* *= *[0-9]+" main components --include=*.c --include=*.h | grep -v "main/pins.h"`
Expected: no output.

Run: `git diff --stat HEAD -- main/player.c components/dap_hw/audio_out.c components/dap_hw/storage.c`
Expected: no output (audio and storage untouched).

- [ ] **Step 6: Flash and verify on the board**

The LCD and keypad should now be wired and nobody should touch the keypad during this run. The capture lasts one full track (about 4.5 minutes): run it in the background and wait.

```bash
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 900 "blocks decoded" > "$LOG"; echo "capture exit: $?"' | newgrp uucp
```

(Substitute a real path for `$LOG` as in Task 3.)

Checks, on the log with colour codes and carriage returns stripped:

1. Playback matches the baseline:
   `diff <(grep -oE "(Playing .*|FLAC: .*|Finished: .*)" docs/superpowers/plans/stage1-baseline.log | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g') <(grep -oE "(Playing .*|FLAC: .*|Finished: .*)" "$CLEAN_LOG") && echo SAME`
   Expected: `SAME`.
2. `grep -cE "Decode failed|Guru Meditation|abort\(\)|Backtrace|sdmmc.*(error|fail)|i2c.*(error|fail)"` — expected `0`.
3. `grep -c "Not found frame in max frame size"` — expected `1` (the known defect, unchanged).
4. `grep -E "lcd:|Running without an LCD"` — expected `LCD found at 0x27` (or `0x3F`).
5. `grep -E "Keypad column|Keypad columns"` — expected `Keypad columns all read high`.
6. `grep -c "dap: key "` — expected `0` (nobody pressed anything).

Checks 1 to 3 are about the code. If any fails, do not adjust anything to make it pass: report DONE_WITH_CONCERNS or BLOCKED with the lines.

Checks 4 to 6 depend on the user's wiring. If one fails, that is a wiring finding, not a code failure: report DONE_WITH_CONCERNS and quote the lines exactly (for example which column reads low, or which keys appear as phantom presses).

- [ ] **Step 7: Commit**

```bash
git add components/dap_hw main/app_main.c
git commit -m "Add keypad driver, keypad task and the key test UI task

Rows are scanned open-drain every 10 ms; debounced events go to a queue
that the UI task reads. Holding # toggles the backlight.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Stage 2 hardware check (run by the user)

Agents can read the serial log but cannot see the LCD or press keys.

1. **Text is readable.** The LCD shows `Key test       0` on the top row and `Press any key` with a small triangle at the right of the bottom row. If the backlight is on but there is no text, turn the contrast screw on the back.
2. **All 16 keys.** Press each key once. The bottom row shows that key's character and `press`, then `release`. Note any key that shows the wrong character or nothing.
3. **Long press and repeat.** Hold `8`. After a little over half a second it shows `8 long`, then `8 repeat` with the counter climbing steadily, then noticeably faster after two seconds.
4. **Two keys.** Hold `5`, then press `6` as well: nothing changes. Release `5`: it shows `5 release` then `6 press`.
5. **Backlight.** Hold `#`: the backlight goes off. Hold it again: it comes back, with the text still there.
6. **Music is unaffected.** Reset the board and press keys while the track plays: no clicks, stutters or dropouts.
7. **Optional, with a multimeter:** measure the current on the 5V wire to the LCD with the backlight on and off, to replace the 20 mA estimate in the spec.
