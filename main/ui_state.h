#pragma once

#include "browse.h"
#include "keys.h"
#include "menu.h"
#include "player_status.h"
#include "screen.h"
#include <stdbool.h>
#include <stdint.h>

// The UI task's state, shared by ui.c (which changes it) and ui_screens.c
// (which draws it). Private to those two. The design is described at the top
// of ui.c.

// A name, a title or an artist from the index: at most 127 characters.
#define UI_TEXT_MAX 128
// A track's path: the mount point and a path from the index (256 bytes).
#define UI_PATH_MAX 320
// The top row of Now Playing: a file name (at most 255 characters on the
// card), or a title and an artist (at most 127 each) with " - " between.
#define UI_TITLE_MAX 260

#define UI_OVERLAY_MS 1500 // how long the volume bar stays

typedef enum {
  UI_MODE_MENU,        // the menu tree (or what covers it: see ui.c)
  UI_MODE_NOW_PLAYING,
  UI_MODE_KEYTEST,
  UI_MODE_ABOUT,
} ui_mode_t;

typedef struct {
  ui_mode_t mode;

  // What the rest of the system has said.
  bool no_card;        // start-up is still waiting for a card that mounts
  bool scanning;       // the library is being scanned
  uint32_t scan_tracks;
  uint32_t generation; // of the index the numbers held below belong to

  // The menus. `browse` holds numbers only. The rest is a copy of what the
  // current list shows, made once per cursor position (ui_fill_list), so
  // that drawing and the marquee never go to the index.
  browse_t browse;
  bool list_dirty;   // the copy is out of date
  bool list_restart; // the cursor or the level changed: restart the marquee
  menu_t list_menu;  // the current level's count and cursor
  char list_name[UI_TEXT_MAX];
  char list_item[UI_TEXT_MAX]; // the item under the cursor
  int64_t item_since_us;       // the marquee's zero

  // The volume bar: on the bottom row until this time. 0 when it is off.
  int64_t volume_until_us;

  // The key test screen.
  bool keytest_any; // an event has been shown
  key_event_t keytest_last;
  unsigned keytest_count;
  char keytest_opened_by; // the key whose release is not to be shown

  // About.
  uint32_t about_tracks;

  // The track that is playing, or the last one that was.
  char path[UI_PATH_MAX];   // as the player has it; "" if none yet
  char title[UI_TITLE_MAX]; // "Title - Artist", or the file name
  int64_t title_since_us;   // the marquee's zero
  uint32_t elapsed_ms;
  uint32_t duration_ms;
  bool failed; // the last `play` ended in an error

  // Key repeats not yet logged (see log_key in ui.c).
  char repeat_key;
  uint32_t repeat_count;

  uint32_t underruns; // how many have been reported so far
} ui_t;

// Whether the menu tree is what the menu side shows: not while the card is
// still awaited or a scan runs (those screens cover it).
static inline bool ui_list_on_show(const ui_t *ui) {
  return ui->mode == UI_MODE_MENU && !ui->no_card && !ui->scanning;
}

// --- ui_screens.c: drawing ---------------------------------------------------

// Send the user-defined characters to the LCD. Call once, after lcd_init and
// before the UI task runs.
void ui_screens_define_glyphs(void);

// Fill `screen` from the state: what the LCD should show at `now_us`. A pure
// function of its arguments; it reads no index, no file and no clock.
void ui_render(const ui_t *ui, const player_status_t *player, int64_t now_us,
               screen_t *screen);

// How many milliseconds after `now_us` the screen will next change with no
// event at all (a marquee step, an overlay running out), or UINT32_MAX if it
// will not.
uint32_t ui_next_change_ms(const ui_t *ui, int64_t now_us);
