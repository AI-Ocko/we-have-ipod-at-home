#include "menu.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static menu_t menu_at(uint32_t count, uint32_t cursor) {
  menu_t m = {.count = count, .cursor = cursor};
  return m;
}

// --- The cursor --------------------------------------------------------------

void test_reset_puts_the_cursor_first(void) {
  menu_t m = menu_at(3, 2);
  menu_reset(&m, 87);
  TEST_ASSERT_EQUAL_UINT32(87, m.count);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  menu_reset(&m, 0);
  TEST_ASSERT_EQUAL_UINT32(0, m.count);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
}

void test_move_down_and_up(void) {
  menu_t m;
  menu_reset(&m, 10);
  menu_move(&m, 1, false);
  TEST_ASSERT_EQUAL_UINT32(1, m.cursor);
  menu_move(&m, 1, true);
  TEST_ASSERT_EQUAL_UINT32(2, m.cursor);
  menu_move(&m, 5, false);
  TEST_ASSERT_EQUAL_UINT32(7, m.cursor);
  menu_move(&m, -1, false);
  TEST_ASSERT_EQUAL_UINT32(6, m.cursor);
  menu_move(&m, -4, true);
  TEST_ASSERT_EQUAL_UINT32(2, m.cursor);
  menu_move(&m, 0, true);
  TEST_ASSERT_EQUAL_UINT32(2, m.cursor);
  TEST_ASSERT_EQUAL_UINT32(10, m.count);
}

void test_single_step_wraps_at_both_ends_when_asked(void) {
  menu_t m;
  menu_reset(&m, 10);
  menu_move(&m, -1, true);
  TEST_ASSERT_EQUAL_UINT32(9, m.cursor);
  menu_move(&m, 1, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
}

void test_single_step_stops_at_the_ends_when_not_wrapping(void) {
  menu_t m;
  menu_reset(&m, 10);
  menu_move(&m, -1, false);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  m.cursor = 9;
  menu_move(&m, 1, false);
  TEST_ASSERT_EQUAL_UINT32(9, m.cursor);
}

void test_big_moves_never_wrap(void) {
  menu_t m = menu_at(10, 7);
  menu_move(&m, 10, true);
  TEST_ASSERT_EQUAL_UINT32(9, m.cursor);
  // Even from the very end, where one step would have gone round.
  menu_move(&m, 2, true);
  TEST_ASSERT_EQUAL_UINT32(9, m.cursor);
  m.cursor = 3;
  menu_move(&m, -10, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  menu_move(&m, -2, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  // A move that lands exactly on the end is not a wrap either.
  m.cursor = 4;
  menu_move(&m, 5, true);
  TEST_ASSERT_EQUAL_UINT32(9, m.cursor);
  m.cursor = 4;
  menu_move(&m, -4, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
}

void test_move_in_an_empty_list_does_nothing(void) {
  static const int32_t DELTAS[] = {1, -1, 10, -10, 0, INT32_MAX, INT32_MIN};
  for (size_t i = 0; i < sizeof(DELTAS) / sizeof(DELTAS[0]); i++) {
    menu_t m;
    menu_reset(&m, 0);
    menu_move(&m, DELTAS[i], true);
    TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
    menu_move(&m, DELTAS[i], false);
    TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
    TEST_ASSERT_EQUAL_UINT32(0, m.count);
  }
}

void test_move_in_a_one_item_list(void) {
  static const int32_t DELTAS[] = {1, -1, 10, -10, 0, INT32_MAX, INT32_MIN};
  for (size_t i = 0; i < sizeof(DELTAS) / sizeof(DELTAS[0]); i++) {
    menu_t m;
    menu_reset(&m, 1);
    menu_move(&m, DELTAS[i], true);
    TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
    menu_move(&m, DELTAS[i], false);
    TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
    TEST_ASSERT_EQUAL_UINT32(1, m.count);
  }
}

// Extra: the arithmetic mixes a signed delta with unsigned positions.
void test_moves_in_the_largest_lists_do_not_overflow(void) {
  menu_t m = menu_at(UINT32_MAX, UINT32_MAX - 3);
  menu_move(&m, INT32_MAX, true);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, m.cursor); // the last item
  menu_move(&m, 1, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  menu_move(&m, INT32_MIN, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  menu_move(&m, -1, true);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, m.cursor);
  menu_move(&m, INT32_MIN, false);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1 - 0x80000000u, m.cursor);
  menu_move(&m, INT32_MAX, false);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 2, m.cursor);
  // Past the middle of the range, where the position no longer fits a signed
  // 32-bit number.
  m.cursor = 0xC0000000u;
  menu_move(&m, -5, false);
  TEST_ASSERT_EQUAL_UINT32(0xBFFFFFFBu, m.cursor);
  menu_move(&m, 5, false);
  TEST_ASSERT_EQUAL_UINT32(0xC0000000u, m.cursor);
}

// Extra: a cursor that is somehow past the end is brought back by a move
// rather than left there.
void test_a_move_from_a_cursor_past_the_end_ends_in_the_list(void) {
  menu_t m = menu_at(5, 9);
  menu_move(&m, 1, false);
  TEST_ASSERT_EQUAL_UINT32(4, m.cursor);
  m.cursor = 9;
  menu_move(&m, -1, true);
  TEST_ASSERT_EQUAL_UINT32(3, m.cursor);
  m = menu_at(0, 9);
  menu_move(&m, 1, true);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
}

void test_set_count_keeps_or_clamps_the_cursor(void) {
  menu_t m = menu_at(10, 6);
  menu_set_count(&m, 20);
  TEST_ASSERT_EQUAL_UINT32(20, m.count);
  TEST_ASSERT_EQUAL_UINT32(6, m.cursor);
  menu_set_count(&m, 7);
  TEST_ASSERT_EQUAL_UINT32(7, m.count);
  TEST_ASSERT_EQUAL_UINT32(6, m.cursor);
  menu_set_count(&m, 4);
  TEST_ASSERT_EQUAL_UINT32(4, m.count);
  TEST_ASSERT_EQUAL_UINT32(3, m.cursor);
  menu_set_count(&m, 0);
  TEST_ASSERT_EQUAL_UINT32(0, m.count);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
  menu_set_count(&m, 5);
  TEST_ASSERT_EQUAL_UINT32(5, m.count);
  TEST_ASSERT_EQUAL_UINT32(0, m.cursor);
}

// --- Groups ------------------------------------------------------------------

void test_group_of_text(void) {
  TEST_ASSERT_EQUAL_CHAR('R', menu_group("Radiohead"));
  TEST_ASSERT_EQUAL_CHAR('R', menu_group("radiohead"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("2Pac"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group(""));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group(NULL));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("(What's the Story)"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("~tilde"));
  // The library folds A-Z to a-z before it compares, so the letters sit at
  // 'a'..'z' and the six characters between 'Z' and 'a' come before them.
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("_under"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("[bracket"));

  // Each edge of the three ranges.
  TEST_ASSERT_EQUAL_CHAR('#', menu_group(" space"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("0"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("9"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("@"));
  TEST_ASSERT_EQUAL_CHAR('A', menu_group("A"));
  TEST_ASSERT_EQUAL_CHAR('Z', menu_group("Z"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("["));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("\\"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("]"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("^"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("_"));
  TEST_ASSERT_EQUAL_CHAR('#', menu_group("`"));
  TEST_ASSERT_EQUAL_CHAR('A', menu_group("a"));
  TEST_ASSERT_EQUAL_CHAR('Z', menu_group("z"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("{"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("|"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("}"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("~"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("\x7f"));
  // Bytes above 127 come last in the library's order.
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("\x80"));
  TEST_ASSERT_EQUAL_CHAR('~', menu_group("\xff"));
}

// Extra: the claim the letter jumps rest on. Walk every possible first byte
// in the library's order (folded byte, then the byte itself) and see that
// the group never goes back.
void test_groups_never_go_back_in_the_librarys_order(void) {
  char last = '#';
  for (int folded = 0; folded < 256; folded++) {
    // The bytes whose folded value this is: itself, unless it is an upper
    // case letter (which folds away); and its upper case twin first if it is
    // a lower case letter.
    int raw[2];
    int n = 0;
    if (folded >= 'a' && folded <= 'z') {
      raw[n++] = folded - 'a' + 'A';
    }
    if (!(folded >= 'A' && folded <= 'Z')) {
      raw[n++] = folded;
    }
    for (int i = 0; i < n; i++) {
      char text[2] = {(char)raw[i], '\0'};
      char g = menu_group(text);
      TEST_ASSERT_TRUE(g == '#' || g == '~' || (g >= 'A' && g <= 'Z'));
      TEST_ASSERT_TRUE_MESSAGE((unsigned char)g >= (unsigned char)last, text);
      last = g;
    }
  }
  TEST_ASSERT_EQUAL_CHAR('~', last);
}

// --- Letter jumps ------------------------------------------------------------

// A list that is nothing but its groups, one character per item. It counts
// the calls and notices any for an item that is not there.
typedef struct {
  const char *groups;
  uint32_t count;
  uint32_t calls;
  bool out_of_range;
} list_t;

static char list_group(void *ctx, uint32_t index) {
  list_t *list = ctx;
  list->calls++;
  if (index >= list->count) {
    list->out_of_range = true;
    return '?';
  }
  return list->groups[index];
}

static list_t list_of(const char *groups) {
  list_t list = {.groups = groups, .count = (uint32_t)strlen(groups)};
  return list;
}

// Where a jump from `cursor` lands, and how many times it asked for a group.
static uint32_t jump(list_t *list, uint32_t cursor, int direction) {
  menu_t m = menu_at(list->count, cursor);
  list->calls = 0;
  list->out_of_range = false;
  menu_jump_group(&m, direction, list_group, list);
  TEST_ASSERT_FALSE_MESSAGE(list->out_of_range, "asked about a missing item");
  TEST_ASSERT_EQUAL_UINT32(list->count, m.count);
  return m.cursor;
}

// The same by walking the list, to compare with.
static uint32_t walk(const list_t *list, uint32_t cursor, int direction) {
  const char *g = list->groups;
  if (list->count == 0) {
    return 0;
  }
  if (direction > 0) {
    for (uint32_t i = cursor + 1; i < list->count; i++) {
      if (g[i] != g[cursor]) {
        return i;
      }
    }
    return cursor;
  }
  uint32_t start = cursor;
  while (start > 0 && g[start - 1] == g[cursor]) {
    start--;
  }
  if (start < cursor || start == 0) {
    return start;
  }
  uint32_t previous = start - 1;
  while (previous > 0 && g[previous - 1] == g[start - 1]) {
    previous--;
  }
  return previous;
}

//                                    0123456789012
static const char GROUPS[] = "##AAABCCCCMZ~";

void test_jump_forward_lands_on_the_first_item_of_the_next_group(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(2, jump(&list, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(2, jump(&list, 1, 1));
  TEST_ASSERT_EQUAL_UINT32(5, jump(&list, 2, 1));
  TEST_ASSERT_EQUAL_UINT32(5, jump(&list, 3, 1));
  TEST_ASSERT_EQUAL_UINT32(5, jump(&list, 4, 1));
  TEST_ASSERT_EQUAL_UINT32(6, jump(&list, 5, 1));
  TEST_ASSERT_EQUAL_UINT32(10, jump(&list, 6, 1));
  TEST_ASSERT_EQUAL_UINT32(10, jump(&list, 9, 1));
  TEST_ASSERT_EQUAL_UINT32(11, jump(&list, 10, 1));
  TEST_ASSERT_EQUAL_UINT32(12, jump(&list, 11, 1));
  // Any positive direction is forward.
  TEST_ASSERT_EQUAL_UINT32(5, jump(&list, 3, 7));
}

void test_jump_forward_from_the_last_group_stays(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(12, jump(&list, 12, 1));
  list_t ending_in_a_run = list_of("AABBB");
  TEST_ASSERT_EQUAL_UINT32(2, jump(&ending_in_a_run, 2, 1));
  TEST_ASSERT_EQUAL_UINT32(3, jump(&ending_in_a_run, 3, 1));
  TEST_ASSERT_EQUAL_UINT32(4, jump(&ending_in_a_run, 4, 1));
  list_t one_group = list_of("QQQQ");
  TEST_ASSERT_EQUAL_UINT32(1, jump(&one_group, 1, 1));
}

void test_jump_back_goes_to_the_start_of_this_group_first(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&list, 1, -1));
  TEST_ASSERT_EQUAL_UINT32(2, jump(&list, 3, -1));
  TEST_ASSERT_EQUAL_UINT32(2, jump(&list, 4, -1));
  TEST_ASSERT_EQUAL_UINT32(6, jump(&list, 7, -1));
  TEST_ASSERT_EQUAL_UINT32(6, jump(&list, 9, -1));
  TEST_ASSERT_EQUAL_UINT32(6, jump(&list, 9, -3));
}

void test_jump_back_from_a_group_start_goes_to_the_previous_group(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&list, 2, -1));
  TEST_ASSERT_EQUAL_UINT32(2, jump(&list, 5, -1));
  TEST_ASSERT_EQUAL_UINT32(5, jump(&list, 6, -1));
  TEST_ASSERT_EQUAL_UINT32(6, jump(&list, 10, -1));
  TEST_ASSERT_EQUAL_UINT32(10, jump(&list, 11, -1));
  TEST_ASSERT_EQUAL_UINT32(11, jump(&list, 12, -1));
}

void test_jump_back_from_the_first_item_stays(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&list, 0, -1));
  list_t one_group = list_of("QQQQ");
  TEST_ASSERT_EQUAL_UINT32(0, jump(&one_group, 0, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump(&one_group, 3, -1));
}

// 28 groups in order: '#', 'A'..'Z', '~'.
static char group_number(uint32_t n) {
  return n == 0 ? '#' : n == 27 ? '~' : (char)('A' + (n - 1));
}

#define BIG 5000u
static char big_groups[BIG + 1];

void test_jump_calls_group_a_logarithmic_number_of_times(void) {
  // Groups of very different sizes: a few items each at the start, most of
  // the list in the last few.
  for (uint32_t i = 0; i < BIG; i++) {
    uint64_t square = (uint64_t)i * i;
    big_groups[i] = group_number((uint32_t)(square * 28 / ((uint64_t)BIG * BIG)));
  }
  big_groups[BIG] = '\0';
  list_t list = list_of(big_groups);
  TEST_ASSERT_EQUAL_UINT32(BIG, list.count);

  uint32_t most = 0;
  for (uint32_t cursor = 0; cursor < BIG; cursor++) {
    for (int direction = -1; direction <= 1; direction += 2) {
      uint32_t to = jump(&list, cursor, direction);
      TEST_ASSERT_EQUAL_UINT32(walk(&list, cursor, direction), to);
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(40, list.calls);
      if (list.calls > most) {
        most = list.calls;
      }
    }
  }
  // The bound the search actually has: one call to see where it is and
  // one per halving of 5,000.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(14, most);
  char note[64];
  snprintf(note, sizeof(note), "most group calls for one jump in %u items: %u",
           (unsigned)BIG, (unsigned)most);
  TEST_MESSAGE(note);

  // And when the whole list is one group, where a walk would be longest.
  memset(big_groups, 'M', BIG);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&list, BIG - 1, -1));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(14, list.calls);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&list, 0, 1));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(14, list.calls);
}

void test_jump_in_empty_and_one_item_lists(void) {
  list_t empty = list_of("");
  TEST_ASSERT_EQUAL_UINT32(0, jump(&empty, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(0, empty.calls);
  TEST_ASSERT_EQUAL_UINT32(0, jump(&empty, 0, -1));
  TEST_ASSERT_EQUAL_UINT32(0, empty.calls);

  list_t one = list_of("K");
  TEST_ASSERT_EQUAL_UINT32(0, jump(&one, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(0, jump(&one, 0, -1));

  list_t two = list_of("KK");
  TEST_ASSERT_EQUAL_UINT32(0, jump(&two, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(1, jump(&two, 1, 1));
  TEST_ASSERT_EQUAL_UINT32(0, jump(&two, 1, -1));
  list_t two_groups = list_of("KL");
  TEST_ASSERT_EQUAL_UINT32(1, jump(&two_groups, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(0, jump(&two_groups, 1, -1));
}

// Extra: every cursor of many lists, against a walk. The lists are made by a
// fixed generator: each of the 28 groups gets 0 to 7 items, most often none.
void test_jumps_agree_with_a_walk_on_many_lists(void) {
  uint32_t seed = 12345;
  for (int round = 0; round < 300; round++) {
    char groups[28 * 7 + 1];
    uint32_t n = 0;
    for (uint32_t g = 0; g < 28; g++) {
      seed = seed * 1664525u + 1013904223u;
      uint32_t size = (seed >> 24) % 16;
      size = size < 8 ? 0 : size - 8;
      for (uint32_t i = 0; i < size; i++) {
        groups[n++] = group_number(g);
      }
    }
    groups[n] = '\0';
    list_t list = list_of(groups);
    for (uint32_t cursor = 0; cursor < n; cursor++) {
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(walk(&list, cursor, 1),
                                       jump(&list, cursor, 1), groups);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(walk(&list, cursor, -1),
                                       jump(&list, cursor, -1), groups);
    }
  }
}

// Extra: things a caller should not do, which must still be harmless.
void test_jump_with_no_direction_no_function_or_a_bad_cursor(void) {
  list_t list = list_of(GROUPS);
  TEST_ASSERT_EQUAL_UINT32(4, jump(&list, 4, 0));
  TEST_ASSERT_EQUAL_UINT32(0, list.calls);

  menu_t m = menu_at(list.count, 4);
  menu_jump_group(&m, 1, NULL, NULL);
  TEST_ASSERT_EQUAL_UINT32(4, m.cursor);

  // A cursor past the end is treated as the last item.
  TEST_ASSERT_EQUAL_UINT32(12, jump(&list, 40, 1));
  TEST_ASSERT_EQUAL_UINT32(11, jump(&list, 40, -1));

  // A list that is not sorted by group gives no promise about where the
  // jump lands, but it lands in the list and asks only about real items.
  list_t unsorted = list_of("ZAZAZAMMZA#~A");
  for (uint32_t cursor = 0; cursor < unsorted.count; cursor++) {
    TEST_ASSERT_LESS_THAN_UINT32(unsorted.count, jump(&unsorted, cursor, 1));
    TEST_ASSERT_LESS_THAN_UINT32(unsorted.count, jump(&unsorted, cursor, -1));
  }
}

// --- The top row -------------------------------------------------------------

// The header for `cursor` (from 0) of `count`, checked to be exactly
// `expected` and to have written nothing either side of its 16 characters.
static void assert_header(const char *expected, const char *name,
                          uint32_t cursor, uint32_t count) {
  TEST_ASSERT_EQUAL_size_t(MENU_HEADER_WIDTH, strlen(expected));
  char buf[4 + MENU_HEADER_WIDTH + 4];
  memset(buf, '\x01', sizeof(buf));
  menu_t m = menu_at(count, cursor);
  menu_header(name, &m, buf + 4);
  char got[MENU_HEADER_WIDTH + 1];
  memcpy(got, buf + 4, MENU_HEADER_WIDTH);
  got[MENU_HEADER_WIDTH] = '\0';
  TEST_ASSERT_EQUAL_STRING(expected, got);
  for (size_t i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_CHAR('\x01', buf[i]);
    TEST_ASSERT_EQUAL_CHAR('\x01', buf[4 + MENU_HEADER_WIDTH + i]);
  }
  TEST_ASSERT_EQUAL_UINT32(count, m.count);
  TEST_ASSERT_EQUAL_UINT32(cursor, m.cursor);
}

void test_header_short_name(void) {
  assert_header("Artists    14/87", "Artists", 13, 87);
}

void test_header_first_and_last(void) {
  assert_header("Artists     1/87", "Artists", 0, 87);
  assert_header("Artists    87/87", "Artists", 86, 87);
  assert_header("Main         1/5", "Main", 0, 5);
  assert_header("Main         5/5", "Main", 4, 5);
  assert_header("Only one     1/1", "Only one", 0, 1);
}

void test_header_large_numbers(void) {
  assert_header("Songs  1028/1028", "Songs", 1027, 1028);
  assert_header("Songs     1/1028", "Songs", 0, 1028);
  assert_header("Song 65535/65535", "Songs", 65534, 65535);
}

void test_header_cuts_a_long_name(void) {
  //             123456789012345678901234567890
  assert_header("The Dark Si 3/10", "The Dark Side of the Moon (30)", 2, 10);
  // A name that fits exactly is not cut; one more character is.
  assert_header("Twelve chars 1/9", "Twelve chars", 0, 9);
  assert_header("Thirteen cha 1/9", "Thirteen char", 0, 9);
  assert_header("Twelve cha 10/10", "Twelve chars", 9, 10);
}

// The rule: a number is never cut. The position is "cursor/count" if that
// fits the row, else the cursor's number alone (ten digits at most, so it
// always fits). The name gets what is left of the position and the space
// before it, which may be nothing.
void test_header_when_position_leaves_no_room_for_a_name(void) {
  // 14 characters of position: one of name.
  assert_header("S 123456/1234567", "Songs", 123455, 1234567);
  // 15: the space and no name.
  assert_header(" 1234567/1234567", "Songs", 1234566, 1234567);
  // 16: the position is the whole row.
  assert_header("9999999/10000000", "Songs", 9999998, 10000000);
  // 17 and more: the cursor alone, and the name has room again.
  assert_header("Songs   10000000", "Songs", 9999999, 10000000);
  assert_header("Songs   12345678", "Songs", 12345677, 123456789);
  assert_header("Songs 4294967295", "Songs", UINT32_MAX - 1, UINT32_MAX);
  assert_header("A long 123456789", "A long name", 123456788, UINT32_MAX);
  // The longest count with a short cursor still fits whole.
  assert_header("Son 1/4294967295", "Songs", 0, UINT32_MAX);
  assert_header("  999/4294967295", "", 998, UINT32_MAX);
}

void test_header_empty_list(void) {
  assert_header("Artists         ", "Artists", 0, 0);
  assert_header("                ", "", 0, 0);
  assert_header("Sixteen letters!", "Sixteen letters!", 0, 0);
  assert_header("A name longer th", "A name longer than the row", 0, 0);
}

// Extra: no name at all, and a cursor that is past the end.
void test_header_without_a_name_and_with_a_bad_cursor(void) {
  assert_header("           14/87", NULL, 13, 87);
  assert_header("           14/87", "", 13, 87);
  assert_header("                ", NULL, 0, 0);
  // Never "99/87": a cursor past the end shows as the last item.
  assert_header("Artists    87/87", "Artists", 98, 87);
  assert_header("Songs 4294967295", "Songs", UINT32_MAX, UINT32_MAX);

  // No menu: the name alone.
  char out[MENU_HEADER_WIDTH];
  menu_header("Artists", NULL, out);
  TEST_ASSERT_EQUAL_STRING_LEN("Artists         ", out, MENU_HEADER_WIDTH);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_reset_puts_the_cursor_first);
  RUN_TEST(test_move_down_and_up);
  RUN_TEST(test_single_step_wraps_at_both_ends_when_asked);
  RUN_TEST(test_single_step_stops_at_the_ends_when_not_wrapping);
  RUN_TEST(test_big_moves_never_wrap);
  RUN_TEST(test_move_in_an_empty_list_does_nothing);
  RUN_TEST(test_move_in_a_one_item_list);
  RUN_TEST(test_moves_in_the_largest_lists_do_not_overflow);
  RUN_TEST(test_a_move_from_a_cursor_past_the_end_ends_in_the_list);
  RUN_TEST(test_set_count_keeps_or_clamps_the_cursor);
  RUN_TEST(test_group_of_text);
  RUN_TEST(test_groups_never_go_back_in_the_librarys_order);
  RUN_TEST(test_jump_forward_lands_on_the_first_item_of_the_next_group);
  RUN_TEST(test_jump_forward_from_the_last_group_stays);
  RUN_TEST(test_jump_back_goes_to_the_start_of_this_group_first);
  RUN_TEST(test_jump_back_from_a_group_start_goes_to_the_previous_group);
  RUN_TEST(test_jump_back_from_the_first_item_stays);
  RUN_TEST(test_jump_calls_group_a_logarithmic_number_of_times);
  RUN_TEST(test_jump_in_empty_and_one_item_lists);
  RUN_TEST(test_jumps_agree_with_a_walk_on_many_lists);
  RUN_TEST(test_jump_with_no_direction_no_function_or_a_bad_cursor);
  RUN_TEST(test_header_short_name);
  RUN_TEST(test_header_first_and_last);
  RUN_TEST(test_header_large_numbers);
  RUN_TEST(test_header_cuts_a_long_name);
  RUN_TEST(test_header_when_position_leaves_no_room_for_a_name);
  RUN_TEST(test_header_empty_list);
  RUN_TEST(test_header_without_a_name_and_with_a_bad_cursor);
  return UNITY_END();
}
