#pragma once

#include "browse.h"
#include "keys.h"
#include "menu.h"
#include "player_status.h"
#include "library.h"
#include "player.h"
#include "screen.h"
#include "state_task.h"
#include "tracklist.h"
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

#define UI_OVERLAY_MS 1500 // how long the volume bar or a notice stays
#define UI_FAILURES_MAX 3  // unplayable tracks in a row before giving up

typedef enum {
  UI_MODE_MENU,        // the menu tree (or what covers it: see ui.c)
  UI_MODE_NOW_PLAYING,
  UI_MODE_KEYTEST,
  UI_MODE_ABOUT,
} ui_mode_t;

// What the bottom row of Now Playing says in place of the time.
typedef enum {
  UI_NOTICE_CANT_PLAY,    // "Can't play": it would not decode
  UI_NOTICE_FILE_MISSING, // "File missing": gone since the last scan
} ui_notice_t;

// The track that is playing, or the last one that was, and the list it was
// chosen from. ui_play.c changes this; its design notes say how.
typedef struct {
  char path[UI_PATH_MAX];   // as given to the player; "" if there is none
  char title[UI_TITLE_MAX]; // "Title - Artist", or the file name
  int64_t title_since_us;   // the marquee's zero
  uint32_t elapsed_ms;
  uint32_t duration_ms;

  uint32_t play_id; // the `play` that is this track's: its events count
  uint32_t min_id;  // events with a lower number are old news
  bool active;      // that play has not ended, failed or been stopped

  // The track before, while its last events may still arrive: for the log.
  char prev_path[UI_PATH_MAX];
  uint32_t prev_id;

  // Where the track came from: when it ends, position + 1 of the list plays.
  tracklist_t list;
  uint32_t list_pos;
  uint32_t list_generation; // the index the list's numbers belong to

  uint32_t failures;       // tracks in a row that would not play
  ui_notice_t notice;
  int64_t notice_until_us; // 0: no notice. Else the bottom row shows it,
                           // and at this time the next track is started
} ui_play_t;

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

  // The settings as they were last handed to the state task to be saved.
  state_settings_t settings;

  // The key test screen.
  bool keytest_any; // an event has been shown
  key_event_t keytest_last;
  unsigned keytest_count;
  char keytest_opened_by; // the key whose release is not to be shown

  // About.
  uint32_t about_tracks;

  ui_play_t play;

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

// --- ui.c ---------------------------------------------------------------------

// The index, if there is one and it is the one the UI's numbers belong to;
// NULL otherwise. Whoever gets one calls library_task_put before returning.
const library_t *ui_index_get(const ui_t *ui);

// --- ui_play.c: the track and its list ---------------------------------------

// The user chose position `index` of `list`. Called with the index held
// (`lib` is not NULL): copies what the track needs out of it. Follow with
// ui_play_start once the index has been given back.
void ui_play_choose(ui_t *ui, const library_t *lib, tracklist_t list,
                    uint32_t index, int64_t now_us);
void ui_play_start(ui_t *ui);

// D, and 5 on Now Playing: pause, resume, or start the remembered track
// again from its beginning.
void ui_play_toggle(ui_t *ui, int64_t now_us);

// A player event: log it and, if it is about the current track, act on it.
void ui_play_on_event(ui_t *ui, const player_event_t *event, int64_t now_us);

// Once per pass: a notice whose time is up goes, and the next track starts.
void ui_play_on_time(ui_t *ui, int64_t now_us);

// Stop the player and stay stopped: the track is remembered, and its list
// does not go on. For a scan, and for the console's `stop`.
void ui_play_stop(ui_t *ui);

// A new index has been published: find the track in it again.
void ui_play_library_changed(ui_t *ui, int64_t now_us);

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
