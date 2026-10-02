# DAP Stage 5 (Browsing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the player usable from its own keypad and LCD: a menu tree over the library (Artists, Albums, Songs, Settings), a proper Now Playing screen, and selecting a track to play it, with the rest of its list following.

**Architecture:** The navigation logic is hardware-free and tested on the host: `menu` (a cursor over a list, the header text, letter jumps), `tracklist` (an album, an artist's songs or all songs, as a numbered list of tracks) and `browse` (the menu tree as a stack of levels over the library index). The UI task moves out of `app_main.c` into `main/ui.c`, where it turns key events into `browse` calls, draws screens through the existing `screen` and `marquee` modules, and drives the player. Player events gain a play number so that an event from an earlier track can never be mistaken for the current one.

**Tech Stack:** ESP-IDF v6.0.2 (C), FreeRTOS, Unity host tests under AddressSanitizer.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read section 3 "Performance rules", section 6 "Queries", section 7 for what is NOT in this stage, all of section 9, section 11, and stage 5 of section 14).

**How this plan works:** it fixes interfaces, behaviour, test cases and acceptance checks; implementation bodies and test code are the implementer's. Headers given here are binding. If an interface cannot be implemented as written, or forces an inefficiency, stop and report NEEDS_CONTEXT; do not change it silently.

## Scope

In this stage: the main menu, Artists, an artist's albums (with "All songs" first), Albums, Songs, Settings (Rescan library, Keypad test, About); Now Playing; selecting a track plays it and the rest of the list it came from follows in order; the volume overlay.

Not in this stage (later stages; do not build them): the play queue, shuffle, loop, previous/next keys and their rules, "Play next" and "Add to queue", resume at power-on (stage 6); playlists and text entry (stage 7); backlight timeout, sleep timer, fast-forward and rewind keys (stage 8). The `#` options key does nothing yet. The main menu has no Playlists entry until stage 7.

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port the SD card sometimes fails to mount; the firmware shows `Insert SD card` and retries every 2 seconds. If the retries go on for more than a minute, STOP and report BLOCKED: only the user can re-insert the card.
- **Library reader rule** (`main/library_task.h`, binding): `library_task_get()` never blocks and may return NULL (no index yet, a scan running, or the last scan failed). Every non-NULL get is followed promptly by `library_task_put()` in the same task: within one key press or one redraw. After the put, the pointer and every string from the index are dead: keep numbers and copies of text, never pointers. Track, album and artist numbers are valid only within one index; `library_status_t.generation` changes when a new index is published.
- **Keys (spec section 9):**

```
 1  2  3  A        A  volume up         (every screen)
 4  5  6  B        B  volume down       (every screen)
 7  8  9  C        C  toggle Now Playing / menus
 *  0  #  D        D  play / pause      (every screen)
```

| Key | Lists and menus | Now Playing |
|---|---|---|
| 2 / 8 | Up / down; hold to repeat | — |
| 5 or 6 | Open / select | 5 play/pause |
| 4 or * | Back | * back to menus |
| 1 / 3 | Jump to previous / next starting letter | — (stage 6) |
| # | — (stage 6) | — (stage 6) |

- **Screens (spec section 9):** lists show the list's name and position on the top row (`Artists    14/87`) and `>` plus the highlighted item on the bottom row; an item longer than 15 characters moves as a marquee. Now Playing shows `Title - Artist` as a marquee on the top row and the play or pause symbol with elapsed and total time on the bottom row. A volume change replaces the bottom row with a volume bar for 1.5 seconds, on any screen.
- **Performance rules (spec section 3):** nothing done at the keypad may loop over all tracks. A list's length and any item come straight from the index's stored ranges and order tables. Letter jumps are O(log n). The LCD is sent only what changed. When several key events are waiting, all are applied before the screen is drawn once. No allocation, no polling, no busy-waiting; waiting on a queue with a timeout for the next marquee step is fine.
- **Tasks:** UI task priority 5, core 0, 8 KB stack; it is the only task that may call `lcd_*`. Do not block it on the card or the player.
- **Player facts that must not be disturbed** (`main/player.c`; its design notes and `docs/superpowers/plans/stage3-*-investigation.md`): the decode loop's stall guard, `eos` handling, the registered FLAC parser wrapper, the pinned codec version, the two-task design. Task 3 of this plan changes `player.c` in one defined way only.
- Every FLAC that plays from start to end must still log `ENDED frames=X expected=X` (tag `player`).
- `components/dap_core` is pure C using only the C standard library. Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan and must not allocate from the heap.
- No compiler warnings from `main/` or `components/`. Code style as existing: 2-space indent, comments that say *why*.
- Nothing that can fail at run time because of a file or the card may use `ESP_ERROR_CHECK`, abort or restart.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Agents cannot hear the output, see the LCD or press keys. The console's `screen` command prints what the LCD has been told to show, and `key <char>` / `long <char>` inject key events; that is how screens are checked. Anything only a person can judge goes in the hardware check; do not claim it.
- Tools: `tools/serial_script.py <port> [--no-reset] <step>...` with steps `expect:<text>[:<timeout_s>]`, `send:<line>`, `sleep:<seconds>` (pipe `2>&1`). Console commands: `play <path> [start_ms]`, `pause`, `resume`, `stop`, `vol <n>`, `seek <ms>`, `key <char>`, `long <char>`, `stat`, `screen`, `scan`, `lib ...`; replies are `dbg: ok ...` or `dbg: err ...`. After `scan`, wait for `library: stack free` before using the library.
- What is on the card: `/music/` (two real tracks, including `16 - Kanye West - Hey Mama.flac`, 13453440 frames), `/test/` (untagged tones: `t01_44k_16_st.flac` is 441000 frames, 10 s), `/lib/` (the 14-track tagged fixture tree: see `test/host/fixtures/lib/README.md`) and `/lib1000/` (10 artists x 10 albums x 10 tracks of 0.1 s). The index holds 1,028 tracks, 108 albums, 17 artists.

## Review Focus

1. **The index disappears or changes under the menus.** A rescan while the user is three levels deep: the artist and album numbers on the stack now mean something else. The menus must never show another artist's albums under the old name or read out of range: on a new index they go back to the main menu. While there is no index, library lists are empty and say so. Pinned in Task 2 (`browse` with a NULL library and after a different library) and Task 4 (a `scan` issued while deep in the tree).
2. **Empty and one-item lists, and the ends of long ones.** Up from the first item, down from the last, letter jump with one item, an empty Songs list on an empty card. Pinned in Task 1.
3. **Long and odd item text:** an album name of 120 characters, a title that is all punctuation, a name starting with a digit or a symbol (which letter group is it in?). Pinned in Tasks 1 and 2.
4. **An event from the previous track arriving after a new one was started** (press a new track at the moment the old one ends): it must not advance the list twice or mark the new track as ended. Pinned in Task 3 by play numbers and a scripted race.
5. **A list that contains a file that will not play** (`/lib/loose/text.mp3`): the player says "Can't play" and moves on, and gives up after three failures in a row instead of skipping forever. Pinned in Task 5.
6. **Keys arriving faster than the LCD can be redrawn** (held `8` at 20 repeats a second): the cursor must end where the events say, with no lag building up. Pinned in Task 4 by injecting a burst and comparing the final position.
7. **Global keys on every screen:** volume, play/pause and the Now Playing toggle must work in a menu, in Settings, on About, during a scan and on the waiting-for-card screen, without disturbing where the user was. Pinned in Tasks 4 and 5.
8. **Audio must not notice the user browsing.** Pinned in Task 5 by playing the music track to its end while a script scrolls through the 1,028-song list: exact frame count, no underrun.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/menu.c/.h` | Cursor over a list, header text, letter jumps | 1 |
| `components/dap_core/tracklist.c/.h` | An album, an artist's songs, or all songs, as a numbered list of tracks | 2 |
| `components/dap_core/browse.c/.h` | The menu tree as a stack of levels over the library | 2 |
| `main/player.c/.h` | Play numbers on events | 3 |
| `main/debug_console.c/.h` | `hold` command | 3 |
| `main/ui.c/.h` | The UI task: screens, key handling, driving the player | 4, 5 |
| `main/app_main.c` | Start-up only | 4 |

---

### Task 1: `menu`

**Files:**
- Create: `components/dap_core/menu.h`, `components/dap_core/menu.c`
- Test: `test/host/test_menu.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Produces (`menu.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

// A cursor over a list of `count` items. The list's contents belong to
// whoever uses this; the menu only knows how many there are and where it is.

typedef struct {
  uint32_t count;
  uint32_t cursor; // 0 .. count - 1; 0 when the list is empty
} menu_t;

// A list of `count` items with the cursor on the first.
void menu_reset(menu_t *m, uint32_t count);

// The list now has `count` items. The cursor stays where it is if it can,
// and moves to the last item if it cannot.
void menu_set_count(menu_t *m, uint32_t count);

// Move by `delta` items. A single step past either end goes round to the
// other end if `wrap` is true; anything else stops at the end.
void menu_move(menu_t *m, int32_t delta, bool wrap);

// The group an item belongs to for letter jumps, from its text: 'A'..'Z' for
// a first character that is a letter (either case), '#' for one that sorts
// before the letters (digits, most punctuation, an empty text), '~' for one
// that sorts after them. These groups come in this order in every list that
// is sorted the library's way.
char menu_group(const char *text);

// The group of the item at `index`.
typedef char (*menu_group_fn)(void *ctx, uint32_t index);

// Jump to the first item of the next group (direction > 0), or back to the
// first item of this group, or of the previous group if already there
// (direction < 0). The list must be sorted by group. Calls `group` O(log n)
// times.
void menu_jump_group(menu_t *m, int direction, menu_group_fn group, void *ctx);

#define MENU_HEADER_WIDTH 16

// The top row: `name` on the left and "cursor/count" (counting from 1) on the
// right, with at least one space between; the name is cut short if it must
// be. An empty list shows the name only. Writes exactly MENU_HEADER_WIDTH
// characters, no terminator.
void menu_header(const char *name, const menu_t *m, char out[MENU_HEADER_WIDTH]);
```

Required tests (binding names):

`test_reset_puts_the_cursor_first`, `test_move_down_and_up`, `test_single_step_wraps_at_both_ends_when_asked`, `test_single_step_stops_at_the_ends_when_not_wrapping`, `test_big_moves_never_wrap` (delta of +10 near the end stops at the last item even with `wrap` true), `test_move_in_an_empty_list_does_nothing`, `test_move_in_a_one_item_list`, `test_set_count_keeps_or_clamps_the_cursor`, `test_group_of_text` (`"Radiohead"` R, `"radiohead"` R, `"2Pac"` #, `""` #, `"(What's the Story)"` #, `"~tilde"` ~, `"_under"` and `"[bracket"`: whichever side of the letters they sort on in the library's order, which folds A-Z to a-z and then compares bytes; state the rule in a comment), `test_jump_forward_lands_on_the_first_item_of_the_next_group`, `test_jump_forward_from_the_last_group_stays`, `test_jump_back_goes_to_the_start_of_this_group_first`, `test_jump_back_from_a_group_start_goes_to_the_previous_group`, `test_jump_back_from_the_first_item_stays`, `test_jump_calls_group_a_logarithmic_number_of_times` (a 5,000-item list: no more than 40 calls per jump), `test_jump_in_empty_and_one_item_lists`, `test_header_short_name` (`Artists`, 14th of 87: `Artists    14/87`), `test_header_first_and_last`, `test_header_large_numbers` (`Songs`, 1028 of 1028: `Songs  1028/1028`), `test_header_cuts_a_long_name` (a 30-character name, 3 of 10: 11 characters of the name, a space, `3/10`), `test_header_when_position_leaves_no_room_for_a_name` (the position wins; at least one character of name or none, but never an overflow), `test_header_empty_list` (name only, padded).

- [ ] **Step 1:** Header, stub, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement. Pass, nothing on stderr. Add to the firmware build; no warnings.
- [ ] **Step 3:** Commit: `Add menu: list cursor, header text and letter jumps`.

---

### Task 2: `tracklist` and `browse`

**Files:**
- Create: `components/dap_core/tracklist.h`, `tracklist.c`, `browse.h`, `browse.c`
- Test: `test/host/test_tracklist.c`, `test/host/test_browse.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Consumes: `menu` (Task 1); `library.h` (stage 4).
- Produces (`tracklist.h`, binding):

```c
#pragma once

#include "library.h"
#include <stdint.h>

// A list of tracks to play in order, described by what it is rather than by
// its contents: the index already holds every such list as a range.

typedef enum {
  TRACKLIST_NONE,
  TRACKLIST_ALBUM,  // id = album number: its tracks in album order
  TRACKLIST_ARTIST, // id = artist number: all the artist's tracks
  TRACKLIST_SONGS,  // every track, A to Z by title
} tracklist_kind_t;

typedef struct {
  tracklist_kind_t kind;
  uint32_t id;
} tracklist_t;

// How many tracks the list has in this index. 0 for TRACKLIST_NONE, an id
// out of range, or a NULL library.
uint32_t tracklist_count(const library_t *lib, tracklist_t list);

// The track number at position `index`, or UINT32_MAX if there is none.
uint32_t tracklist_track(const library_t *lib, tracklist_t list,
                         uint32_t index);
```

- Produces (`browse.h`, binding):

```c
#pragma once

#include "library.h"
#include "menu.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The menu tree. It is a stack of levels, each a list with its own cursor,
// so that going back returns to where the user was. It holds numbers, never
// text or pointers: every call that needs the index is given it, and may be
// given NULL when there is none.

#define BROWSE_DEPTH_MAX 5

typedef enum {
  BROWSE_MAIN,         // Now Playing, Artists, Albums, Songs, Settings
  BROWSE_ARTISTS,      // every artist, A to Z
  BROWSE_ARTIST,       // id = artist: "All songs", then the artist's albums
  BROWSE_ARTIST_SONGS, // id = artist: all the artist's tracks
  BROWSE_ALBUMS,       // every album, A to Z
  BROWSE_ALBUM,        // id = album: its tracks
  BROWSE_SONGS,        // every song, A to Z
  BROWSE_SETTINGS,     // Rescan library, Keypad test, About
} browse_screen_t;

typedef struct {
  browse_screen_t screen;
  uint32_t id;
  menu_t menu;
} browse_level_t;

typedef struct {
  browse_level_t levels[BROWSE_DEPTH_MAX];
  int depth; // index of the current level
} browse_t;

typedef enum {
  BROWSE_NOTHING,     // nothing to do (an empty list, an item with no effect)
  BROWSE_ENTERED,     // a new level is now current
  BROWSE_PLAY,        // play `list` starting at position `index`
  BROWSE_NOW_PLAYING, // show the Now Playing screen
  BROWSE_RESCAN,
  BROWSE_KEYTEST,
  BROWSE_ABOUT,
} browse_action_t;

typedef struct {
  browse_action_t action;
  tracklist_t list;
  uint32_t index;
} browse_result_t;

// Start at the main menu.
void browse_init(browse_t *b);

// Bring the current level's item count up to date with `lib`. Call after
// anything that could have changed it. With a NULL library every library
// list is empty.
void browse_refresh(browse_t *b, const library_t *lib);

// The index has been replaced: numbers on the stack no longer mean what they
// did. Go back to the main menu (its cursor is kept).
void browse_library_changed(browse_t *b);

// The current level's menu, to move its cursor.
menu_t *browse_menu(browse_t *b);

// The current level's name for the top row: "Main", "Artists", an artist's
// or an album's name, "Albums", "Songs", "Settings". Always terminated.
void browse_name(const browse_t *b, const library_t *lib, char *out,
                 size_t out_size);

// The text of item `index` of the current level. Empty if there is none.
void browse_item(const browse_t *b, const library_t *lib, uint32_t index,
                 char *out, size_t out_size);

// Whether the current level is sorted A to Z, so letter jumps make sense.
bool browse_is_alphabetical(const browse_t *b);

// The letter group of item `index` of the current level (see menu_group).
char browse_group(const browse_t *b, const library_t *lib, uint32_t index);

// Open or act on the item under the cursor.
browse_result_t browse_select(browse_t *b, const library_t *lib);

// Go back one level. False at the main menu.
bool browse_back(browse_t *b);
```

Required behaviour:
- **Main menu:** `Now Playing`, `Artists`, `Albums`, `Songs`, `Settings`, in that order. **Settings:** `Rescan library`, `Keypad test`, `About`.
- **An artist's level** lists `All songs` first, then its albums A to Z. Selecting `All songs` enters `BROWSE_ARTIST_SONGS`.
- **Selecting a track** (in an album, an artist's songs, or Songs) returns `BROWSE_PLAY` with the list it is in and its position; the level does not change.
- Items come straight from the index: an album level's item `i` is track `first + i`, Albums' item `i` is `library_album_by_name(i)`, Songs' is `library_song_by_title(i)`. No loop over tracks anywhere.
- Alphabetical levels: Artists, Albums, Songs, and an artist's albums apart from its first row. Album tracks and an artist's songs are not.
- With a NULL library: Main and Settings work; every library list has no items.
- Going deeper than `BROWSE_DEPTH_MAX` cannot happen with this tree; assert it in a test by walking the deepest path.
- Each level remembers its cursor: enter Artists, move to the 5th, enter it, go back: the cursor is on the 5th.

Tests build their library in a static arena with `library_builder_*` from a fixed set of tracks (use the 16 tracks of `test/host/fixtures/lib/README.md`, typed into the test; no file access needed).

Required tests (binding names):
- `test_tracklist.c`: `test_none_and_null_are_empty`, `test_album_tracks_in_album_order`, `test_artist_tracks_are_all_its_albums_in_order`, `test_songs_are_every_track_by_title`, `test_out_of_range_ids_and_positions`.
- `test_browse.c`: `test_starts_at_the_main_menu`, `test_main_menu_items_and_actions`, `test_settings_items_and_actions`, `test_artists_are_listed_a_to_z`, `test_an_artist_lists_all_songs_then_its_albums`, `test_all_songs_of_an_artist`, `test_album_tracks_are_in_album_order`, `test_albums_and_songs_lists`, `test_selecting_a_track_asks_to_play_its_list_from_that_position` (from an album, from an artist's songs, from Songs), `test_back_returns_to_the_same_cursor`, `test_back_at_the_main_menu_is_refused`, `test_names_for_the_top_row` (each level, including an artist's and an album's name), `test_alphabetical_levels`, `test_groups_follow_the_items`, `test_null_library_leaves_main_and_settings_working`, `test_library_changed_returns_to_main`, `test_refresh_after_a_smaller_library_clamps_the_cursor`, `test_long_item_text_is_cut_to_the_buffer`, `test_the_deepest_path_fits`.

- [ ] **Step 1:** Headers, stubs, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement both. Pass, nothing on stderr. Add to the firmware build; no warnings.
- [ ] **Step 3:** Commit: `Add tracklist and browse: the menu tree over the library`.

---

### Task 3: Play numbers on player events; `hold` console command

**Files:**
- Modify: `main/player.h`, `main/player.c`, `main/app_main.c` (callers), `main/debug_console.c/.h`

**Interfaces:**
- Produces (`player.h`, replacing the current declarations of these two items; everything else in the header is unchanged):

```c
typedef struct {
  player_event_type_t type;
  uint32_t play_id; // which player_play call this event belongs to
  uint32_t position_ms;
  uint32_t duration_ms; // 0 if unknown
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;
  player_error_t error;
} player_event_t;

// Queue a track. `play_id`, if not NULL, receives the number that every
// event about this track will carry. Numbers are never 0 and never repeat
// within a run.
esp_err_t player_play(const char *path, uint32_t start_ms, uint32_t *play_id);
```

Requirements:
- The number is assigned in `player_play` itself, before the command is queued, so the caller has it before any event about that track can arrive. `player_play` may be called from more than one task (the UI task and the console task): numbers must still be unique.
- Every `STARTED`, `POSITION`, `ENDED` and `ERROR` event carries the number of the `player_play` call it is about. A seek within a track keeps the track's number.
- No change to how audio flows. The decode and output loops are untouched; the number travels with the existing messages.
- **`hold <char> <repeats>`** console command: posts a press, a long press, `<repeats>` repeat events, and a release for that key to the key queue, spaced so that the UI sees them as it would from a held key (the repeat events may be posted back to back; say what you chose). Cap `<repeats>` at 2000.
- Update every caller and keep the current behaviour of the firmware otherwise (it still plays the first FLAC at boot until Task 4 changes that).

Board checks:
1. `play t01`: its `STARTED`, `POSITION` and `ENDED` log lines are unchanged in format (the binding log formats do not gain a field), frames exact.
2. The race: `play /sdcard/lib1000/Artist 00/Album 00/01 Track.flac` (0.1 s long) and, timed to arrive as it ends, `play t01`. Add `play_id` to the UI task's DEBUG-level logging or to `stat` so the script can show that the `ENDED` of the first track carried the first number and was not attributed to `t01`. Repeat with several timings (for example 0, 50, 100, 150 ms after the first `play`).
3. The music track start to end: `ENDED frames=13453440 expected=13453440`, no `UNDERRUN`.

- [ ] **Step 1:** Implement; build without warnings; host tests still pass.
- [ ] **Step 2:** Board checks. Commit: `Player events carry a play number; add the hold console command`.

---

### Task 4: The UI task in `ui.c`: menus

**Files:**
- Create: `main/ui.h`, `main/ui.c`
- Modify: `main/app_main.c`, `main/CMakeLists.txt`, `main/debug_console.c` if the `screen` hand-over moves

**Interfaces:**
- Consumes: `menu`, `browse`, `tracklist` (Tasks 1, 2); `screen`, `marquee`, `keys`, `keytest`, `translit` (earlier stages); `lcd`, `keypad`; `library_task` (stage 4); `player` (Task 3).
- Produces: `esp_err_t ui_start(...)` creating the UI task and owning its queues (define the signature to fit what `app_main` has to pass in), and `app_main.c` reduced to start-up: display and keys, player, console, library task, wait for the card, load or scan the library. **Nothing plays automatically at boot any more**: the main menu is shown.

Move the existing UI task out of `app_main.c` into `ui.c` first, with no change in behaviour, and confirm it on the board; then build the menus on it.

Required behaviour:
- **Start-up:** `Insert SD card` while waiting for the card, `Scanning...` with the count during a scan (both exist), then the main menu.
- **Lists:** top row from `menu_header` with `browse_name`; bottom row `>` and the highlighted item, through the marquee in the remaining 15 columns when it is longer. The marquee restarts when the cursor moves or the level changes.
- **Keys in lists:** `2`/`8` move up/down on press and on repeat; a press wraps at the ends, a repeat stops there. `6` or `5` selects. `4` or `*` goes back; at the main menu it does nothing. `1`/`3` jump to the previous/next letter group on alphabetical levels and move by 10 on the others.
- **An empty library list** shows the list's name on the top row and `(empty)` on the bottom. While there is no index (a scan running or none loaded) and the user is in a library list, the same applies; when a new index is published the menus go to the main menu (`browse_library_changed`).
- **Settings:** `Rescan library` starts a scan (the scanning screen shows until it finishes, then the main menu). `Keypad test` shows the existing key test screen; a long press of `*` leaves it. `About` shows two rows of facts (tracks in the library; free space on the card or the firmware's build date: choose two that fit, and say which); any key leaves it.
- **Global keys on every screen**, including Settings, About, the scanning screen and the waiting screen: `A`/`B` change the volume (press and repeat) and show the volume bar on the bottom row for 1.5 seconds: a bar of the LCD's custom characters and the level's number, in 16 columns; `D` toggles pause if something is playing or paused; `C` switches between the menus and Now Playing (Now Playing itself is Task 5: for this task `C` and the main menu's `Now Playing` item may show the existing playing screen). On the key test screen every key is shown instead of acted on, except the long `*` that leaves it.
- **Selecting a track** in this task: start it with `player_play` (the list following on is Task 5).
- **Efficiency:** each key press takes the index at most once (get, compute, put) and each redraw at most once. All waiting key events are applied before one redraw. The item under the cursor is fetched once per cursor position, not once per marquee step: keep a copy.
- **Custom characters:** define what the screens need with `lcd_define_char` (a play triangle, a pause sign, a full and an empty bar cell; at most eight in all). The key test screen's glyph must still work.

Board checks (script key presses with `key`/`long`/`hold`, read screens with `screen`):
1. Boot shows `Main         1/5` and `>Now Playing` (or your exact rendering: quote it). Nothing is playing (`stat` says idle).
2. Walk: `8` to Artists, `6`: `Artists     1/17`, `>Artist 00`. `hold 8 5` moves five down. Press `3` repeatedly and quote each stop: the groups `A`, `B`, `D`, `K`, `Q`, `R`, `U`, `V` for this card's artists. `1` goes back a group.
3. Enter `Radiohead`: `All songs`, `Kid A`, `OK Computer`. Enter `Kid A`: three tracks in the order 1, 2, 10 (quote their titles; the first is 29 characters and must scroll: sample `screen` to show it). `4` twice returns to Artists with the cursor still on `Radiohead`.
4. `Albums`: 108 items; `Greatest Hits` appears twice. `Songs`: 1,028 items; `hold 8 500` ends on item 501 (Review Focus 6: the position must be exact), and the time from the command to the final screen is reported.
5. Wrap: at the first item `2` goes to the last; a held `2` from item 3 stops at the first.
6. Select `/lib/` `Kid A` track 2 through the menus: `STARTED path=/sdcard/lib/Radiohead/Kid A/02 Kid A.flac` and `ENDED frames=4410 expected=4410`.
7. Rescan from deep in the tree (Review Focus 1): three levels down, send `scan`: the scanning screen shows, then the main menu; no stale names, no crash. Also Settings → `Rescan library` by keys.
8. Global keys (Review Focus 7): `A`, `B`, `D`, `C` on a list, in Settings, on About, during a scan. Quote the volume bar row.
9. Keypad test from Settings, entered and left by keys.
10. The LCD traffic check: over 20 s sitting on a list whose item fits, the UI task makes no LCD updates at all; on one whose item scrolls, about one per marquee step.

- [ ] **Step 1:** Move the UI task to `ui.c` unchanged; build; confirm on the board.
- [ ] **Step 2:** Menus, settings, global keys, volume bar.
- [ ] **Step 3:** Board checks. Commit: `UI: menus over the library, settings, global keys and volume bar`.

---

### Task 5: Now Playing, and the list plays on

**Files:**
- Modify: `main/ui.c`, `main/ui.h`

**Interfaces:**
- Consumes: everything from Task 4; play numbers from Task 3; `tracklist`.
- Produces: the Now Playing screen, and playback that continues through the list a track was chosen from.

Required behaviour:
- **Now Playing:** top row `Title - Artist` from the index (the file name if the track is not in it), as a marquee. Bottom row: the play or pause symbol (custom characters), a space, elapsed and total time as `m:ss/m:ss` (hours are not shown: a track of 75 minutes shows `75:00`); the rest of the row is left for stage 6's shuffle and loop marks. When nothing is loaded: top row `Nothing playing`, bottom row empty. `5` and `D` toggle pause; `*` and `C` return to the menus, where the user left them.
- **Selecting a track** starts it and switches to Now Playing.
- **The list plays on:** when the current track ends, the next position of the list it was chosen from starts, until the list's last track ends; then playback stops and Now Playing shows the last track, stopped. The list is remembered as a `tracklist_t` and a position, together with the index generation it belongs to: if the index has been replaced since, playback stops at the end of the current track.
- Only an `ENDED` or `ERROR` event carrying the current play number advances the list (Review Focus 4).
- **A track that will not play:** `Can't play` on the bottom row for 1.5 seconds (the spec's wording), then the next track starts. After three failures in a row playback stops (Review Focus 5). A successful `STARTED` resets the count.
- **`D` when nothing is playing** but a track is remembered (the list ended, or playback was stopped by a scan): start that track again from its beginning. With nothing remembered it does nothing.
- The marquee, the clock and the volume bar keep the efficiency rules: one LCD update per change, no polling.

Board checks:
1. Select `Kid A` track 1 through the menus: Now Playing shows `Everything In Its Right Place - Radiohead` scrolling and a running time; then tracks 2 and 10 follow by themselves (three `STARTED` lines in album order, each with frames equal to expected), then it stops, showing the last track.
2. Select the first of an artist's `All songs` on `/lib1000/` (`Artist 03`): 100 tracks play in order (they are 0.1 s each): count the `STARTED` lines and check the order of the first ten paths.
3. Songs → a track: the following tracks come in title order.
4. `Unknown Artist` → `All songs` contains `/lib/loose/text.mp3` and `/lib/loose/empty.flac` among playable tones: `Can't play` appears (quote `screen`), playback moves on; and a list arranged to give three failures in a row stops (use the console to build the situation if the card's order does not: say how).
5. The race (Review Focus 4): while a 0.1 s track is ending, select another track by keys; exactly one track is started afterwards, and the list position is the selected one. Repeat ten times with varied timing.
6. Pause with `D` on a menu screen, resume with `5` on Now Playing; `C` there and back keeps the menu position; the volume bar appears over Now Playing and goes after 1.5 s (sample `screen`).
7. `scan` while playing from a list: playback stops; after the scan `D` restarts the remembered track if it is still in the index.
8. Review Focus 8: start the music track from the menus, then for its whole length have a script scroll the Songs list (`hold 8 200`, `hold 2 200`, letter jumps, entering and leaving levels) every few seconds: `ENDED frames=13453440 expected=13453440`, `underruns=0`.
9. `stat`: the UI task's stack high-water mark after all of the above (add it to `stat` if it is not there).

- [ ] **Step 1:** Now Playing and list continuation.
- [ ] **Step 2:** Board checks. Commit: `UI: Now Playing screen; a selected track's list plays on`.

---

## Stage 5 hardware check (run by the user)

On the phone charger.

1. **Find and play a track by artist:** Artists → an artist → an album → a track. It plays, and the next tracks of the album follow.
2. **Lists feel right:** `2` and `8` move; holding scrolls quickly without overshooting when you let go; `1` and `3` jump between letters; `4` or `*` goes back to where you were.
3. **Long names scroll** on the bottom row of a list and on Now Playing.
4. **Now Playing** shows the title and artist, a play or pause symbol and a running time; `C` jumps between it and the menus.
5. **Volume** with `A`/`B` works on every screen and shows a bar that goes away.
6. **Pause** with `D` works on every screen.
7. **Songs** (the long list) is quick to move through.
8. **Settings:** Rescan library shows the scanning screen and returns; Keypad test works and a long `*` leaves it; About shows its facts.
9. **Browsing while music plays** causes no clicks or stutters.
