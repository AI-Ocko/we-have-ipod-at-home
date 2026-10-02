#include "menu.h"

#include <stddef.h>
#include <string.h>

// Design notes
//
// Nothing here knows what the items are. A list of 5,000 songs costs the
// same as one of five settings: two numbers, and no step of any function
// below depends on the count except the binary search of a letter jump.
//
// GROUPS
//
// The library orders all text by its bytes with A-Z folded to a-z, and only
// then by the bytes as written (library.c, ORDER). So in any list sorted
// that way, what decides where an item's first character puts it is the
// folded byte `f`, and the byte values fall into three runs:
//
//   f < 'a'          '#'       the end of an empty text (0), space, digits,
//                              and all punctuation below 'a'. Folding moves
//                              A-Z up to a-z, so this includes the six
//                              characters between 'Z' and 'a':
//                              [ \ ] ^ _ and the back-quote. '@' too.
//   'a' <= f <= 'z'  'A'..'Z'  a letter of either case, as its capital
//   f > 'z'          '~'       { | } ~, DEL, and every byte above 127
//
// The runs are in rising order of `f`, so the groups never go back along a
// sorted list. The characters chosen for them are in the same order as
// bytes ('#' < 'A' .. 'Z' < '~'), which lets a jump compare two groups
// directly.
//
// JUMPS
//
// Both jumps are a search for the first item of a group, by halving, in the
// part of the list on the far side of the cursor. Each costs one call to
// see a group and one per halving: at most 14 for 5,000 items. A walk would
// touch every item of a group (hundreds of rows of the index, in slow
// memory, for one key press); this touches a dozen.

void menu_reset(menu_t *m, uint32_t count) {
  if (m == NULL) {
    return;
  }
  m->count = count;
  m->cursor = 0;
}

void menu_set_count(menu_t *m, uint32_t count) {
  if (m == NULL) {
    return;
  }
  m->count = count;
  if (count == 0) {
    m->cursor = 0;
  } else if (m->cursor >= count) {
    m->cursor = count - 1;
  }
}

void menu_move(menu_t *m, int32_t delta, bool wrap) {
  if (m == NULL) {
    return;
  }
  if (m->count == 0) {
    m->cursor = 0;
    return;
  }
  uint32_t last = m->count - 1;
  if (m->cursor > last) {
    m->cursor = last;
  }
  // Only a single step goes round. A held key repeats single steps, so it
  // goes round too; a page or a jump must not, or it would overshoot to
  // somewhere the user did not ask for.
  if (wrap && delta == 1 && m->cursor == last) {
    m->cursor = 0;
    return;
  }
  if (wrap && delta == -1 && m->cursor == 0) {
    m->cursor = last;
    return;
  }
  // 64 bits hold any 32-bit position plus any 32-bit delta of either sign.
  int64_t to = (int64_t)m->cursor + delta;
  if (to < 0) {
    m->cursor = 0;
  } else if (to > (int64_t)last) {
    m->cursor = last;
  } else {
    m->cursor = (uint32_t)to;
  }
}

char menu_group(const char *text) {
  uint8_t c = text != NULL ? (uint8_t)text[0] : 0;
  if ((uint8_t)(c - 'A') < 26u) {
    return (char)c;
  }
  if ((uint8_t)(c - 'a') < 26u) {
    return (char)(c - ('a' - 'A'));
  }
  // Not a letter, so folding leaves it as it is: see GROUPS above.
  return c < 'a' ? '#' : '~';
}

// The first item in [lo, hi) whose group is not before `g`, or `hi` if there
// is none. Asks only about items in [lo, hi).
static uint32_t first_not_before(menu_group_fn group, void *ctx, uint32_t lo,
                                 uint32_t hi, uint8_t g) {
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if ((uint8_t)group(ctx, mid) >= g) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// The first item in [lo, hi) whose group is after `g`, or `hi`.
static uint32_t first_after(menu_group_fn group, void *ctx, uint32_t lo,
                            uint32_t hi, uint8_t g) {
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if ((uint8_t)group(ctx, mid) > g) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

void menu_jump_group(menu_t *m, int direction, menu_group_fn group, void *ctx) {
  if (m == NULL || group == NULL || direction == 0 || m->count == 0) {
    return;
  }
  if (m->cursor >= m->count) {
    m->cursor = m->count - 1;
  }
  uint32_t cursor = m->cursor;
  if (direction > 0) {
    uint8_t here = (uint8_t)group(ctx, cursor);
    uint32_t next = first_after(group, ctx, cursor + 1, m->count, here);
    if (next < m->count) {
      m->cursor = next;
    } // else this is the last group: stay
    return;
  }
  if (cursor == 0) {
    return;
  }
  // Both cases of going back are one question: where does the group of the
  // item above the cursor start? If that item is in the cursor's group, the
  // answer is the start of this group; if it is not, the cursor is at the
  // start of its group and the answer is the start of the previous one. So
  // the cursor's own group need not be read at all.
  uint8_t above = (uint8_t)group(ctx, cursor - 1);
  m->cursor = first_not_before(group, ctx, 0, cursor - 1, above);
}

// Write `value` in decimal ending just before `end`; returns where it
// starts. At most 10 characters.
static char *number_before(char *end, uint32_t value) {
  do {
    *--end = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  return end;
}

// The rules, in the order they are applied:
//
// 1. An empty list has no position: the name alone, cut at the row's width.
// 2. The position is "cursor/count". If that is wider than the row (both
//    numbers in the millions), it is the cursor's number alone, which is at
//    most ten digits and always fits. A number is never cut: half a number
//    is a wrong number.
// 3. The position is at the right edge. The name gets the columns to the
//    left of the position and the one space before it, and is cut to them;
//    with a position of 15 or 16 characters that is no name at all.
void menu_header(const char *name, const menu_t *m,
                 char out[MENU_HEADER_WIDTH]) {
  char position[10 + 1 + 10];
  char *end = position + sizeof(position);
  char *start = end;
  if (m != NULL && m->count != 0) {
    // A cursor past the end is shown as the last item, never "99/87".
    uint32_t cursor = m->cursor < m->count ? m->cursor : m->count - 1;
    start = number_before(end, m->count);
    *--start = '/';
    char *slash = start;
    start = number_before(start, cursor + 1); // <= count, so no overflow
    if ((size_t)(end - start) > MENU_HEADER_WIDTH) {
      end = slash;
    }
  }
  size_t position_len = (size_t)(end - start);

  size_t left = MENU_HEADER_WIDTH - position_len; // columns before it
  size_t name_room = position_len == 0 ? left : left > 0 ? left - 1 : 0;
  size_t i = 0;
  if (name != NULL) {
    for (; i < name_room && name[i] != '\0'; i++) {
      out[i] = name[i];
    }
  }
  for (; i < left; i++) {
    out[i] = ' ';
  }
  memcpy(out + left, start, position_len);
}
