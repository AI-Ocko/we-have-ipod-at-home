#pragma once

#include "browse.h"
#include "keys.h"
#include "menu.h"
#include "options.h"
#include "play_queue.h"
#include "player_status.h"
#include "library.h"
#include "player.h"
#include "playlist_task.h"
#include "screen.h"
#include "standby.h"
#include "state_task.h"
#include "textentry.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stdint.h>

// The UI task's state, shared by ui.c, ui_options.c, ui_play.c,
// ui_playlists.c and ui_text.c (which change it) and ui_screens.c (which
// draws it). Private to those six. The design is described at the top of
// ui.c.

// A name, a title or an artist from the index: at most 127 characters.
#define UI_TEXT_MAX 128
// A track's path: the mount point and a path from the index (256 bytes).
#define UI_PATH_MAX 320
// The top row of Now Playing: a file name (at most 255 characters on the
// card), or a title and an artist (at most 127 each) with " - " between.
#define UI_TITLE_MAX 260

#define UI_OVERLAY_MS 1500 // how long the volume bar or a notice stays
#define UI_FAILURES_MAX 3  // unplayable tracks in a row before giving up
#define UI_SAVE_EVERY_MS 30000 // how often a playing track's time is saved
// How long after the sleep fade's stop (or a fade cancelled by a pause or a
// stop) the player's volume goes back to the setting: the player stops a
// moment after it is told to, and what it still plays meanwhile must not
// come out at the full volume (ui.c, STANDBY).
#define UI_VOLUME_BACK_MS 500

// The most tracks the queue holds (the spec's 8,192), which is also the most
// the state task saves.
#define UI_QUEUE_CAPACITY STATE_QUEUE_CAPACITY

typedef enum {
  UI_MODE_MENU,        // the menu tree (or what covers it: see ui.c)
  UI_MODE_NOW_PLAYING,
  UI_MODE_KEYTEST,
  UI_MODE_ABOUT,
} ui_mode_t;

// What covers the screen of the mode (the menus or Now Playing) without
// being a screen of its own: see ui.c, LAYERS. At most one at a time.
typedef enum {
  UI_LAYER_NONE,
  UI_LAYER_OPTIONS, // the options menu (ui_options.c)
  UI_LAYER_PICKER,  // "Add to playlist": which playlist (ui_playlists.c)
  UI_LAYER_CONFIRM, // "Delete" and the name: No or Yes (ui_playlists.c)
  UI_LAYER_TEXT,    // typing a name (ui_text.c)
} ui_layer_t;

// About's free space: asked for, read, or it could not be had.
typedef enum {
  UI_SPACE_WAITING, // "Free ..."
  UI_SPACE_KNOWN,
  UI_SPACE_UNKNOWN, // "Free ?"
} ui_space_t;

// What the bottom row of Now Playing says in place of the time.
typedef enum {
  UI_NOTICE_CANT_PLAY,    // "Can't play": it would not decode
  UI_NOTICE_FILE_MISSING, // "File missing": gone since the last scan
} ui_notice_t;

// What takes over the bottom row for UI_OVERLAY_MS: the volume bar, or a
// few words that confirm what a key just did.
typedef enum {
  UI_OVERLAY_VOLUME,
  UI_OVERLAY_SHUFFLE_ON,
  UI_OVERLAY_SHUFFLE_OFF,
  UI_OVERLAY_LOOP_OFF,
  UI_OVERLAY_LOOP_ALL,
  UI_OVERLAY_LOOP_ONE,
  UI_OVERLAY_ADDED,
  UI_OVERLAY_QUEUE_FULL,
  // Stage 7: the playlists' results and refusals (ui_playlists.c, ui_text.c).
  UI_OVERLAY_SAVED,
  UI_OVERLAY_SAVED_FIRST_1000,
  UI_OVERLAY_RENAMED,
  UI_OVERLAY_DELETED,
  UI_OVERLAY_PLAYLIST_FULL,
  UI_OVERLAY_NAME_IN_USE,
  UI_OVERLAY_CARD_ERROR,
  UI_OVERLAY_LIBRARY_BUSY,
  UI_OVERLAY_TRY_AGAIN, // the index has changed under the tracks
  UI_OVERLAY_BUSY,      // a request could not be queued
  UI_OVERLAY_BAD_FILE,  // a playlist with a line too long to read
  UI_OVERLAY_FILE_MISSING,
  UI_OVERLAY_NOTHING_TO_PLAY,
  UI_OVERLAY_NOTHING_PLAYING,
  UI_OVERLAY_QUEUE_EMPTY,
  UI_OVERLAY_NAME_EMPTY,
  UI_OVERLAY_NAME_TOO_LONG,
  UI_OVERLAY_BAD_CHARACTER,
  UI_OVERLAY_BAD_END,
  UI_OVERLAY_BAD_START,
  UI_OVERLAY_NAME_RESERVED, // a name Windows keeps for a device (CON, ...)
} ui_overlay_t;

// The track that is playing, or the last one that was. ui_play.c changes
// this; its design notes say how.
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

  // The track is the queue's current track (ui_t.queue): when it ends the
  // queue says what follows. False for a track the console started.
  bool from_queue;
  uint32_t saved_ms; // `elapsed_ms` as it was last handed over to be saved

  uint32_t failures;       // tracks in a row that would not play
  ui_notice_t notice;
  int64_t notice_until_us; // 0: no notice. Else the bottom row shows it,
                           // and at this time the next track is started
} ui_play_t;

// --- the playlists' side of the UI (ui_playlists.c, ui_text.c) -------------
//
// All of it is one block of PSRAM, allocated once by ui_start (ui_t holds
// the pointer): the catalog alone is 17 KB, and internal RAM has about 38 KB
// free while a track plays. Sizes are in ui_playlists.c, MEMORY.

// Edits sent and not yet answered, at most (ui_playlists.c, EDITS).
#define UI_EDITS_MAX 32

// What a text entry is for (ui_text.c).
typedef enum {
  UI_TEXT_NEW,         // "New playlist": an empty playlist
  UI_TEXT_NEW_TRACKS,  // "New..." in the picker: a playlist of `tracks`
  UI_TEXT_SAVE_QUEUE,  // "Save queue": a playlist of `tracks`, the queue's
  UI_TEXT_RENAME,      // "Rename": the playlist `target_file`
} ui_text_purpose_t;

// An edit sent to the playlist task, until its event comes.
typedef struct {
  uint32_t request; // its number; 0: the slot is free
  uint8_t kind;     // playlist_request_t
  uint8_t flags;    // UI_EDIT_*
  uint32_t serial;  // the open playlist's `open_serial` when it was sent
} ui_edit_t;

#define UI_EDIT_ON_OPEN 1u   // it changes the open playlist
#define UI_EDIT_RELOAD 2u    // when done, load the open playlist again
#define UI_EDIT_CUT 4u       // a Save queue that kept the first 1,000
#define UI_EDIT_FOLLOW 8u    // a create or rename: the cursor follows it

typedef struct {
  // The playlists in the folder, as the newest listing had them. Only
  // ready once a listing has arrived; until then the Playlists screen says
  // "Loading". `list_request` is the listing waited for (0: none).
  playlist_catalog_t catalog;
  bool catalog_ready;
  uint32_t list_request;
  // No catalog yet and the listing failed or was refused: the Playlists
  // screen shows why (list_why) instead of "Loading", and 5 or 6 asks again.
  bool list_failed;
  ui_overlay_t list_why;
  // A create or a rename: the name it gave, and whether it is done. The
  // first listing after it is done puts the Playlists screen's cursor on
  // that name (if the screen is on the stack), and then it is forgotten.
  char follow[PLAYLIST_NAME_MAX + 1];
  bool follow_done;

  // What browse reads for the playlist screens (browse_set_playlists).
  browse_playlists_t view;

  // The open playlist: its file name ("" when none is open), its entries
  // as the last load gave them and changed since by this side's own moves
  // and removes, and the load waited for. `open_serial` counts the
  // playlists opened, so that an answer about an earlier one is known.
  char open_file[PLAYLIST_FILE_MAX + 1];
  uint32_t open_serial;
  uint32_t load_request;
  bool open_loaded;
  // A move or a remove of the open playlist failed: the copy differs from
  // the file until a load asked for since replaces it. Meanwhile moves and
  // removes are refused ("Busy"), and the load is asked for again after
  // every key and event until the task takes it (ui_pl_retry_reload).
  bool reload_failed;
  playlist_loaded_t open;

  // "Play" or "Shuffle play" on a playlist row: the load waited for, and
  // where it lands.
  uint32_t play_request;
  bool play_shuffle;
  playlist_loaded_t play;

  // Tracks to add, from the picker's target or the queue: numbers of the
  // index whose checksum is `tracks_index`, made when the picker or the
  // text entry for Save queue opened.
  uint16_t tracks[PLAYLIST_ENTRIES_MAX];
  uint32_t track_count;
  uint32_t tracks_index;
  bool tracks_cut; // the queue had more than PLAYLIST_ENTRIES_MAX

  // The playlist the options menu, the delete confirmation or a rename are
  // about, by file name, and its name for the confirmation's top row.
  char target_file[PLAYLIST_FILE_MAX + 1];
  char target_name[PLAYLIST_FILE_MAX + 1];

  // The picker: row 0 "New...", row r the catalog's playlist r - 1.
  menu_t picker_menu;
  int64_t picker_since_us; // the marquee's zero
  // The confirmation: row 0 "No", row 1 "Yes".
  menu_t confirm_menu;
  int64_t confirm_since_us;

  // The text entry.
  textentry_t text;
  ui_text_purpose_t text_purpose;

  ui_edit_t edits[UI_EDITS_MAX];
} ui_pl_t;

typedef struct {
  ui_mode_t mode;
  ui_layer_t layer; // over the menus or Now Playing

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
  bool list_loading; // a playlist screen waiting for its listing or load

  // The options menu (layer UI_LAYER_OPTIONS), over the list or Now
  // Playing: the actions `#` offers for the item that was under the cursor,
  // or for Now Playing. Its numbers belong to `generation`.
  browse_target_t options_target;
  option_t options[OPTIONS_MAX];
  menu_t options_menu;              // how many actions, and the cursor
  char options_title[UI_TEXT_MAX]; // the item's name

  // The overlay: on the bottom row until this time. 0 when there is none.
  ui_overlay_t overlay;
  int64_t overlay_until_us;

  // The settings as they were last handed to the state task to be saved.
  // Shuffle and loop here are always the queue's.
  state_settings_t settings;

  // The play queue (ui_play.c). Its track numbers belong to the index whose
  // checksum is `queue_index`; an empty queue belongs to none.
  play_queue_t queue;
  uint32_t queue_index;
  bool queue_restored; // the queue saved at power-off has been asked for

  // Now Playing: 4 or 6 has been pressed there and not held long enough to
  // be a long press: its release skips. '\0' when neither is.
  char skip_key;

  // The key test screen.
  bool keytest_any; // an event has been shown
  key_event_t keytest_last;
  unsigned keytest_count;
  char keytest_opened_by; // the key whose release is not to be shown

  // About: the track count, and the card's free space, which the playlist
  // task reads (the request waited for; 0 when none is).
  uint32_t about_tracks;
  uint32_t about_space_request;
  ui_space_t about_space;
  uint64_t about_free_bytes; // when UI_SPACE_KNOWN
  int64_t about_since_us;    // the bottom row's marquee's zero

  // The backlight and the sleep timer (ui.c, STANDBY). `standby` decides;
  // the rest carries its decisions out.
  standby_t standby;
  browse_settings_t settings_view; // what the Settings rows show
  bool lcd_lit;          // what the LCD's backlight was last switched to
  int fade_volume;       // the level the fade last gave the player; -1: none
  bool volume_back;      // the player's volume is to go back to the setting
  int64_t volume_back_ms; // at this time; INT64_MAX: when the pass ends

  ui_play_t play;

  // Key repeats not yet logged (see log_key in ui.c).
  char repeat_key;
  uint32_t repeat_count;

  uint32_t underruns; // how many have been reported so far

  ui_pl_t *pl; // PSRAM: see above
} ui_t;

// Whether the menu tree is what the menu side shows: not while the card is
// still awaited or a scan runs (those screens cover it).
static inline bool ui_list_on_show(const ui_t *ui) {
  return ui->mode == UI_MODE_MENU && !ui->no_card && !ui->scanning;
}

// Whether the list is what is seen: on show and no layer over it.
static inline bool ui_list_seen(const ui_t *ui) {
  return ui_list_on_show(ui) && ui->layer == UI_LAYER_NONE;
}

// Put `what` on the bottom row for UI_OVERLAY_MS.
static inline void ui_overlay_show(ui_t *ui, ui_overlay_t what,
                                   int64_t now_us) {
  ui->overlay = what;
  ui->overlay_until_us = now_us + UI_OVERLAY_MS * 1000;
}

// --- ui.c --------------------------------------------------------------------

// The index, if there is one and it is the one the UI's numbers belong to;
// NULL otherwise. Whoever gets one calls library_task_put before returning.
const library_t *ui_index_get(const ui_t *ui);

// Put Now Playing up.
void ui_show_now_playing(ui_t *ui, int64_t now_us);

// Whatever layer is up goes, as its own "close" key would make it go (a
// text entry is cancelled).
void ui_close_layer(ui_t *ui);

// --- ui_options.c: the options menu ------------------------------------------

// `#` in a list: open the menu for the item under the cursor, if it is one
// that has options. Takes the index once.
void ui_options_open(ui_t *ui);
// `#` on Now Playing: the menu for the current track and the queue.
void ui_options_open_now_playing(ui_t *ui);
void ui_options_close(ui_t *ui);

// A key while the menu is open.
void ui_options_on_key(ui_t *ui, const key_event_t *event, int64_t now_us);

// --- ui_play.c: the track and the queue --------------------------------------

// Set the queue up, empty, over `orders`: room for 2 * UI_QUEUE_CAPACITY
// track numbers. Shuffle and loop are taken from ui->settings. Once, before
// anything else here.
void ui_play_init(ui_t *ui, uint16_t *orders);

// Replace the queue with `list` and make its position `start` the current
// track; with `shuffle`, turn shuffle on first and start from a track picked
// at random instead. Called with the index held (`lib` is not NULL): copies
// what the track needs out of it. False if the list has no tracks. Follow
// with ui_play_start once the index has been given back.
bool ui_play_load(ui_t *ui, const library_t *lib, tracklist_t list,
                  uint32_t start, bool shuffle, int64_t now_us);
void ui_play_start(ui_t *ui);

// The same for a playlist's entries (`tracks`, numbers of `lib`, with
// PLAYLIST_MISSING for a missing one): the queue becomes the playable ones,
// current at entry `start`, which must be playable. With `shuffle`, turn
// shuffle on first (`start` is then the caller's random pick). False if
// nothing was taken. Follow with ui_play_start once the index is back.
bool ui_play_load_tracks(ui_t *ui, const library_t *lib, const uint16_t *tracks,
                         uint32_t count, uint32_t start, bool shuffle,
                         int64_t now_us);

// Put what `target` is (a track, or the tracks of an album or an artist) in
// the queue: a track after the current one if `next`, everything else at the
// end. Called with the index held. False, and nothing is added, if it does
// not all fit. Starts nothing; if the queue was empty and nothing is
// playing, the first track added is the one D will start.
bool ui_play_add(ui_t *ui, const library_t *lib, const browse_target_t *target,
                 bool next, int64_t now_us);

// D, and 5 on Now Playing: pause, resume, or start the remembered track
// from where it is shown to be.
void ui_play_toggle(ui_t *ui, int64_t now_us);

// 6 and 4 on Now Playing: the queue's next track, and its previous one or
// the current one from its beginning. Each takes the index once.
void ui_play_next(ui_t *ui, int64_t now_us);
void ui_play_previous(ui_t *ui, int64_t now_us);

// 1 and 3 on Now Playing. Each puts its confirmation on the bottom row.
void ui_play_toggle_shuffle(ui_t *ui, int64_t now_us);
void ui_play_cycle_loop(ui_t *ui, int64_t now_us);

// A player event: log it and, if it is about the current track, act on it.
void ui_play_on_event(ui_t *ui, const player_event_t *event, int64_t now_us);

// Once per pass: a notice whose time is up goes, and the next track starts.
void ui_play_on_time(ui_t *ui, int64_t now_us);

// Stop the player and stay stopped: the track is remembered, and the queue
// does not go on. For a scan, and for the console's `stop`.
void ui_play_stop(ui_t *ui);

// A new index has been published. The first time, the queue saved at
// power-off is restored; after that the queue stays if the index is the one
// it was built for and is emptied if not. The track on Now Playing is found
// in the index again. True if Now Playing should be put up: the restored
// queue has a current track, which waits there, paused.
bool ui_play_library_changed(ui_t *ui, int64_t now_us);

// --- ui_playlists.c: the playlist screens, the picker, the confirmation ------

// Point browse at what the playlist screens show. Once, at the start.
void ui_pl_init(ui_t *ui, ui_pl_t *pl);

// A list key has changed the menus (ui.c calls it after every one): a
// playlist level that was left closes the open playlist, and one that was
// entered asks for what it shows.
void ui_pl_after_list_key(ui_t *ui, int depth_before, int64_t now_us);

// What a select in a list asked for (browse.h). Called with the index held
// (NULL: none), for the actions that need it.
void ui_pl_open_playlist(ui_t *ui, uint32_t index, int64_t now_us);
bool ui_pl_play_entries(ui_t *ui, const library_t *lib, uint32_t entry,
                        int64_t now_us);

// Whether the list on show is a playlist screen still waiting for what it
// lists ("Loading", or why its first listing failed).
bool ui_pl_loading(const ui_t *ui);

// 5 or 6 on a Playlists screen whose first listing failed: ask again. True
// if that is what the key did.
bool ui_pl_retry_listing(ui_t *ui, int64_t now_us);

// The options menu has just opened on options_target: a playlist row's
// file name is kept (target_file), so a listing meanwhile changes nothing.
void ui_pl_options_opened(ui_t *ui);
// The options menu's playlist actions, with the index held (not NULL).
// True if a track was loaded to be played (the caller starts it).
bool ui_pl_option(ui_t *ui, const library_t *lib, option_t option,
                  int64_t now_us);

// Keys for the picker and the confirmation.
void ui_pl_picker_key(ui_t *ui, const key_event_t *event, int64_t now_us);
void ui_pl_confirm_key(ui_t *ui, const key_event_t *event, int64_t now_us);

// The text entry is done: the name is checked and passes. Send what it was
// for. False (and "Busy" shown) if it could not be sent.
bool ui_pl_send_name(ui_t *ui, const char *name, int64_t now_us);

// What a refused name says on the bottom row.
ui_overlay_t ui_name_overlay(playlist_name_t why);

// A listing for a name check or a screen.
void ui_pl_list(ui_t *ui, int64_t now_us);

// An event of the playlist task.
void ui_pl_on_event(ui_t *ui, const playlist_event_t *event, int64_t now_us);

// After every key and event (ui.c): if the open playlist's copy is known
// to differ from its file and the load that would put it right was
// refused, ask for it again. O(1) when there is nothing to do.
void ui_pl_retry_reload(ui_t *ui);

// The index is going (a scan began) or has been replaced: nothing that holds
// its track numbers may go on (ui.c, THE INDEX).
void ui_pl_index_changed(ui_t *ui, int64_t now_us);

// The text of the picker's row `row`: "New..." or a playlist's name. For
// drawing.
void ui_pl_row_name(const ui_t *ui, uint32_t row, char *out, size_t size);

// --- ui_text.c: the text entry -----------------------------------------------

// Open it for `purpose`, starting with `initial` (NULL: nothing).
void ui_text_open(ui_t *ui, ui_text_purpose_t purpose, const char *initial,
                  int64_t now_us);
// Close it, sending nothing.
void ui_text_cancel(ui_t *ui);
// A key while it is up (C included).
void ui_text_on_key(ui_t *ui, const key_event_t *event, int64_t now_us);
// Once per pass: a pending character whose second is up is committed.
void ui_text_on_time(ui_t *ui, int64_t now_us);

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
