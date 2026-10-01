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
