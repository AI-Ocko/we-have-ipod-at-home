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
