#pragma once

#include <stdbool.h>
#include <stdint.h>

// A cursor over a list of `count` items. The list's contents belong to
// whoever uses this; the menu only knows how many there are and where it is.

typedef struct {
  uint32_t count;
  uint32_t cursor; // 0 .. count - 1; 0 when the list is empty
} menu_t;

// A list of `count` items with the cursor on the first.
void menu_reset(menu_t *m, uint32_t count);

// The list now has `count` items. The cursor stays where it is if it can,
// and moves to the last item if it cannot.
void menu_set_count(menu_t *m, uint32_t count);

// Move by `delta` items. A single step past either end goes round to the
// other end if `wrap` is true; anything else stops at the end.
void menu_move(menu_t *m, int32_t delta, bool wrap);

// The group an item belongs to for letter jumps, from its text: 'A'..'Z' for
// a first character that is a letter (either case), '#' for one that sorts
// before the letters (digits, most punctuation, an empty text), '~' for one
// that sorts after them. These groups come in this order in every list that
// is sorted the library's way.
char menu_group(const char *text);

// The group of the item at `index`.
typedef char (*menu_group_fn)(void *ctx, uint32_t index);

// Jump to the first item of the next group (direction > 0), or back to the
// first item of this group, or of the previous group if already there
// (direction < 0). The list must be sorted by group. Calls `group` O(log n)
// times.
void menu_jump_group(menu_t *m, int direction, menu_group_fn group, void *ctx);

#define MENU_HEADER_WIDTH 16

// The top row: `name` on the left and "cursor/count" (counting from 1) on the
// right, with at least one space between; the name is cut short if it must
// be. An empty list shows the name only. Writes exactly MENU_HEADER_WIDTH
// characters, no terminator.
void menu_header(const char *name, const menu_t *m, char out[MENU_HEADER_WIDTH]);
