#include "ui.h"

#include "debug_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "keys.h"
#include "keytest.h"
#include "lcd.h"
#include "library_task.h"
#include "pcm.h"
#include "pins.h"
#include "player.h"
#include "player_status.h"
#include "screen.h"
#include "ui_state.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// Design notes
//
// ONE TASK, ONE LOOP
//
// The UI task owns the LCD and is the only reader of four queues: key events
// (the keypad and the console), player events, card events (start-up) and
// the library's status. They are joined in a queue set, so the task sleeps
// on all of them at once. One pass of its loop is:
//
//   1. read the clock, once: it decides what is drawn and how long to sleep
//   2. act on a time that has come: a "Can't play" notice that has had its
//      1.5 s gives way to the next track (ui_play_on_time)
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
// The work is in three files. This one has the loop, the keys and the
// menus. ui_play.c has the track that is playing and the list it came from:
// what the player's events do. ui_screens.c draws. State changes and
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
//                Settings). Its state is `browse` (numbers only) and the
//                copy of the current list's texts (list_*).
//                While start-up waits for a card, "Insert SD card" covers
//                the list; while the library is scanned, "Scanning..." and
//                the count do. These are not modes: they come and go with
//                what the card and the library report, and the list keys do
//                nothing while one is up.
//   NOW_PLAYING  the current track's title and artist, the play or pause
//                symbol and the time; "Nothing playing" if there is no
//                track. Its state is ui_t.play, which is kept up whatever
//                screen is up. Choosing a track in a list comes here.
//   KEYTEST      the key test screen: keytest_*.
//   ABOUT        two facts: about_tracks and the firmware's build date.
//
// OVERLAYS
//
// Two things take over a bottom row for 1.5 s, and neither is a screen. The
// volume bar replaces the bottom row of whatever is up (except the key
// test) until volume_until_us. A notice ("Can't play", "File missing")
// replaces Now Playing's bottom row until play.notice_until_us, and when it
// goes the next track is started; the bar, the newer of the two, is drawn
// over it. Each is a time in ui_t: ui_render compares it with the clock,
// and ui_next_change_ms makes it one of the moments the loop sleeps until,
// along with the marquee's next step. There is one sleep and no timer.
//
// KEYS
//
// On the key test screen every event is shown and none acted on, except a
// long press of `*`, which leaves. Everywhere else a key goes first to the
// global keys and, if it is not one of them, to the screen that is up:
//
//   A / B   volume up / down, on press and on repeat; shows the bar for
//           1.5 s, also when the volume was already at its end
//   D       pause if playing, resume if paused; if nothing is loaded and a
//           track is remembered (its list ended, or a scan stopped it),
//           play that track again from its beginning; otherwise nothing
//   C       Now Playing if the menus (or About) are up; the menus if Now
//           Playing is, where the user left them
//   #       nothing yet
//
//   a list  2 / 8 up / down on press and on repeat (a press goes round at
//           the ends, a repeat stops there); 5 or 6 opens or selects; 4 or
//           `*` goes back; 1 / 3 jump to the previous / next first letter
//           where the list is A to Z, and move by 10 where it is not
//   Now Playing   5 does what D does; `*` goes to the menus; 1, 3, 4, 6
//           do nothing yet
//   About   any key that is not a global one leaves it, to Settings
//
// Only presses act (and repeats, where it says so). A release does nothing,
// so the release of the key that opened a screen cannot also close it.
//
// THE INDEX
//
// The library's index is taken (library_task_get) and given back
// (library_task_put) inside one function each time, never across a wait:
//
//   - a key that needs it takes it once: select (browse_select, and the
//     path, title and artist of a track to play), a letter jump (one take
//     around the whole binary search), opening About (the track count).
//     Moving the cursor and going back need no index at all: a list's
//     length is already in its menu_t.
//   - a redraw takes it once, and only if the list on show has moved since
//     its texts were copied: ui_fill_list counts the list afresh and copies
//     the list's name and the item under the cursor.
//   - a track that ends takes it once, for the next track of its list; a
//     new index takes it once, to find the current track in it again.
//
// What is kept between passes is copies and numbers: list_name, list_item,
// list_menu, and play.path and play.title. A marquee step, a clock tick,
// the volume bar or a notice draws from those and never touches the index.
//
// The numbers in `browse` belong to one index. ui_index_get therefore hands
// out an index only when the UI has caught up with the library's latest
// status: not while a scan is running, and not when a newer index has been
// published than the one `generation` names. When the status with the new
// generation is read, the menus go to the main menu (browse_library_changed)
// and only then is the new index used.
//
// With no index (a scan that failed, or before the first load) the library's
// lists are empty: the list's name and "(empty)".

#define KEY_QUEUE_LENGTH 16
#define PLAYER_EVENT_QUEUE_LENGTH 16
#define CARD_EVENT_QUEUE_LENGTH 2
#define LIBRARY_STATUS_QUEUE_LENGTH 1 // the latest status, overwritten
#define UI_TASK_STACK 8192
#define UI_TASK_PRIORITY 5
#define UI_TASK_CORE 0

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
static QueueSetHandle_t s_ui_events; // all of the above

static TaskHandle_t s_task;
static atomic_uint s_passes;
static atomic_uint s_lcd_updates;
static atomic_bool s_stop_wanted; // ui_stop was called

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

static void ui_show_now_playing(ui_t *ui, int64_t now_us) {
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
    ui_play_choose(ui, lib, result.list, result.index, now_us);
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
    ui->volume_until_us = 0;
    break;
  case BROWSE_ABOUT:
    ui->mode = UI_MODE_ABOUT;
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
    if (press) {
      ui_select(ui, event->key, now_us);
    }
    break;
  default:
    break;
  }

  if (ui->browse.depth != depth || browse_menu(&ui->browse)->cursor != cursor) {
    ui->list_dirty = true;
    ui->list_restart = true;
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

static void ui_on_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  if (event->key == '\0') {
    // The console's "look again": not a key. The pass it started redraws.
    return;
  }
  log_key(ui, event);
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
      }
      ui->volume_until_us = now_us + UI_OVERLAY_MS * 1000;
    }
    return;
  case 'D':
    if (press) {
      ui_play_toggle(ui, now_us);
    }
    return;
  case 'C':
    if (press) {
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
    if (ui_list_on_show(ui)) {
      ui_on_list_key(ui, event, now_us);
    }
    break;
  case UI_MODE_NOW_PLAYING:
    // 1, 3, 4, 6 and # are stage 6's.
    if (press && event->key == '*') {
      ui_show_menus(ui);
    } else if (press && event->key == '5') {
      ui_play_toggle(ui, now_us);
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
  }
  if (!status->scanning && status->generation != ui->generation) {
    // A new index (the first load, or a scan that has finished). The numbers
    // in the menus belonged to the old one: back to the main menu. The track
    // on Now Playing may have started while there was no index, its tags may
    // have changed, or it may be gone: look it up again.
    ui->generation = status->generation;
    browse_library_changed(&ui->browse);
    ui->list_restart = true;
    ui_play_library_changed(ui, now_us);
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
  }
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
  screen_t shown;        // what the LCD was last told to show
  bool shown_ok = false; // and it took it

  for (;;) {
    atomic_fetch_add(&s_passes, 1);
    int64_t now_us = esp_timer_get_time();
    ui_play_on_time(&ui, now_us);
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
  }
}

esp_err_t ui_start(ui_queues_t *queues) {
  // One-time start-up allocations.
  s_key_events = xQueueCreate(KEY_QUEUE_LENGTH, sizeof(key_event_t));
  s_player_events =
      xQueueCreate(PLAYER_EVENT_QUEUE_LENGTH, sizeof(player_event_t));
  s_card_events = xQueueCreate(CARD_EVENT_QUEUE_LENGTH, sizeof(card_event_t));
  s_library_status =
      xQueueCreate(LIBRARY_STATUS_QUEUE_LENGTH, sizeof(library_status_t));
  s_ui_events = xQueueCreateSet(KEY_QUEUE_LENGTH + PLAYER_EVENT_QUEUE_LENGTH +
                                CARD_EVENT_QUEUE_LENGTH +
                                LIBRARY_STATUS_QUEUE_LENGTH);
  if (!s_key_events || !s_player_events || !s_card_events ||
      !s_library_status || !s_ui_events) {
    return ESP_ERR_NO_MEM;
  }
  // Queues must join the set while still empty, so before anything posts.
  xQueueAddToSet(s_key_events, s_ui_events);
  xQueueAddToSet(s_player_events, s_ui_events);
  xQueueAddToSet(s_card_events, s_ui_events);
  xQueueAddToSet(s_library_status, s_ui_events);

  // The LCD is set up here, before the UI task exists; from then on only
  // that task touches it.
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err == ESP_OK) {
    ui_screens_define_glyphs();
  } else {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
  }
  if (xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL,
                              UI_TASK_PRIORITY, &s_task,
                              UI_TASK_CORE) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  queues->keys = s_key_events;
  queues->player_events = s_player_events;
  queues->library_status = s_library_status;
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
