#pragma once

#include <stddef.h>
#include <stdint.h>

// A marquee: text too long for its space moves through it from right to left,
// round and round, with a gap between the end and the start, and rests for a
// moment each time the start comes back to the left edge. Text that fits does
// not move.
//
// It is a pure function of elapsed time: nothing to keep between calls.

#define MARQUEE_REST_MS 1500 // how long it rests with the start at the left
#define MARQUEE_STEP_MS 350  // time per one-character step
#define MARQUEE_GAP 3        // spaces between the end and the start coming round

// Write exactly `width` characters to `out` (no terminator): the part of
// `text` visible `elapsed_ms` after it was first shown. Text of `width`
// characters or fewer is shown as it is, padded with spaces. `text` may be
// NULL or empty (all spaces).
void marquee_window(const char *text, size_t width, uint32_t elapsed_ms,
                    char *out);

// How many milliseconds from `elapsed_ms` until the window next changes, or
// UINT32_MAX if it never will (the text fits). Lets the caller sleep until
// there is something new to draw.
uint32_t marquee_next_change_ms(const char *text, size_t width,
                                uint32_t elapsed_ms);
