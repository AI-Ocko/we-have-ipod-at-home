#pragma once

#include <stdint.h>

// What the 16x2 LCD should show, held in memory. Screens are drawn into a
// screen_t and handed to the LCD driver, which sends only the cells that
// differ from what is already displayed.

#define SCREEN_ROWS 2
#define SCREEN_COLS 16

// Most runs screen_diff can produce: runs on a row start at least
// SCREEN_MERGE_GAP + 2 cells apart, so at most six fit on a row.
#define SCREEN_MAX_RUNS 12

// Two changed stretches separated by this many unchanged cells or fewer are
// sent as one run. A whole update goes to the LCD in one I2C transfer, where
// starting a new run costs one cursor move, the same as sending one
// character. Bridging a single unchanged cell therefore costs no more than
// jumping it and keeps the run count down; a wider gap is cheaper to jump.
#define SCREEN_MERGE_GAP 1

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
