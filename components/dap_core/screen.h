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
