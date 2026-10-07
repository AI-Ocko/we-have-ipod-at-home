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
#include "standby.h"
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
// library's status and the playlist task's answers. They are joined in a
// queue set, so the task sleeps on all of them at once. One pass of its loop
// is:
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
//                While there is no card (start-up waits for one, or one was
//                pulled out: THE CARD), "Insert SD card" covers the list;
//                while the library is scanned, "Scanning..." and the count
//                do. These are not modes: they come and go with what the
//                card and the library report, and the list keys do nothing
//                while one is up.
//   NOW_PLAYING  the current track's title and artist, the play or pause
//                symbol, the time and the shuffle and loop marks; "Nothing
//                playing" if there is no track. Its state is ui_t.play and
//                ui_t.queue, which are kept up whatever screen is up.
//                Choosing a track in a list comes here, and so does
//                power-on if a track was remembered.
//   KEYTEST      the key test screen: keytest_*.
//   ABOUT        about_tracks on the top row; the card's free space and the
//                firmware's build date as a marquee on the bottom row. The
//                free space is asked of the playlist task when About opens
//                (it reads the card; this task never does) and comes back
//                as an event: "Free ..." until then, "Free ?" if it failed.
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
// newest replaces the one before. Words (not the volume bar) go early when
// the user closes the options menu, the picker or the confirmation, and
// when Now Playing gives way to the menus or the menus to Now Playing: they
// were about what has gone (ui_overlay_end_words). A notice ("Can't play",
// "File missing") replaces Now Playing's bottom row until
// play.notice_until_us, and when it goes the next track is started; the
// overlay is drawn over it. Each is a time in ui_t: ui_render compares it
// with the clock, and ui_next_change_ms makes it one of the moments the
// loop sleeps until, along with the marquee's next step. There is one sleep
// and no timer.
//
// KEYS
//
// Every key event goes to standby_key first, before anything else (the key
// test included): the press that wakes the backlight, and that key's long
// press, repeats and release, are dropped there (STANDBY).
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
//           6 is next and 4 previous, ON RELEASE (below); held, 6 is
//           fast-forward and 4 rewind (below); `#` opens the options menu
//           for the current track and the queue
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
// Holding them seeks (ui_play.c, FAST-FORWARD AND REWIND). The long press
// starts a scrub only if skip_key still names that key, that is, if its
// press was made on Now Playing and acted on (a press that woke the
// backlight never gets here, and nor do its long press and repeats); its
// repeats move the target, and its release, which skip_key no longer
// names, jumps there. Any press drops a scrub, before standby_key, so the
// next key can never find one left over.
//
// STANDBY: THE BACKLIGHT AND THE SLEEP TIMER
//
// standby.h decides; this file carries out what it decides. `standby` holds
// the backlight timeout, the sleep timer and the fade, as times in ms of
// esp_timer's clock; it is moved on by standby_tick at the top of every pass
// (step 2), and standby_deadline is one of the times step 5 sleeps until, so
// the backlight going off, the timer running out and each step of the fade's
// volume all happen with nothing polled. The timers count whatever else
// happens: a scan, a card removed, any screen. Nothing here touches the card.
//
//   the backlight  standby.lit is what it should be; lcd_lit is what the LCD
//                  was last told. They are compared once per pass and the
//                  LCD switched only when they differ, whatever changed lit
//                  (a timeout, the sleep timer, the fade's end, a key).
//   the fade       when the timer runs out while a track plays, the volume
//                  steps down from the setting to 0 over STANDBY_FADE_MS:
//                  each level goes to the player (player_set_volume) and
//                  nowhere else. ui->settings.volume, which is what the
//                  state task is handed, never changes, so the fade is never
//                  saved, and a reset during it starts at the setting.
//   its end        standby_tick says STOP: the player is stopped as the
//                  console's `stop` stops it (ui_play_stop), the backlight
//                  goes off (standby has made lit false), and the volume
//                  goes back to the setting UI_VOLUME_BACK_MS later.
//   a cancel       a key press during the fade (standby_key ends it), or
//                  playback that is no longer playing: paused, stopped (the
//                  console, a scan) or come to rest at the end of the queue.
//                  The last is checked once per pass after the events: a
//                  fade is running and nothing plays (ui_playing) means
//                  standby_set_sleep(OFF). A gap between two tracks of the
//                  queue is not "nothing plays": the next track is started in
//                  the same call that saw the last one end, and a "Can't
//                  play" notice is on its way to the next track. A pause
//                  while the timer is still counting changes nothing.
//   the volume     after a cancel it goes back at the end of that pass if a
//                  track is still playing (a key that is not a pause); if not
//                  (a pause, a stop), UI_VOLUME_BACK_MS later: the player
//                  stops or pauses a moment after it is told to, fading out
//                  what it was playing, and that should fade from where the
//                  sleep fade had got to, not from the full setting. Playback
//                  starting again, or A or B, brings it back at once.
//
// The Settings rows read settings_view (browse_set_settings). It is brought
// up to date each pass; when the Settings screen is up the list's copy is
// renewed when a value changes, and the moment the minutes left next change
// is one of the times the loop sleeps until.
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
// menus, which the scan screen covers until the new index comes. The queue's
// numbers are tied to an index by its checksum instead, because a queue
// outlives a scan that finds the same tracks (ui_play.c, A NEW INDEX).
//
// With no index (a scan that failed, or before the first load) the library's
// lists are empty: the list's name and "(empty)".
//
// A scan that stopped because the index was full says "Library full" for
// 1.5 s as it ends, and About shows "(full)" after the track count for as
// long as that partial index is the one loaded (library_status_t.full).
//
// THE CARD
//
// The task that mounts the card sends card events (ui_card_waiting,
// ui_card_lost); this task never touches the card. When the card is pulled
// out while the player runs: playback stops as ui_stop stops it (the track
// stays on Now Playing, at the time it had reached), any layer closes, the
// menus come up with "Card error" over them for 1.5 s and then "Insert SD
// card", and nothing starts a track until the card is back (D, 4, 6, a
// scrub and a notice running out all look at no_card). Keys keep working:
// the volume, C to Now Playing and back, shuffle and loop, the options menu
// on Now Playing. The backlight and sleep timers keep counting. The index
// that was loaded stays (it is in memory); when the card is back the library
// publishes one again, and the queue is kept if it is the same index or
// emptied if not, as after a scan (ui_play.c, A NEW INDEX). The UI reports a
// track's read error (PLAYER_ERROR_IO) as a card failure (storage.h).
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

// The backlight and the sleep timer for the console's `stat`: copied by the
// UI task once per pass, inside s_standby_lock.
static portMUX_TYPE s_standby_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_standby_lit = true;
static uint32_t s_standby_backlight;
static int64_t s_standby_sleep_at_ms = INT64_MAX;
static bool s_standby_fading;

// What the task that waits for the card tells the UI task (ui_card_waiting).
// The UI task owns the LCD, so the task that waits for the card cannot draw
// the "insert card" screen itself; it sends one of these instead.
typedef enum {
  CARD_EVENT_WAITING, // the card did not mount; the mount is being retried
  CARD_EVENT_LOST,    // the card stopped answering; it is to be retried
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

// --- standby: the backlight and the sleep timer (see STANDBY) ----------------

static const char *const BACKLIGHT_NAME[STANDBY_BACKLIGHT_COUNT] = {
    "15 s", "30 s", "60 s", "on"};
static const uint32_t SLEEP_MINUTES[STANDBY_SLEEP_COUNT] = {0, 15, 30, 60};

static bool ui_fading(const ui_t *ui) {
  return ui->standby.fade_from_ms != INT64_MAX;
}

// Whether a track is playing, as far as the sleep timer is concerned: one is
// the player's or on its way to it (a notice that gives way to the next
// track), and it is not paused.
static bool ui_playing(const ui_t *ui, const player_status_t *status) {
  return (ui->play.active || ui->play.notice_until_us != 0) &&
         status->state != PLAYER_STATE_PAUSED;
}

// The player's volume back to the setting.
static void ui_volume_back(ui_t *ui) {
  player_set_volume(ui->settings.volume);
  ESP_LOGI(TAG, "SLEEP volume back to %d", ui->settings.volume);
  ui->fade_volume = -1;
  ui->volume_back = false;
  ui->volume_back_ms = INT64_MAX;
}

// The fade has ended before its end: its volume goes back (when: see
// ui_standby_after_events).
static void ui_fade_cancelled(ui_t *ui, const char *why) {
  ESP_LOGI(TAG, "SLEEP fade cancelled (%s)", why);
  ui->volume_back = true;
  ui->volume_back_ms = INT64_MAX;
}

// Switch the LCD's backlight if it is not what standby says.
static void ui_backlight_sync(ui_t *ui) {
  if (ui->standby.lit == ui->lcd_lit) {
    return;
  }
  ui->lcd_lit = ui->standby.lit;
  // The driver keeps the bit even if this transfer fails, and sends it with
  // the next one.
  esp_err_t err = lcd_set_backlight(ui->lcd_lit);
  ESP_LOGI(TAG, "BACKLIGHT %s%s", ui->lcd_lit ? "on" : "off",
           err == ESP_OK ? "" : " (not sent to the LCD)");
}

// Whether a track is playing (ui_playing), asking the player. That costs
// about 0.7 ms (player_get_status measures the audio tasks' stacks), so the
// standby code asks only when the answer decides something.
static bool ui_playing_now(const ui_t *ui) {
  player_status_t status;
  player_get_status(&status);
  return ui_playing(ui, &status);
}

// Step 2 of a pass: whatever time has come for the backlight, the timer and
// the fade.
static void ui_standby_on_time(ui_t *ui, int64_t now_ms) {
  if (ui->volume_back && now_ms >= ui->volume_back_ms) {
    ui_volume_back(ui);
  }
  bool timer_on = ui->standby.sleep_at_ms != INT64_MAX;
  // standby_tick reads `playing` only when the timer has run out and no
  // fade is running: that is a fade beginning, or the backlight going off.
  bool playing = !ui_fading(ui) && timer_on &&
                 now_ms >= ui->standby.sleep_at_ms && ui_playing_now(ui);
  uint32_t acts = standby_tick(&ui->standby, playing, now_ms);
  if (acts & STANDBY_ACT_FADE) {
    ESP_LOGI(TAG, "SLEEP timer ran out: fading over %d ms", STANDBY_FADE_MS);
  } else if (timer_on && ui->standby.sleep_at_ms == INT64_MAX &&
             !ui_fading(ui)) {
    ESP_LOGI(TAG, "SLEEP timer ran out with nothing playing");
  }
  if (acts & STANDBY_ACT_STOP) {
    // Silent first: the stop takes a moment.
    player_set_volume(0);
    ui->fade_volume = 0;
    ESP_LOGI(TAG, "SLEEP fade over: stopping");
    ui_play_stop(ui);
    // standby has made lit false: the backlight goes off below.
    ui->volume_back = true;
    ui->volume_back_ms = now_ms + UI_VOLUME_BACK_MS;
  }
  if (ui_fading(ui)) {
    int level = standby_fade_level(&ui->standby, ui->settings.volume, now_ms);
    if (level != ui->fade_volume) {
      player_set_volume(level);
      ui->fade_volume = level;
      ESP_LOGI(TAG, "FADE volume=%d", level);
    }
  }
  ui_backlight_sync(ui);
}

// After a pass's events: a fade with nothing playing is over, and a volume
// that is to go back is given its time.
static void ui_standby_after_events(ui_t *ui, int64_t now_ms) {
  if (!ui_fading(ui) && !ui->volume_back) {
    return; // nothing below depends on whether a track is playing
  }
  bool playing = ui_playing_now(ui);
  if (ui_fading(ui) && !playing) {
    standby_set_sleep(&ui->standby, STANDBY_SLEEP_OFF, now_ms);
    ui_fade_cancelled(ui, "playback paused or stopped");
  }
  if (ui->volume_back) {
    if (playing) {
      ui_volume_back(ui);
    } else if (ui->volume_back_ms == INT64_MAX) {
      ui->volume_back_ms = now_ms + UI_VOLUME_BACK_MS;
    }
  }
}

static bool ui_settings_up(const ui_t *ui) {
  return ui_list_seen(ui) &&
         ui->browse.levels[ui->browse.depth].screen == BROWSE_SETTINGS;
}

// What the Settings rows show, brought up to date. The list's copy is
// renewed if it changed while they are up.
static void ui_settings_view(ui_t *ui, int64_t now_ms) {
  browse_settings_t view = {
      .backlight = ui->standby.backlight,
      .sleep_minutes = standby_sleep_minutes(&ui->standby, now_ms),
  };
  if (view.backlight != ui->settings_view.backlight ||
      view.sleep_minutes != ui->settings_view.sleep_minutes) {
    ui->settings_view = view;
    if (ui_settings_up(ui)) {
      ui->list_dirty = true;
    }
  }
}

// The next time anything of the above is due, or INT64_MAX.
static int64_t ui_standby_deadline(const ui_t *ui, int64_t now_ms) {
  int64_t next =
      standby_deadline(&ui->standby, ui->settings.volume, now_ms);
  if (ui->volume_back && ui->volume_back_ms < next) {
    next = ui->volume_back_ms;
  }
  // The minutes left on the Settings row: they change when the time left
  // drops to a whole number of minutes.
  uint32_t minutes = ui->settings_view.sleep_minutes;
  if (minutes > 1 && ui_settings_up(ui)) {
    int64_t step = ui->standby.sleep_at_ms - (int64_t)(minutes - 1) * 60000;
    if (step < next) {
      next = step;
    }
  }
  return next;
}

// 5 or 6 on the Backlight row: the next timeout, saved with the settings.
static void ui_next_backlight(ui_t *ui, int64_t now_ms) {
  standby_backlight_t next =
      (standby_backlight_t)((ui->standby.backlight + 1) %
                            STANDBY_BACKLIGHT_COUNT);
  standby_set_backlight(&ui->standby, next, now_ms);
  ui->settings.backlight = next;
  state_task_save_settings(&ui->settings);
  ESP_LOGI(TAG, "BACKLIGHT setting %s", BACKLIGHT_NAME[next]);
}

// 5 or 6 on the Sleep timer row: the next of off, 15, 30, 60 after the one
// last chosen, counted from now.
static void ui_next_sleep(ui_t *ui, int64_t now_ms) {
  standby_sleep_t next = standby_next_sleep(&ui->standby);
  standby_set_sleep(&ui->standby, next, now_ms);
  if (next == STANDBY_SLEEP_OFF) {
    ESP_LOGI(TAG, "SLEEP timer off");
  } else {
    ESP_LOGI(TAG, "SLEEP timer %" PRIu32 " min", SLEEP_MINUTES[next]);
  }
}

// For the console (ui_get_stats): a few words, copied once per pass.
static void ui_publish_standby(const ui_t *ui) {
  portENTER_CRITICAL(&s_standby_lock);
  s_standby_lit = ui->standby.lit;
  s_standby_backlight = (uint32_t)ui->standby.backlight;
  s_standby_sleep_at_ms = ui->standby.sleep_at_ms;
  s_standby_fading = ui_fading(ui);
  portEXIT_CRITICAL(&s_standby_lock);
}

// --- About -------------------------------------------------------------------

// About has opened: ask the playlist task for the card's free space.
static void ui_about_open(ui_t *ui, int64_t now_us) {
  ui->mode = UI_MODE_ABOUT;
  ui->about_since_us = now_us;
  ui->about_space_request = playlist_task_space(PLAYLIST_CLIENT_UI);
  ui->about_space =
      ui->about_space_request != 0 ? UI_SPACE_WAITING : UI_SPACE_UNKNOWN;
}

// The answer. One for an earlier opening of About is not this one's.
static void ui_about_on_space(ui_t *ui, const playlist_event_t *event,
                              int64_t now_us) {
  if (event->request == 0 || event->request != ui->about_space_request) {
    return;
  }
  ui->about_space_request = 0;
  playlist_space_t space;
  if (event->outcome == PLAYLIST_TASK_DONE &&
      playlist_task_copy_space(PLAYLIST_CLIENT_UI, event->request, &space)) {
    ui->about_space = UI_SPACE_KNOWN;
    ui->about_free_bytes = space.free_bytes;
  } else {
    ui->about_space = UI_SPACE_UNKNOWN;
  }
  ui->about_since_us = now_us; // new words: read from their beginning
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
  if (ui->mode != UI_MODE_NOW_PLAYING) {
    ui_overlay_end_words(ui); // said on the screen that is going (OVERLAYS)
  }
  ui->mode = UI_MODE_NOW_PLAYING;
  ui->play.title_since_us = now_us; // the title is read from its beginning
}

static void ui_show_menus(ui_t *ui) {
  if (ui->mode != UI_MODE_MENU) {
    ui_overlay_end_words(ui);
  }
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
  case BROWSE_BACKLIGHT:
    ui_next_backlight(ui, now_us / 1000); // the row: ui_settings_view
    break;
  case BROWSE_SLEEP:
    ui_next_sleep(ui, now_us / 1000);
    break;
  case BROWSE_KEYTEST:
    ui->mode = UI_MODE_KEYTEST;
    ui->keytest_any = false;
    ui->keytest_count = 0;
    ui->keytest_opened_by = key;
    ui->overlay_until_us = 0;
    break;
  case BROWSE_ABOUT:
    ui_about_open(ui, now_us);
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
    ui_overlay_end_words(ui);
    break;
  case UI_LAYER_PICKER:
  case UI_LAYER_CONFIRM:
    ui->layer = UI_LAYER_NONE;
    ui->list_dirty = true;
    ui->list_restart = true;
    ui_overlay_end_words(ui);
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
    int direction = key == '6' ? +1 : -1;
    bool scrubbing = ui->scrub.active && ui->scrub.direction == direction;
    switch (event->type) {
    case KEY_PRESS:
      ui->skip_key = key;
      break;
    case KEY_LONG:
      if (ui->skip_key == key) {
        ui_play_scrub_start(ui, direction, now_us);
      }
      ui->skip_key = '\0'; // held: not a skip
      break;
    case KEY_REPEAT:
      ui->skip_key = '\0';
      if (scrubbing) {
        ui_play_scrub_repeat(ui, now_us);
      }
      break;
    case KEY_RELEASE:
      if (ui->skip_key == key) {
        ui->skip_key = '\0';
        if (key == '6') {
          ui_play_next(ui, now_us);
        } else {
          ui_play_previous(ui, now_us);
        }
      } else if (scrubbing) {
        ui_play_scrub_release(ui);
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
    // The keypad sends the held key's release before another press, so a
    // scrub still going is one whose release was lost (see KEYS).
    ui_play_scrub_drop(ui, "a key press");
  }
  // Before anything else (STANDBY). A press during the fade ends it.
  bool was_fading = ui_fading(ui);
  bool act = standby_key(&ui->standby, event->key, event->type, now_us / 1000);
  if (was_fading && !ui_fading(ui)) {
    ui_fade_cancelled(ui, "a key");
  }
  if (!act) {
    if (event->type == KEY_PRESS) {
      ESP_LOGI(TAG, "KEY %c woke the backlight and does nothing else",
               event->key);
    }
    return;
  }
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
      // A volume key held down when the fade began: its repeats change the
      // volume, so the fade is over (and is never saved).
      if (ui_fading(ui)) {
        standby_set_sleep(&ui->standby, STANDBY_SLEEP_OFF, now_us / 1000);
        ui_fade_cancelled(ui, "a volume key");
      }
      if (ui->volume_back) {
        ui_volume_back(ui); // from the setting, not from a faded level
      }
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

// --- the player, the card and the library ------------------------------------

static void ui_on_library_status(ui_t *ui, const library_status_t *status,
                                 int64_t now_us) {
  bool scan_began = status->scanning && !ui->scanning;
  bool scan_ended = !status->scanning && ui->scanning;
  ui->scanning = status->scanning;
  ui->scan_tracks = status->tracks;
  if (scan_began) {
    ui->library_full = false; // "(full)" lasts until the next scan
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
    ui->library_full = status->full;
    browse_library_changed(&ui->browse);
    ui_pl_index_changed(ui, now_us); // layers close; the open playlist too
    ui->list_restart = true;
    if (ui_play_library_changed(ui, now_us) && ui->mode != UI_MODE_KEYTEST) {
      ui_show_now_playing(ui, now_us);
    }
    if (scan_ended && status->full) {
      // After the screen change above, which would end the words.
      ESP_LOGW(TAG, "Library full: the scan stopped at the index's cap");
      ui_overlay_show(ui, UI_OVERLAY_LIBRARY_FULL, now_us);
    }
  }
  // With or without a new index, what the lists hold may have changed.
  ui->list_dirty = true;
}

// What the task that mounts the card says. See THE CARD.
static void ui_on_card_event(ui_t *ui, card_event_t event, int64_t now_us) {
  switch (event) {
  case CARD_EVENT_WAITING:
    ui->no_card = true;
    break;
  case CARD_EVENT_LOST:
    ESP_LOGW(TAG, "CARD lost: stopping");
    ui_play_stop(ui); // the track stays; D resumes it when the card is back
    ui->no_card = true;
    ui->card_error_until_us = now_us + UI_OVERLAY_MS * 1000;
    ui_close_layer(ui);
    if (ui->mode != UI_MODE_KEYTEST) {
      ui_show_menus(ui); // where "Card error" is seen
    }
    break;
  case CARD_EVENT_MOUNTED:
    ESP_LOGI(TAG, "CARD mounted");
    ui->no_card = false;
    ui->card_error_until_us = 0;
    break;
  }
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
      ui_on_card_event(ui, event, now_us);
    }
  } else if (ready == s_library_status) {
    library_status_t status;
    if (xQueueReceive(s_library_status, &status, 0) == pdTRUE) {
      ui_on_library_status(ui, &status, now_us);
    }
  } else if (ready == s_playlist_events) {
    playlist_event_t event;
    if (xQueueReceive(s_playlist_events, &event, 0) == pdTRUE) {
      if (event.kind == PLAYLIST_REQUEST_SPACE) {
        ui_about_on_space(ui, &event, now_us);
      } else {
        ui_pl_on_event(ui, &event, now_us);
      }
    }
  }
  // A reload the playlist task refused, asked for again (ui_playlists.c).
  ui_pl_retry_reload(ui);
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

// --- the task ----------------------------------------------------------------

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
  standby_init(&ui.standby, ui.settings.backlight,
               esp_timer_get_time() / 1000);
  ui.lcd_lit = true; // lcd_init leaves it on
  ui.fade_volume = -1;
  ui.volume_back_ms = INT64_MAX;
  ui.settings_view.backlight = ui.standby.backlight;
  browse_set_settings(&ui.browse, &ui.settings_view);
  screen_t shown;        // what the LCD was last told to show
  bool shown_ok = false; // and it took it

  for (;;) {
    atomic_fetch_add(&s_passes, 1);
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;
    ui_standby_on_time(&ui, now_ms);
    ui_play_on_time(&ui, now_us);
    ui_text_on_time(&ui, now_us);
    ui_settings_view(&ui, now_ms);
    if (ui.list_dirty && ui_list_on_show(&ui)) {
      ui_fill_list(&ui, now_us);
    }
    // After whatever the times above did, and only if the screen shows any
    // of it: it costs about 0.7 ms (ui_playing_now), which a held key in a
    // list would otherwise pay on every pass.
    player_status_t player = {0};
    if (ui_render_needs_player(&ui, now_us)) {
      player_get_status(&player);
    }
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

    uint32_t wait_ms = ui_next_change_ms(&ui, now_us);
    int64_t standby_ms = ui_standby_deadline(&ui, now_ms);
    if (standby_ms != INT64_MAX) {
      // now_ms is rounded down, so this wakes at or after the deadline.
      int64_t left = standby_ms > now_ms ? standby_ms - now_ms : 0;
      if (left < (int64_t)wait_ms) {
        wait_ms = (uint32_t)left;
      }
    }
    ui_publish_standby(&ui);
    QueueSetMemberHandle_t ready =
        xQueueSelectFromSet(s_ui_events, ticks_until(wait_ms));
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
    ui_standby_after_events(&ui, now_us / 1000);
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

// Card events come from one task, the one that mounts the card, one at a
// time and seconds apart (a loss, then at least one 2 s retry before the
// card is back), so the queue of two is never full in practice; if it were,
// that task would wait for room, and the UI never waits for it.
void ui_card_waiting(bool waiting) {
  card_event_t event = waiting ? CARD_EVENT_WAITING : CARD_EVENT_MOUNTED;
  xQueueSend(s_card_events, &event, portMAX_DELAY);
}

void ui_card_lost(void) {
  const card_event_t event = CARD_EVENT_LOST;
  xQueueSend(s_card_events, &event, portMAX_DELAY);
}

void ui_get_stats(ui_stats_t *out) {
  out->stack_free = s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
  out->passes = atomic_load(&s_passes);
  out->lcd_updates = atomic_load(&s_lcd_updates);
  portENTER_CRITICAL(&s_standby_lock);
  out->lit = s_standby_lit;
  out->backlight = s_standby_backlight;
  int64_t sleep_at_ms = s_standby_sleep_at_ms;
  out->fading = s_standby_fading;
  portEXIT_CRITICAL(&s_standby_lock);
  int64_t now_ms = esp_timer_get_time() / 1000;
  out->sleep_left_s = sleep_at_ms == INT64_MAX || sleep_at_ms <= now_ms
                          ? 0
                          : (uint32_t)((sleep_at_ms - now_ms + 999) / 1000);
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
