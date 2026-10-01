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
