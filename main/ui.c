#include "ui.h"

#include "debug_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "keys.h"
#include "keytest.h"
#include "lcd.h"
#include "library_task.h"
#include "pcm.h"
#include "pins.h"
#include "player.h"
#include "player_status.h"
#include "playlist_task.h"
#include "screen.h"
#include "state_task.h"
#include "ui_state.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// Design notes
//
// ONE TASK, ONE LOOP
//
// The UI task owns the LCD and is the only reader of five queues: key events
// (the keypad and the console), player events, card events (start-up), the
// library's status and the playlist task's answers. They are joined in a queue set, so the task sleeps
// on all of them at once. One pass of its loop is:
//
//   1. read the clock, once: it decides what is drawn and how long to sleep
//   2. act on a time that has come: a "Can't play" notice that has had its
//      1.5 s gives way to the next track (ui_play_on_time); a character
//      typed a second ago is committed (ui_text_on_time)
//   3. if the list on show has moved, copy its texts out of the index
//      (ui_fill_list: the one place a redraw takes the index, at most once)
//   4. draw the state into a screen_t (ui_render, in ui_screens.c) and send
//      the LCD only what differs from the screen it has
//   5. sleep until an event arrives or the next thing is due by itself
//      (ui_next_change_ms): a marquee step, the volume bar or a notice
//      running out. If nothing is due, sleep for ever
//   6. take every event that is waiting, in the order they were posted, and
//      apply each to the state; then go round. So sixteen key events that
//      arrived during one LCD transfer cost one redraw, not sixteen
//
// Nothing is polled. A pass happens because an event arrived or because the
// time step 5 slept for has run out; in the second case there is always
// something to draw or to do. The loop allocates nothing; all its state is
// one static ui_t.
//
// The work is in six files. This one has the loop, the keys and the
// menus. ui_options.c has the options menu. ui_play.c has the track that is
// playing and the queue it belongs to: what the player's events do, and
// what is saved for the next power-on. ui_playlists.c has the playlist
// screens, the picker, the delete confirmation and the playlist task's
// answers; ui_text.c the text entry. ui_screens.c draws. State changes and
// drawing are kept apart: an event handler only changes ui_t, and ui_render
// is a function of ui_t, the player's status and the time. "Draw only what
// changed" then needs no thought: whatever the handlers did, one screen is
// built and compared with the last one sent.
//
// THE SCREENS
//
// ui_t.mode says which screen is up:
//
//   MENU         the menu tree: a list (the main menu, Artists, an artist's
//                albums, Albums, an album's or an artist's tracks, Songs,
//                Playlists, a playlist's entries, Settings). Its state is
//                `browse` (numbers only) and the copy of the current list's
//                texts (list_*). A playlist screen still waiting for its
//                listing or its entries says "Loading" (list_loading).
//                While start-up waits for a card, "Insert SD card" covers
//                the list; while the library is scanned, "Scanning..." and
//                the count do. These are not modes: they come and go with
//                what the card and the library report, and the list keys do
//                nothing while one is up.
//   NOW_PLAYING  the current track's title and artist, the play or pause
//                symbol, the time and the shuffle and loop marks; "Nothing
//                playing" if there is no track. Its state is ui_t.play and
//                ui_t.queue, which are kept up whatever screen is up.
//                Choosing a track in a list comes here, and so does
//                power-on if a track was remembered.
//   KEYTEST      the key test screen: keytest_*.
//   ABOUT        two facts: about_tracks and the firmware's build date.
//
// LAYERS
//
// Four things cover the menus or Now Playing without being a mode (`layer`;
// at most one at a time): the options menu (`#`, ui_options.c), the picker
// ("Add to playlist") and the delete confirmation (ui_playlists.c), and the
// text entry (ui_text.c). The screen under a layer keeps its state, and is
// shown again as it was when the layer goes. A layer takes the keys that
// are not global (A, B, D, C):
//
//   options       2 / 8 move; 5 or 6 does the action; 4, `*` or `#` close
//   picker        2 / 8 move; 5 or 6 chooses; 4, `*` or `#` close
//   confirmation  2 / 8 move between No and Yes; 5 or 6 chooses; 4 or `*`
//                 cancel; `#` does nothing
//   text entry    presses of 0-9 type, `*` deletes, `#` saves (ui_text.c);
//                 4 is a letter key here
//
// C over the text entry cancels it, back to the screen it was opened on, and
// does nothing else. C over any other layer closes it and then does what C
// does (Now Playing / the menus), so a layer never comes back by itself. A
// scan beginning or a new index closes every layer: their numbers belong to
// the old index (THE INDEX).
//
// OVERLAYS
//
// Two things take over a bottom row for 1.5 s, and neither is a screen. The
// overlay replaces the bottom row of whatever is up (except the key test),
// layers included, until overlay_until_us: the volume bar, or the words that
// confirm a key ("Shuffle on", "Loop all", "Added", "Queue full") or give a
// playlist request's answer ("Saved", "Name in use", "Busy", ...); the
// newest replaces the one before. A notice ("Can't play", "File missing") replaces Now
// Playing's bottom row until play.notice_until_us, and when it goes the
// next track is started; the overlay is drawn over it. Each is a time in
// ui_t: ui_render compares it with the clock, and ui_next_change_ms makes
// it one of the moments the loop sleeps until, along with the marquee's
// next step. There is one sleep and no timer.
//
// KEYS
//
// On the key test screen every event is shown and none acted on, except a
// long press of `*`, which leaves. Everywhere else a key goes first to the
// global keys and, if it is not one of them, to the screen that is up:
//
//   A / B   volume up / down, on press and on repeat; shows the bar for
//           1.5 s, also when the volume was already at its end. A new level
//           is handed to the state task, which saves it (state_task.h)
//   D       pause if playing, resume if paused; if nothing is loaded and a
//           track is remembered (the queue ended, a scan stopped it, or it
//           is the one from before power-off), play that track from the
//           time it shows; otherwise nothing
//   C       Now Playing if the menus (or About) are up; the menus if Now
//           Playing is, where the user left them (and see LAYERS)
//
//   a list  2 / 8 up / down on press and on repeat (a press goes round at
//           the ends, a repeat stops there); 5 or 6 opens or selects; 4 or
//           `*` goes back; 1 / 3 jump to the previous / next first letter
//           where the list is A to Z, and move by 10 where it is not; `#`
//           opens the options menu if the row is a track, an album, an
//           artist, a playlist or an entry
//   a layer see LAYERS
//   Now Playing   5 does what D does; `*` goes to the menus; 1 turns
//           shuffle on or off; 3 takes loop from off to all to one to off;
//           6 is next and 4 previous, ON RELEASE (below); `#` opens the
//           options menu for the current track and the queue
//   About   any key that is not a global one leaves it, to Settings
//
// Only presses act (and repeats, where it says so), with one exception. A
// release does nothing, so the release of the key that opened a screen
// cannot also close it.
//
// The exception: on Now Playing, 4 and 6 act when the key is let go, and
// only if it was not held long enough to be a long press (holding them is
// kept for seeking). skip_key is the whole of it: a press of 4 or 6 on Now
// Playing sets it to that key; a long press or a repeat clears it; so does
// the press of any key at all, before it is acted on; and a release skips
// only if it is of the key skip_key names and Now Playing is still up. So
// the 6 that chose a track in a list, and is let go on Now Playing, skips
// nothing: its press was not made there.
//
// THE INDEX
//
// The library's index is taken (library_task_get) and given back
// (library_task_put) inside one function each time, never across a wait:
//
//   - a key that needs it takes it once: select (browse_select, the queue
//     filled from the list, and the path, title and artist of the track to
//     play), a letter jump (one take around the whole binary search),
//     opening About (the track count), `#` (what is under the cursor and
//     its name), an action of the options menu (which is also where the
//     picker's tracks are gathered), next and previous. The picker, the
//     confirmation and the text entry need no index.
//     Moving the cursor and going back need no index at all: a list's
//     length is already in its menu_t. Nor do pause, shuffle and loop.
//   - a redraw takes it once, and only if the list on show has moved since
//     its texts were copied: ui_fill_list counts the list afresh and copies
//     the list's name and the item under the cursor.
//   - a track that ends takes it once, for the track the queue says is
//     next; a new index takes it once, to restore or check the queue and to
//     find the current track in it again; a playlist's entries that arrive
//     to be played take it once, to load the queue.
//
// What is kept between passes is copies and numbers: list_name, list_item,
// list_menu, options_title, and play.path and play.title. A marquee step, a
// clock tick, an overlay or a notice draws from those and never touches the
// index.
//
// The numbers in `browse` and in the options menu belong to one index.
// ui_index_get therefore hands out an index only when the UI has caught up
// with the library's latest status: not while a scan is running, and not
// when a newer index has been published than the one `generation` names.
// When the status with the new generation is read, the menus go to the main
// menu (browse_library_changed), every layer closes, the open playlist and
// any tracks held for a playlist are dropped (ui_pl_index_changed), and only
// then is the new index used. A scan beginning does the same but for the
// menus, which the scan screen covers until the new index comes. The queue's numbers are tied to an index by its
// checksum instead, because a queue outlives a scan that finds the same
// tracks (ui_play.c, A NEW INDEX).
//
// With no index (a scan that failed, or before the first load) the library's
// lists are empty: the list's name and "(empty)".
//
// MEMORY
//
// The queue's two orders, 8,192 track numbers each, are one block of 32 KB
// of PSRAM, allocated by ui_start and never freed. (Internal RAM has no
// room for it: about 38 KB is free while a track plays.) So is the
// playlists' side (ui_pl_t, about 24 KB: ui_playlists.c, MEMORY); ui_t holds
// a pointer to it. If either cannot be had the LCD says so and ui_start
// fails: the player does not run without them.

#define KEY_QUEUE_LENGTH 16
#define PLAYER_EVENT_QUEUE_LENGTH 16
#define CARD_EVENT_QUEUE_LENGTH 2
#define LIBRARY_STATUS_QUEUE_LENGTH 1 // the latest status, overwritten
#define UI_TASK_STACK 8192
#define UI_TASK_PRIORITY 5
#define UI_TASK_CORE 0
#define UI_REPORT_WAIT_MS 1000 // how long the console waits for an answer

// A held key posts a repeat every 50 ms, and a line of log costs the UI task
// about 2.6 ms of UART. So repeats are counted and logged as one line,
// `KEY repeat <char> x<count>`, when the next other key event arrives (the
// release, as a rule). Set to 1 to log each repeat as it is taken.
#define UI_LOG_EVERY_REPEAT 0

static const char *TAG = "dap";

static QueueHandle_t s_key_events;
static QueueHandle_t s_player_events;
static QueueHandle_t s_card_events;
static QueueHandle_t s_library_status;
static QueueHandle_t s_playlist_events;
static QueueSetHandle_t s_ui_events; // all of the above

static TaskHandle_t s_task;
static atomic_uint s_passes;
static atomic_uint s_lcd_updates;
static atomic_bool s_stop_wanted; // ui_stop was called
static uint16_t *s_queue_orders;  // the queue's two arrays: see MEMORY
static ui_pl_t *s_pl;             // the playlists' side: see MEMORY

// The console's `queue` (ui_queue_report). What it asked for, and the answer:
// the UI task fills s_report inside s_report_lock and gives s_report_done.
static atomic_bool s_report_wanted;
static atomic_uint s_report_from;
static atomic_uint s_report_count;
static SemaphoreHandle_t s_report_done;
static portMUX_TYPE s_report_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_queue_report_t s_report;

// What the task that waits for the card tells the UI task (ui_card_waiting).
// The UI task owns the LCD, so the task that waits for the card cannot draw
// the "insert card" screen itself; it sends one of these instead.
typedef enum {
  CARD_EVENT_WAITING, // the card did not mount; the mount is being retried
  CARD_EVENT_MOUNTED, // the wait is over
} card_event_t;

// --- the index ---------------------------------------------------------------

// The index, if there is one and it is the one the UI's numbers belong to
// (see THE INDEX). NULL otherwise. Whoever gets one calls library_task_put
// before it returns.
const library_t *ui_index_get(const ui_t *ui) {
  // A status still in the queue is newer than what `ui` holds. Looking at it
  // does not take it: it is handled in its turn, later in this same pass.
  library_status_t latest;
  if (xQueuePeek(s_library_status, &latest, 0) == pdTRUE) {
    if (latest.scanning || latest.generation != ui->generation) {
      return NULL;
    }
  } else if (ui->scanning) {
    return NULL;
  }
  return library_task_get();
}

// --- the menus ---------------------------------------------------------------

// Bring the copy of the current list up to date: its length, its name and
// the item under the cursor. The one place a redraw takes the index.
static void ui_fill_list(ui_t *ui, int64_t now_us) {
  char item[UI_TEXT_MAX];
  const library_t *lib = ui_index_get(ui);
  browse_refresh(&ui->browse, lib);
  const menu_t *menu = browse_menu(&ui->browse);
  browse_name(&ui->browse, lib, ui->list_name, sizeof(ui->list_name));
  browse_item(&ui->browse, lib, menu->cursor, item, sizeof(item));
  if (lib != NULL) {
    library_task_put(); // only copies and numbers from here
  }
  ui->list_menu = *menu;
  ui->list_loading = ui_pl_loading(ui);
  // Different words on the row, or the same words at another place in the
  // list: they are read from the beginning.
  if (ui->list_restart || strcmp(item, ui->list_item) != 0) {
    strcpy(ui->list_item, item);
    ui->item_since_us = now_us;
  }
  ui->list_dirty = false;
  ui->list_restart = false;
}

typedef struct {
  const browse_t *browse;
  const library_t *lib;
} group_ctx_t;

static char group_at(void *ctx, uint32_t index) {
  const group_ctx_t *c = ctx;
  return browse_group(c->browse, c->lib, index);
}

// 1 and 3: to the previous or next first letter in a list that is A to Z,
// by ten items in one that is not.
static void ui_jump(ui_t *ui, int direction) {
  menu_t *menu = browse_menu(&ui->browse);
  if (!browse_is_alphabetical(&ui->browse)) {
    menu_move(menu, 10 * direction, false);
    return;
  }
  // The search asks for a dozen items' groups; the index is taken once
  // around all of them. With no index the list is empty and stays put.
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    return;
  }
  group_ctx_t ctx = {.browse = &ui->browse, .lib = lib};
  menu_jump_group(menu, direction, group_at, &ctx);
  library_task_put();
}

void ui_show_now_playing(ui_t *ui, int64_t now_us) {
  ui->mode = UI_MODE_NOW_PLAYING;
  ui->play.title_since_us = now_us; // the title is read from its beginning
}

static void ui_show_menus(ui_t *ui) {
  ui->mode = UI_MODE_MENU;
  ui->list_dirty = true;
  ui->list_restart = true;
}

// 5 or 6 in a list: open or act on the item under the cursor. The index is
// taken once, for browse_select and for whatever the action needs from it.
static void ui_select(ui_t *ui, char key, int64_t now_us) {
  const library_t *lib = ui_index_get(ui); // NULL is an empty library
  browse_result_t result = browse_select(&ui->browse, lib);
  if (result.action == BROWSE_PLAY) {
    // The list on the screen becomes the queue, from the row selected.
    if (!ui_play_load(ui, lib, result.list, result.index, false, now_us)) {
      result.action = BROWSE_NOTHING;
    }
  } else if (result.action == BROWSE_PLAY_ENTRIES) {
    // The open playlist's playable entries become the queue, from this one.
    if (!ui_pl_play_entries(ui, lib, result.index, now_us)) {
      result.action = BROWSE_NOTHING;
    }
  } else if (result.action == BROWSE_ABOUT) {
    ui->about_tracks = library_track_count(lib);
  }
  if (lib != NULL) {
    library_task_put();
  }

  switch (result.action) {
  case BROWSE_NOTHING:
  case BROWSE_ENTERED: // ui_on_list_key sees that the level changed
    break;
  case BROWSE_PLAY:
  case BROWSE_PLAY_ENTRIES:
    ui_play_start(ui);
    ui_show_now_playing(ui, now_us);
    break;
  case BROWSE_NOW_PLAYING:
    ui_show_now_playing(ui, now_us);
    break;
  case BROWSE_RESCAN:
    // Returns at once; the scanning screen comes with the library's status.
    // Refused (no card, a scan already running): nothing happens.
    if (library_task_rescan() != ESP_OK) {
      ESP_LOGW(TAG, "Rescan refused");
    }
    break;
  case BROWSE_KEYTEST:
    ui->mode = UI_MODE_KEYTEST;
    ui->keytest_any = false;
    ui->keytest_count = 0;
    ui->keytest_opened_by = key;
    ui->overlay_until_us = 0;
    break;
  case BROWSE_ABOUT:
    ui->mode = UI_MODE_ABOUT;
    break;
  case BROWSE_NEW_PLAYLIST:
    ui_text_open(ui, UI_TEXT_NEW, NULL, now_us);
    break;
  case BROWSE_OPEN_PLAYLIST:
    ui_pl_open_playlist(ui, result.index, now_us); // "Loading" until it comes
    break;
  case BROWSE_MISSING:
    ui_overlay_show(ui, UI_OVERLAY_FILE_MISSING, now_us);
    break;
  }
}

static void ui_on_list_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  bool press = (event->type == KEY_PRESS);
  bool step = press || event->type == KEY_REPEAT;
  menu_t *menu = browse_menu(&ui->browse);
  int depth = ui->browse.depth;
  uint32_t cursor = menu->cursor;

  switch (event->key) {
  case '2':
  case '8':
    if (step) {
      // A press goes round at the ends; a held key stops there.
      menu_move(menu, event->key == '8' ? 1 : -1, press);
    }
    break;
  case '1':
  case '3':
    if (press) {
      ui_jump(ui, event->key == '3' ? 1 : -1);
    }
    break;
  case '4':
  case '*':
    if (press) {
      browse_back(&ui->browse);
    }
    break;
  case '5':
  case '6':
    if (press && !ui_pl_retry_listing(ui, now_us)) {
      ui_select(ui, event->key, now_us);
    }
    break;
  case '#':
    if (press) {
      ui_options_open(ui);
    }
    break;
  default:
    break;
  }

  if (ui->browse.depth != depth || browse_menu(&ui->browse)->cursor != cursor) {
    ui->list_dirty = true;
    ui->list_restart = true;
  }
  ui_pl_after_list_key(ui, depth, now_us);
}

void ui_close_layer(ui_t *ui) {
  switch (ui->layer) {
  case UI_LAYER_TEXT:
    ui_text_cancel(ui);
    break;
  case UI_LAYER_OPTIONS:
    ui_options_close(ui);
    break;
  case UI_LAYER_PICKER:
  case UI_LAYER_CONFIRM:
    ui->layer = UI_LAYER_NONE;
    ui->list_dirty = true;
    ui->list_restart = true;
    break;
  case UI_LAYER_NONE:
    break;
  }
}

// A key while a layer is up. C has been dealt with (ui_on_key).
static void ui_on_layer_key(ui_t *ui, const key_event_t *event,
                            int64_t now_us) {
  switch (ui->layer) {
  case UI_LAYER_OPTIONS:
    ui_options_on_key(ui, event, now_us);
    break;
  case UI_LAYER_PICKER:
    ui_pl_picker_key(ui, event, now_us);
    break;
  case UI_LAYER_CONFIRM:
    ui_pl_confirm_key(ui, event, now_us);
    break;
  case UI_LAYER_TEXT:
    ui_text_on_key(ui, event, now_us);
    break;
  case UI_LAYER_NONE:
    break;
  }
}

// --- keys --------------------------------------------------------------------

static void flush_repeats(ui_t *ui) {
  if (ui->repeat_count != 0) {
    ESP_LOGI(TAG, "KEY repeat %c x%" PRIu32, ui->repeat_key, ui->repeat_count);
    ui->repeat_count = 0;
  }
}

// Every key event is logged as it is taken off the queue, so that a script
// can see what the UI task received and in what order; repeats as a count
// (see UI_LOG_EVERY_REPEAT).
static void log_key(ui_t *ui, const key_event_t *event) {
  if (!UI_LOG_EVERY_REPEAT && event->type == KEY_REPEAT) {
    if (ui->repeat_key != event->key) {
      flush_repeats(ui);
      ui->repeat_key = event->key;
    }
    ui->repeat_count++;
    return;
  }
  flush_repeats(ui);
  ESP_LOGI(TAG, "KEY %s %c", keytest_event_name(event->type), event->key);
}

static void ui_on_keytest_key(ui_t *ui, const key_event_t *event) {
  if (event->key == '*' && event->type == KEY_LONG) {
    ui_show_menus(ui);
    return;
  }
  // The release of the key that opened the screen is the end of that press,
  // not something the user did here.
  char opened_by = ui->keytest_opened_by;
  ui->keytest_opened_by = '\0';
  if (event->type == KEY_RELEASE && event->key == opened_by) {
    return;
  }
  ui->keytest_any = true;
  ui->keytest_last = *event;
  ui->keytest_count++;
}

// See KEYS for 4 and 6.
static void ui_on_now_playing_key(ui_t *ui, const key_event_t *event,
                                  int64_t now_us) {
  char key = event->key;
  if (key == '4' || key == '6') {
    switch (event->type) {
    case KEY_PRESS:
      ui->skip_key = key;
      break;
    case KEY_LONG:
    case KEY_REPEAT:
      ui->skip_key = '\0'; // held: not a skip
      break;
    case KEY_RELEASE:
      if (ui->skip_key == key) {
        ui->skip_key = '\0';
        if (key == '6') {
          ui_play_next(ui, now_us);
        } else {
          ui_play_previous(ui, now_us);
        }
      }
      break;
    }
    return;
  }
  if (event->type != KEY_PRESS) {
    return;
  }
  switch (key) {
  case '*':
    ui_show_menus(ui);
    break;
  case '5':
    ui_play_toggle(ui, now_us);
    break;
  case '1':
    ui_play_toggle_shuffle(ui, now_us);
    break;
  case '3':
    ui_play_cycle_loop(ui, now_us);
    break;
  case '#':
    ui_options_open_now_playing(ui);
    break;
  default:
    break;
  }
}

static void ui_on_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  if (event->key == '\0') {
    // The console's "look again": not a key. The pass it started redraws.
    return;
  }
  log_key(ui, event);
  if (event->type == KEY_PRESS) {
    ui->skip_key = '\0'; // a new press: whatever was held before is over
  }
  if (ui->mode == UI_MODE_KEYTEST) {
    ui_on_keytest_key(ui, event);
    return;
  }

  bool press = (event->type == KEY_PRESS);
  bool step = press || event->type == KEY_REPEAT;
  player_status_t status;

  switch (event->key) {
  case 'A':
  case 'B':
    if (step) {
      player_get_status(&status);
      int level = status.volume + (event->key == 'A' ? 1 : -1);
      if (level >= 0 && level <= PCM_VOLUME_MAX) {
        player_set_volume(level);
        // Returns at once; a held key is saved once, when it has settled.
        ui->settings.volume = level;
        state_task_save_settings(&ui->settings);
      }
      ui_overlay_show(ui, UI_OVERLAY_VOLUME, now_us);
    }
    return;
  case 'D':
    if (press) {
      ui_play_toggle(ui, now_us);
    }
    return;
  case 'C':
    if (press) {
      if (ui->layer == UI_LAYER_TEXT) {
        ui_text_cancel(ui); // back to where it was opened (see LAYERS)
        return;
      }
      ui_close_layer(ui);
      if (ui->mode == UI_MODE_NOW_PLAYING) {
        ui_show_menus(ui);
      } else {
        ui_show_now_playing(ui, now_us);
      }
    }
    return;
  default:
    break;
  }

  switch (ui->mode) {
  case UI_MODE_MENU:
    if (!ui_list_on_show(ui)) {
      break;
    }
    if (ui->layer != UI_LAYER_NONE) {
      ui_on_layer_key(ui, event, now_us);
    } else {
      ui_on_list_key(ui, event, now_us);
    }
    break;
  case UI_MODE_NOW_PLAYING:
    if (ui->layer != UI_LAYER_NONE) {
      ui_on_layer_key(ui, event, now_us);
    } else {
      ui_on_now_playing_key(ui, event, now_us);
    }
    break;
  case UI_MODE_ABOUT:
    if (press) {
      ui_show_menus(ui); // Settings, where About was opened
    }
    break;
  case UI_MODE_KEYTEST:
    break;
  }
}

// --- the player, the card and the library --------------------------------------

static void ui_on_library_status(ui_t *ui, const library_status_t *status,
                                 int64_t now_us) {
  bool scan_began = status->scanning && !ui->scanning;
  ui->scanning = status->scanning;
  ui->scan_tracks = status->tracks;
  if (scan_began) {
    // The scan wants the card to itself. The library task stops the
    // player, but it cannot stop a track this task is about to start.
    ui_play_stop(ui);
    // The scan is the thing to look at, whatever was up. (The key test is
    // left only by its own key.)
    if (ui->mode != UI_MODE_KEYTEST) {
      ui->mode = UI_MODE_MENU;
    }
    // The options menu's numbers are about to mean nothing, and so are the
    // tracks a picker or a text entry holds.
    ui_pl_index_changed(ui, now_us);
  }
  if (!status->scanning && status->generation != ui->generation) {
    // A new index (the first load, or a scan that has finished). The numbers
    // in the menus belonged to the old one: back to the main menu. The queue
    // is restored (the first time), kept or emptied, and the track on Now
    // Playing looked up again: ui_play.c. A track restored at power-on waits
    // on Now Playing.
    ui->generation = status->generation;
    browse_library_changed(&ui->browse);
    ui_pl_index_changed(ui, now_us); // layers close; the open playlist too
    ui->list_restart = true;
    if (ui_play_library_changed(ui, now_us) && ui->mode != UI_MODE_KEYTEST) {
      ui_show_now_playing(ui, now_us);
    }
  }
  // With or without a new index, what the lists hold may have changed.
  ui->list_dirty = true;
}

// Take one event from the queue that has one and apply it to the state.
static void ui_take(ui_t *ui, QueueSetMemberHandle_t ready, int64_t now_us) {
  if (ready == s_key_events) {
    key_event_t event;
    if (xQueueReceive(s_key_events, &event, 0) == pdTRUE) {
      ui_on_key(ui, &event, now_us);
    }
  } else if (ready == s_player_events) {
    player_event_t event;
    if (xQueueReceive(s_player_events, &event, 0) == pdTRUE) {
      ui_play_on_event(ui, &event, now_us);
    }
  } else if (ready == s_card_events) {
    card_event_t event;
    if (xQueueReceive(s_card_events, &event, 0) == pdTRUE) {
      ui->no_card = (event == CARD_EVENT_WAITING);
      ui->list_dirty = true;
    }
  } else if (ready == s_library_status) {
    library_status_t status;
    if (xQueueReceive(s_library_status, &status, 0) == pdTRUE) {
      ui_on_library_status(ui, &status, now_us);
    }
  } else if (ready == s_playlist_events) {
    playlist_event_t event;
    if (xQueueReceive(s_playlist_events, &event, 0) == pdTRUE) {
      ui_pl_on_event(ui, &event, now_us);
    }
  }
}

// The console asked what the queue holds (ui_queue_report). Numbers only:
// the console looks the paths up itself.
static void ui_answer_report(const ui_t *ui) {
  const play_queue_t *q = &ui->queue;
  ui_queue_report_t report = {
      .count = q->count,
      .position = q->count != 0 ? q->position : 0,
      .shuffle = q->shuffle,
      .loop = q->loop,
      .index = ui->queue_index,
      .from = atomic_load(&s_report_from),
  };
  if (report.from == UINT32_MAX) {
    report.from = report.position;
  }
  if (report.from > q->count) {
    report.from = q->count;
  }
  uint32_t n = atomic_load(&s_report_count);
  if (n > UI_QUEUE_REPORT_MAX) {
    n = UI_QUEUE_REPORT_MAX;
  }
  if (n > q->count - report.from) {
    n = q->count - report.from;
  }
  report.listed = n;
  memcpy(report.tracks, q->play + report.from, n * sizeof(uint16_t));
  portENTER_CRITICAL(&s_report_lock);
  s_report = report;
  portEXIT_CRITICAL(&s_report_lock);
  xSemaphoreGive(s_report_done);
}

// --- the task ------------------------------------------------------------------

// How long to sleep for a change that is `ms` away. A wait of n ticks ends
// at the n-th tick from now, which is anything from n - 1 to n tick periods
// away, so round up and add one: waking early would find nothing to draw and
// have to go round again.
static TickType_t ticks_until(uint32_t ms) {
  if (ms == UINT32_MAX) {
    return portMAX_DELAY;
  }
  return (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

// See ONE TASK, ONE LOOP.
static void ui_task(void *arg) {
  (void)arg;
  static ui_t ui; // too big for the stack
  browse_init(&ui.browse);
  ui.list_dirty = true;
  state_task_loaded_settings(&ui.settings);
  ui_play_init(&ui, s_queue_orders);
  ui_pl_init(&ui, s_pl);
  screen_t shown;        // what the LCD was last told to show
  bool shown_ok = false; // and it took it

  for (;;) {
    atomic_fetch_add(&s_passes, 1);
    int64_t now_us = esp_timer_get_time();
    ui_play_on_time(&ui, now_us);
    ui_text_on_time(&ui, now_us);
    if (ui.list_dirty && ui_list_on_show(&ui)) {
      ui_fill_list(&ui, now_us);
    }
    player_status_t player;
    player_get_status(&player);
    screen_t wanted;
    ui_render(&ui, &player, now_us, &wanted);
    // Most events change nothing that is on the screen (a key release, a
    // clock tick inside the same second). Comparing two screens costs
    // nothing next to an I2C transfer. A transfer that failed is tried
    // again whether or not anything changed; with no LCD at all that is a
    // call that returns at once.
    if (!shown_ok || memcmp(&wanted, &shown, sizeof(shown)) != 0) {
      shown = wanted;
      shown_ok = (lcd_show(&shown) == ESP_OK);
      debug_console_note_screen(&shown);
      if (shown_ok) {
        atomic_fetch_add(&s_lcd_updates, 1);
      }
    }

    QueueSetMemberHandle_t ready = xQueueSelectFromSet(
        s_ui_events, ticks_until(ui_next_change_ms(&ui, now_us)));
    // Everything that is waiting, in the order it was posted, before the
    // screen is drawn once. If nothing was (the wait ran out), something is
    // due by itself and the next pass does it.
    now_us = esp_timer_get_time();
    while (ready != NULL) {
      ui_take(&ui, ready, now_us);
      ready = xQueueSelectFromSet(s_ui_events, 0);
    }
    if (atomic_exchange(&s_stop_wanted, false)) {
      ui_play_stop(&ui);
    }
    if (atomic_exchange(&s_report_wanted, false)) {
      ui_answer_report(&ui);
    }
  }
}

esp_err_t ui_start(ui_queues_t *queues) {
  // The LCD is set up here, before the UI task exists; from then on only
  // that task touches it.
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err == ESP_OK) {
    ui_screens_define_glyphs();
  } else {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
  }

  // One-time start-up allocations. The queue's arrays are in PSRAM: see
  // MEMORY.
  s_key_events = xQueueCreate(KEY_QUEUE_LENGTH, sizeof(key_event_t));
  s_player_events =
      xQueueCreate(PLAYER_EVENT_QUEUE_LENGTH, sizeof(player_event_t));
  s_card_events = xQueueCreate(CARD_EVENT_QUEUE_LENGTH, sizeof(card_event_t));
  s_library_status =
      xQueueCreate(LIBRARY_STATUS_QUEUE_LENGTH, sizeof(library_status_t));
  s_playlist_events =
      xQueueCreate(PLAYLIST_EVENT_QUEUE_LENGTH, sizeof(playlist_event_t));
  // A set must be as long as all its members together.
  s_ui_events = xQueueCreateSet(KEY_QUEUE_LENGTH + PLAYER_EVENT_QUEUE_LENGTH +
                                CARD_EVENT_QUEUE_LENGTH +
                                LIBRARY_STATUS_QUEUE_LENGTH +
                                PLAYLIST_EVENT_QUEUE_LENGTH);
  s_report_done = xSemaphoreCreateBinary();
  s_queue_orders =
      heap_caps_malloc(2 * UI_QUEUE_CAPACITY * sizeof(uint16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_pl = heap_caps_calloc(1, sizeof(ui_pl_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bool have = s_key_events && s_player_events && s_card_events &&
              s_library_status && s_playlist_events && s_ui_events &&
              s_report_done && s_queue_orders && s_pl;
  if (have) {
    ESP_LOGI(TAG, "UI memory: queue %u bytes, playlists %u bytes (PSRAM)",
             (unsigned)(2 * UI_QUEUE_CAPACITY * sizeof(uint16_t)),
             (unsigned)sizeof(ui_pl_t));
    // Queues must join the set while still empty, so before anything posts.
    xQueueAddToSet(s_key_events, s_ui_events);
    xQueueAddToSet(s_player_events, s_ui_events);
    xQueueAddToSet(s_card_events, s_ui_events);
    xQueueAddToSet(s_library_status, s_ui_events);
    xQueueAddToSet(s_playlist_events, s_ui_events);
    have = xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL,
                                   UI_TASK_PRIORITY, &s_task,
                                   UI_TASK_CORE) == pdPASS;
  }
  if (!have) {
    // Nothing can run without these. Say so where the user can see it; the
    // caller stops there, and nothing restarts.
    ESP_LOGE(TAG,
             "no memory for the user interface (free: %u internal, %u PSRAM)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    screen_t screen;
    screen_clear(&screen);
    screen_set_row(&screen, 0, "Out of memory");
    lcd_show(&screen);
    s_task = NULL;
    return ESP_ERR_NO_MEM;
  }

  queues->keys = s_key_events;
  queues->player_events = s_player_events;
  queues->library_status = s_library_status;
  queues->playlist_events = s_playlist_events;
  return ESP_OK;
}

void ui_card_waiting(bool waiting) {
  // At most two card events are ever sent, and the queue holds two, so
  // neither send can block or be lost.
  card_event_t event = waiting ? CARD_EVENT_WAITING : CARD_EVENT_MOUNTED;
  xQueueSend(s_card_events, &event, portMAX_DELAY);
}

void ui_get_stats(ui_stats_t *out) {
  out->stack_free = s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
  out->passes = atomic_load(&s_passes);
  out->lcd_updates = atomic_load(&s_lcd_updates);
}

void ui_stop(void) {
  atomic_store(&s_stop_wanted, true);
  // Wake the task: a key event with no key is one it takes and ignores. If
  // the queue is full the task is awake already and will see the flag.
  const key_event_t nothing = {.type = KEY_RELEASE, .key = '\0'};
  xQueueSend(s_key_events, &nothing, 0);
}

bool ui_queue_report(uint32_t from, uint32_t count, ui_queue_report_t *out) {
  if (s_task == NULL) {
    return false;
  }
  xSemaphoreTake(s_report_done, 0); // an answer nobody waited for, if any
  atomic_store(&s_report_from, from);
  atomic_store(&s_report_count, count);
  atomic_store(&s_report_wanted, true);
  // Wake the task, as ui_stop does.
  const key_event_t nothing = {.type = KEY_RELEASE, .key = '\0'};
  xQueueSend(s_key_events, &nothing, 0);
  if (xSemaphoreTake(s_report_done, pdMS_TO_TICKS(UI_REPORT_WAIT_MS)) !=
      pdTRUE) {
    return false;
  }
  portENTER_CRITICAL(&s_report_lock);
  *out = s_report;
  portEXIT_CRITICAL(&s_report_lock);
  return true;
}
