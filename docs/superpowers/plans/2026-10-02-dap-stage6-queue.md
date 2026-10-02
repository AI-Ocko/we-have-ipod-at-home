# DAP Stage 6 (Queue, Shuffle, Loop, Resume) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the player a real play queue: previous and next, shuffle, loop, "Play next" and "Add to queue" from an options menu, and remembering its settings, its queue and its place across power-off.

**Architecture:** The queue's rules are hardware-free and tested on the host: `play_queue` holds the play order and the original order as track numbers in caller-supplied arrays and implements every rule of spec section 7; `options` says which actions the `#` key offers for what is under the cursor. On the device the UI replaces stage 5's simple "list plays on" with the queue. A low-priority state task does all the saving (settings and position in NVS, the queue in a small file on the card), so the UI task never waits on flash or the card.

**Tech Stack:** ESP-IDF v6.0.2 (C), NVS, FreeRTOS, Unity host tests under AddressSanitizer.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read section 3 "Performance rules", section 7 in full, section 9 "Keys", "Screens" and "Options menus", section 10, section 11, and stage 6 of section 14).

**How this plan works:** it fixes interfaces, behaviour, test cases and acceptance checks; implementation bodies and test code are the implementer's. Headers given here are binding. If an interface cannot be implemented as written, or forces an inefficiency, stop and report NEEDS_CONTEXT; do not change it silently.

## Scope

In this stage: the play queue and its rules; `4`/`6` previous/next and `1`/`3` shuffle/loop on Now Playing, with their marks on the bottom row; the `#` options menu in lists (Play, Play next, Add to queue for a track; Play, Shuffle play, Add to queue for an album or an artist); saving and restoring volume, shuffle, loop, the queue and the place in the current track; opening Now Playing paused on the remembered track at power-on.

Not in this stage (do not build): playlists, "Add to playlist", "Save queue as playlist", text entry (stage 7); backlight timeout, sleep timer, fast-forward and rewind by holding `4`/`6` (stage 8). `#` on Now Playing does nothing yet. There is no screen that lists the queue.

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port the SD card sometimes fails to mount; the firmware shows `Insert SD card` and retries every 2 seconds. If the retries go on for more than a minute, STOP and report BLOCKED: only the user can re-insert the card.
- **Library reader rule** (`main/library_task.h`, binding): `library_task_get()` may return NULL; every non-NULL get is followed promptly by `library_task_put()` in the same task; no pointer into the index survives a put; numbers are valid only within one index.
- **The queue rules (spec section 7), binding:**
  - Selecting a track in a list replaces the queue with that whole list and starts at the chosen track.
  - Play next inserts after the current track. Add to queue appends.
  - Shuffle on: the order of everything except the current track is randomised and the current track becomes first. Shuffle off: the original order returns and the current track stays current.
  - Next at the end: with loop all it wraps to the start; with loop off playback stops and the queue stays.
  - Loop one repeats the current track when it ends by itself; a manual Next still advances.
  - Previous: more than 3 seconds into the track restarts it; otherwise it goes to the track before (wrapping only when loop all is on; at the first track with no wrap it restarts).
  - A track that fails is skipped; three failures in a row stop playback.
- **Keys on Now Playing (spec section 9):** `4` previous, `6` next, both acting on **release**, and only if no long press happened in between (holding them is reserved for seeking in stage 8). `1` toggles shuffle. `3` cycles loop: off, all, one. `5` and `D` play/pause, `*` and `C` to the menus, as now.
- **Bottom row of Now Playing:** the play or pause symbol, the time, and at the right end the shuffle mark (`S`) and the loop mark (a custom character for loop all, another for loop one), each shown only when on, as in the spec's `> 1:23/4:11  S L`.
- **Options menu (spec section 9):** `#` on a list item opens a short list of actions for it; `5`/`6` chooses, `4`/`*` or `#` closes it. Tracks: `Play`, `Play next`, `Add to queue`. Albums and artists: `Play`, `Shuffle play`, `Add to queue`. On rows that are not a track, album or artist (the main menu, Settings, `All songs`) `#` does nothing.
- **Saving (spec section 10, as amended by this plan):** volume, shuffle and loop are saved in NVS when they change. The queue is saved to `/.dap/queue.bin` on the card when it changes: the two orders as track numbers, the position, and the checksum of the index they belong to. The position in the queue and the elapsed time are saved to NVS on a track change, on pause, and every 30 seconds while playing. At power-on, if the saved queue belongs to the index that was loaded, the queue is restored and Now Playing opens **paused** on the remembered track at the saved time; no sound is made until the user presses play. If it does not, the queue starts empty.
- **The UI task never waits on flash or the card.** All saving and loading of state happens in a separate low-priority task. Nothing saved may be half-written in a way a later boot would trust.
- **Performance rules (spec section 3):** shuffle is a Fisher-Yates shuffle, O(n). Next, previous, play next and append are O(1) apart from the unavoidable move of array elements for an insert. Filling the queue from a list is one pass over that list. No allocation in the UI loop; no polling.
- **Player facts that must not be disturbed** (`main/player.c`): the decode loop's stall guard, `eos` handling, the registered FLAC parser wrapper, the pinned codec version, the two-task design, play numbers. This stage does not modify `player.c`.
- Every FLAC that plays from start to end must still log `ENDED frames=X expected=X` (tag `player`). Audio must not notice saving: no `UNDERRUN` while state is being written.
- `components/dap_core` is pure C using only the C standard library. Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan and must not allocate from the heap.
- No compiler warnings from `main/` or `components/`. Nothing that can fail at run time because of a file, the card or flash may use `ESP_ERROR_CHECK`, abort or restart.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Agents cannot hear the output, see the LCD or press keys. Console: `key <char>`, `long <char>`, `hold <char> <n>` inject keys (a held key is one press, one long, N repeats, one release); `screen` prints what the LCD shows; `stat`, `lib ...`, `play`, `pause`, `resume`, `stop`, `seek`, `scan`. Replies are `dbg: ok ...` / `dbg: err ...`. Anything only a person can judge goes in the hardware check.
- On the card: `/music/` (two real tracks; `16 - Kanye West - Hey Mama.flac` is 13453440 frames, 305 s), `/test/` (untagged tones; `t01_44k_16_st.flac` is 10 s, `t06_tone_30s.flac` is 30 s), `/lib/` (14 tagged 0.1 s tracks plus two that cannot play), `/lib1000/` (1,000 tracks of 0.1 s: 10 artists x 10 albums x 10). The index has 1,028 tracks.

## Review Focus

1. **Shuffle must be fair and reversible.** Every order equally likely (no track stuck in place, no bias from a sloppy swap range); turning it off returns exactly the original order with the same track current; turning it on, off and on again never loses or duplicates a track. Pinned in Task 1 by a statistical test and by round-trip tests.
2. **Edits while shuffled.** Play next and Add to queue while shuffle is on must show up in both orders so that shuffle-off does not lose them. Pinned in Task 1.
3. **The ends of the queue and an empty queue:** next at the last track with each loop mode; previous at the first; every operation on an empty queue and on a queue of one. Pinned in Task 1.
4. **A full queue.** Adding 1,028 tracks to a queue that is nearly full must add what fits or nothing, say so, and leave the queue consistent. Pinned in Task 1 and shown on the board in Task 4.
5. **Power pulled at any moment.** A half-written queue file, settings saved but the queue not, a queue saved for an index that has since been rebuilt differently: the next boot must end up with either the right state or an empty queue, never a queue pointing at the wrong tracks. Pinned in Task 1 (rejecting damaged or mismatched files) and Task 3 (constructed cases on the board).
6. **The index is rebuilt while a queue exists.** A rescan that produces the same index keeps the queue; one that produces a different index empties it rather than play wrong tracks. Pinned in Task 4.
7. **Saving must not be heard.** NVS commits and the queue file being written while music plays. Pinned in Task 3 by forcing hundreds of saves during playback: exact frame count, no underrun.
8. **Keys on release.** A long press of `4` or `6` must not also skip a track when released; a quick tap must. Pinned in Task 4.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/play_queue.c/.h` | The queue: orders, position, shuffle, loop, every rule of spec section 7; save and restore | 1 |
| `components/dap_core/options.c/.h` | What the `#` key offers for a track, an album, an artist | 2 |
| `components/dap_core/library.c/.h` | One accessor: the index's checksum | 1 |
| `main/state_task.c/.h` | Saving and loading settings, position and the queue, off the UI task | 3 |
| `main/ui*.c`, `main/ui_state.h` | The queue in the UI; previous/next/shuffle/loop; options menu; resume | 4 |
| `main/debug_console.c/.h` | `queue` and `state` commands | 3, 4 |

---

### Task 1: `play_queue`

**Files:**
- Create: `components/dap_core/play_queue.h`, `components/dap_core/play_queue.c`
- Test: `test/host/test_play_queue.c`
- Modify: `components/dap_core/library.h`, `library.c`, `test/host/test_library.c` (the checksum accessor), `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Produces in `library.h`: `uint32_t library_checksum(const library_t *lib);` — the CRC-32 stored in the index's header, identifying this exact index; 0 for a NULL library.
- Produces (`play_queue.h`, binding):

```c
#pragma once

#include "library.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// The play queue: which tracks will play and in what order. Tracks are held
// as track numbers of one library index. The queue keeps two orders: the
// order tracks were put in, and the order they play in, which is the same
// unless shuffle is on.

#define PLAY_QUEUE_NONE UINT32_MAX

typedef enum {
  PLAY_LOOP_OFF,
  PLAY_LOOP_ALL,
  PLAY_LOOP_ONE,
} play_loop_t;

typedef struct {
  uint16_t *play;     // play order: `count` track numbers
  uint16_t *original; // the order they were added in
  uint32_t capacity;  // entries each array can hold
  uint32_t count;
  uint32_t position;  // in `play`; meaningless when count is 0
  bool shuffle;
  play_loop_t loop;
  uint32_t random;    // generator state; never 0
} play_queue_t;

// An empty queue over two caller-supplied arrays of `capacity` entries each.
// `seed` starts the random generator (any value; 0 is replaced).
void play_queue_init(play_queue_t *q, uint16_t *play, uint16_t *original,
                     uint32_t capacity, uint32_t seed);

void play_queue_clear(play_queue_t *q);

// Replace the queue with `list`, current at its position `start`. If shuffle
// is on the rest is shuffled and the chosen track is first. Returns how many
// tracks were taken (fewer than the list has if it is longer than the
// capacity; the chosen track is always among them).
uint32_t play_queue_load(play_queue_t *q, const library_t *lib,
                         tracklist_t list, uint32_t start);

// Add after the current track / at the end. With an empty queue either makes
// the track current. Return how many were added: all of `list` or none
// (false / 0 if they do not fit).
bool play_queue_insert_next(play_queue_t *q, uint32_t track);
bool play_queue_append(play_queue_t *q, uint32_t track);
uint32_t play_queue_append_list(play_queue_t *q, const library_t *lib,
                                tracklist_t list);

// The current track, or PLAY_QUEUE_NONE if the queue is empty.
uint32_t play_queue_current(const play_queue_t *q);

// Move on. `ended` is true when the current track finished by itself and
// false when the user asked for the next one. Returns the track to play, or
// PLAY_QUEUE_NONE if playback should stop (the position then stays on the
// last track).
uint32_t play_queue_next(play_queue_t *q, bool ended);

// Go back. `elapsed_ms` is how far into the current track playback is.
// Returns the track to play from its beginning: the current one again, or
// the one before. PLAY_QUEUE_NONE only for an empty queue.
uint32_t play_queue_previous(play_queue_t *q, uint32_t elapsed_ms);

void play_queue_set_shuffle(play_queue_t *q, bool on);
void play_queue_set_loop(play_queue_t *q, play_loop_t loop);

// Save to, or restore from, an open file. A restore succeeds only if the file
// is whole, was saved for the index whose checksum and track count are given,
// fits the arrays, and holds only track numbers that index has. On failure
// the queue is left empty. Shuffle and loop are part of what is saved.
bool play_queue_save(const play_queue_t *q, uint32_t index_checksum,
                     FILE *f);
bool play_queue_restore(play_queue_t *q, uint32_t index_checksum,
                        uint32_t index_tracks, FILE *f);
```

Required properties:
- Nothing allocates; no global state; several queues can coexist. The random generator is a small integer one (for example xorshift32), deterministic from the seed, so tests are repeatable.
- Shuffle uses Fisher-Yates over the tracks other than the current one. No modulo bias worth the name: say how you pick a number below n.
- Shuffle off finds the current track's place in the original order. If a track appears more than once in the queue (it was added twice), the result must still be a valid position on that track; say which occurrence.
- An insert or append while shuffle is on updates both orders: `insert_next` puts the track after the current one in both; `append` puts it last in both.
- The saved file: a header with a magic, a version, the index checksum, the count, the position, shuffle, loop and a CRC-32 of everything after the CRC field; then the two arrays. Little-endian, no padding surprises, the same bytes on the 64-bit host and the 32-bit device. `play_queue_restore` checks all of it, and that every track number is below `index_tracks` and the position below the count.

Required tests (binding names):

`test_empty_queue_answers_none`, `test_load_from_an_album_starts_at_the_chosen_track`, `test_load_takes_what_fits_and_keeps_the_chosen_track`, `test_next_walks_the_queue`, `test_next_at_the_end_with_loop_off_stops_and_stays`, `test_next_at_the_end_with_loop_all_wraps`, `test_loop_one_repeats_when_the_track_ends`, `test_loop_one_still_advances_on_a_manual_next`, `test_previous_restarts_after_three_seconds`, `test_previous_within_three_seconds_goes_back`, `test_previous_at_the_first_track_restarts_without_loop_and_wraps_with_loop_all`, `test_insert_next_goes_after_the_current_track`, `test_append_goes_last`, `test_insert_and_append_into_an_empty_queue_make_it_current`, `test_append_list_adds_all_or_nothing`, `test_full_queue_refuses_and_stays_consistent`, `test_shuffle_keeps_the_current_track_first_and_every_track_once`, `test_shuffle_off_restores_the_original_order_and_the_current_track`, `test_shuffle_on_off_on_never_loses_a_track`, `test_shuffle_is_fair` (many shuffles of a short queue from different seeds: each of the other tracks lands in each place about equally often, within a stated tolerance), `test_shuffle_of_one_and_two_tracks`, `test_load_while_shuffle_is_on_shuffles_the_rest`, `test_edits_while_shuffled_survive_shuffle_off`, `test_duplicate_tracks_are_handled`, `test_same_seed_same_order`, `test_save_and_restore_round_trip` (including shuffle on, loop one, and a position in the middle), `test_restore_rejects_another_index` (wrong checksum), `test_restore_rejects_a_track_number_the_index_does_not_have`, `test_restore_rejects_a_truncated_or_damaged_file` (cut at several points; one flipped bit in each of 32 places), `test_restore_rejects_a_queue_larger_than_the_arrays`, `test_failed_restore_leaves_the_queue_empty`, `test_five_thousand_tracks` (load, shuffle, unshuffle, walk to the end: prints the time; checks nothing is lost).

And in `test_library.c`: `test_checksum_identifies_the_index` (two builds of the same tracks give the same checksum; a different library gives a different one; NULL gives 0).

- [ ] **Step 1:** Headers, stubs, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement. Pass, nothing on stderr. Add to the firmware build; no warnings.
- [ ] **Step 3:** Commit: `Add play_queue: order, shuffle, loop, previous/next rules, save and restore`.

---

### Task 2: `options`

**Files:**
- Create: `components/dap_core/options.h`, `components/dap_core/options.c`
- Test: `test/host/test_options.c`
- Modify: `components/dap_core/browse.h`, `browse.c`, `test/host/test_browse.c` (one new function), CMake files

**Interfaces:**
- Produces in `browse.h`:

```c
typedef enum {
  BROWSE_TARGET_NONE,   // nothing the options menu applies to
  BROWSE_TARGET_TRACK,  // `list` and `index` say which
  BROWSE_TARGET_ALBUM,  // `list` is the album
  BROWSE_TARGET_ARTIST, // `list` is all the artist's tracks
} browse_target_kind_t;

typedef struct {
  browse_target_kind_t kind;
  tracklist_t list;
  uint32_t index; // for a track: its position in `list`
} browse_target_t;

// What is under the cursor, for the options menu. Does not change the level.
browse_target_t browse_target(const browse_t *b, const library_t *lib);
```

- Produces (`options.h`, binding):

```c
#pragma once

#include "browse.h"
#include <stdint.h>

typedef enum {
  OPTION_PLAY,
  OPTION_PLAY_NEXT,
  OPTION_ADD_TO_QUEUE,
  OPTION_SHUFFLE_PLAY,
} option_t;

#define OPTIONS_MAX 4

// The actions offered for a target, in menu order. Returns how many (0 for
// BROWSE_TARGET_NONE).
uint32_t options_for(browse_target_kind_t kind, option_t out[OPTIONS_MAX]);

// The menu text of an action.
const char *options_text(option_t option);
```

Required behaviour: a track offers `Play`, `Play next`, `Add to queue`; an album or an artist offers `Play`, `Shuffle play`, `Add to queue`. `browse_target` gives: a track row in an album, an artist's songs or Songs (with the list it is in and its position); an album row in Albums or under an artist; an artist row in Artists. `All songs`, the main menu and Settings rows give `BROWSE_TARGET_NONE`, as does an empty list or a stale cursor.

Required tests (binding names): in `test_options.c`: `test_track_options`, `test_album_and_artist_options`, `test_no_options_for_nothing`, `test_texts`. In `test_browse.c`: `test_target_of_a_track_row` (in each of the three kinds of track list), `test_target_of_an_album_row` (in Albums and under an artist), `test_target_of_an_artist_row`, `test_no_target_for_all_songs_main_and_settings`, `test_no_target_with_a_null_library_or_stale_cursor`.

- [ ] **Step 1:** Headers, stubs, tests. Fail. Implement. Pass. Firmware build clean.
- [ ] **Step 2:** Commit: `Add options: what the # key offers; browse says what is under the cursor`.

---

### Task 3: Saving and loading state

**Files:**
- Create: `main/state_task.h`, `main/state_task.c`
- Modify: `main/app_main.c`, `main/CMakeLists.txt`, `main/debug_console.c/.h`, `sdkconfig.defaults` if NVS needs an option

**Interfaces:**
- Consumes: `play_queue_save`, `play_queue_restore`, `library_checksum` (Task 1); `library_task` (stage 4).
- Produces (`state_task.h`; define the exact signatures, document them fully, and keep to these roles):
  - a start function creating a task at priority 2 on core 0;
  - **settings**: `volume`, `shuffle`, `loop`: a call that hands the task the latest values to save, returning at once; and the values loaded at start-up;
  - **position**: queue position and elapsed milliseconds, likewise;
  - **queue**: a call that hands the task a consistent copy of the queue to save, returning at once (the caller's queue may change immediately afterwards), and a call that restores the saved queue into a caller-supplied `play_queue_t` given the current index;
  - everything is safe to call from the UI task without blocking on flash or the card.

Requirements:
- **NVS**: namespace of your choice; initialise NVS at start-up and cope with a partition that needs erasing (first boot, or a version change): erase and carry on with defaults, logged once. A value that has not changed is not written again.
- **The queue file** `/.dap/queue.bin`: written to a temporary name and then put in place, with the same care as the index (`main/library_task.c` documents the pattern and the power-cut analysis: read it and follow it). Written only when the queue's contents changed.
- **Handing over a copy:** the UI's queue arrays change under the user's fingers. The state task must save a snapshot that is consistent in itself. Decide how (for example a second pair of arrays the UI copies into before signalling, with the latest request replacing an unsaved older one) and explain why no torn queue can be written and why the UI's cost is bounded. Say where the arrays live (PSRAM or internal RAM) and how big they are: the queue's capacity is 8,192 tracks.
- **Coalescing:** many changes in quick succession (a held volume key, a burst of "Add to queue") must cost one save, not one per change: wait a short settling time after the last change before writing (say how long), but never lose the final state.
- **Saving must not be heard** (Review Focus 7). Measure it: while the music track plays, force 300 settings saves and 50 queue saves of a full-size queue spread over a minute: `ENDED` frame count exact, `underruns=0`. Report how long an NVS commit and a queue save take (typical and worst), and whether an NVS page erase was seen in the run and how long it took.
- **Console:** `state` prints what is saved (volume, shuffle, loop, queue position, elapsed, and for the queue file: present or not, count, the index checksum it was saved for and whether that matches the loaded index). `state clear` erases all saved state. Temporary commands used for the forced-save test are not committed.
- In this task nothing in the UI changes yet except what is needed to prove saving and loading work: volume is saved and restored (the UI already has a volume). The queue and position calls are exercised through temporary console commands or a test hook, not committed.

Board checks:
1. Set the volume to 14 with keys, wait for the save (log line), reset: the volume is 14 and `state` says so. A reset immediately after a change (before the settling time) loses at most that last change, never corrupts.
2. Queue file: save a known queue (temporary command), reset, restore: identical; `state` shows it matches the index.
3. Mismatch: save a queue with a wrong checksum (temporary command): restore refuses and the queue is empty.
4. Power-cut cases for the queue file, constructed as in the library task's analysis: only the temporary file present, both present, a truncated file: each boots to a sane state.
5. First boot with NVS erased (`state clear`, or erase the NVS partition): defaults, no error loop.
6. The forced-save run of Review Focus 7, with the timings.

- [ ] **Step 1:** Implement the task, NVS, queue file, console commands. Build without warnings.
- [ ] **Step 2:** Board checks; remove temporary commands; re-verify a normal boot. Commit: `Save and restore settings, position and the queue off the UI task`.

---

### Task 4: The queue in the UI

**Files:**
- Modify: `main/ui.c`, `main/ui_play.c`, `main/ui_screens.c`, `main/ui_state.h`, `main/ui.h`, `main/debug_console.c/.h`

**Interfaces:**
- Consumes: `play_queue`, `options`, `browse_target` (Tasks 1, 2); `state_task` (Task 3); play numbers (stage 5).
- Produces: the stage's behaviour on the device.

Required behaviour:
- **The queue replaces stage 5's list continuation.** Selecting a track in a list loads the queue from that list at that track and plays it. When a track ends, `play_queue_next(q, true)` decides; when it returns none, playback stops on the last track.
- **Now Playing keys:** `6` next and `4` previous on release with no long press in between (Review Focus 8); previous uses the elapsed time for the 3-second rule; `1` toggles shuffle, `3` cycles loop off, all, one. The bottom row shows the marks. A brief confirmation on the bottom row for 1.5 s when shuffle or loop changes (`Shuffle on`, `Shuffle off`, `Loop off`, `Loop all`, `Loop one`), using the same overlay mechanism as the volume bar.
- **Next and previous when nothing is playing** but the queue has tracks: they move in the queue and start playing.
- **Options menu:** `#` on a track, album or artist row opens it over the list (top row: the item's name, cut to fit; bottom row: `>` and the action; `2`/`8` move, `5`/`6` choose, `4`, `*` or `#` close). `Play` on a track does what selecting it does. `Play` on an album or artist loads the queue with it from its first track. `Shuffle play` turns shuffle on and loads it, starting from a random track. `Play next` and `Add to queue` change the queue and show a confirmation for 1.5 s (`Added` or `Queue full`), leaving playback and the menu where they were; if nothing was playing and the queue was empty, the added track becomes current but does not start by itself.
- **A failing track** is skipped as in stage 5 (`Can't play`, three in a row stop), now through the queue.
- **A new index:** if its checksum equals the one the queue was built for, the queue stays; otherwise the queue is cleared (and playback, which the scan stopped, has nothing remembered).
- **Saving:** every change of volume, shuffle or loop goes to the state task; the queue goes to it whenever its contents or position change; the elapsed time on a track change, on pause, and every 30 seconds while playing (driven by the player's position events, not a new timer).
- **Resume at power-on:** after the library is ready, the saved settings are applied and the queue restored if it matches the index. If the queue has a current track, Now Playing is shown, paused, at the saved time, with the play/pause symbol showing paused and the time showing the saved position; `D` or `5` starts it from there (`player_play(path, start_ms, ...)`). If not, the main menu is shown as now.
- **Console:** `queue` prints the count, the position, shuffle, loop and the next few tracks in play order (cap the listing), in a fixed greppable form documented in `debug_console.h`.
- The queue's arrays (two of 8,192 `uint16_t`, plus whatever the snapshot needs) are allocated once at start-up; say where.
- The UI loop's rules from stage 5 hold: one index take per key press, all waiting events applied before one redraw, no polling, no allocation, nothing blocking.

Board checks:
1. Select `Kid A` track 1: `queue` shows 3 tracks, position 0. `key 6` twice walks to track 10; a third `6` stops (loop off) and stays on the last track. `key 4` within 3 s goes back one; `key 4` after 5 s of `t06` restarts it (position back near 0).
2. Loop: `3` to loop all: next from the last track wraps to the first. `3` again to loop one: let a 0.1 s track end by itself several times: the same path starts each time; a manual `6` advances. `screen` shows the marks changing and the confirmations.
3. Shuffle: load an artist's `All songs` on `/lib1000/` (100 tracks), press `1`: `queue` shows the current track first and 100 distinct tracks; let them play: 100 `STARTED` lines, 100 distinct paths, not in album order. `1` again: the order returns and the current track is unchanged.
4. Options: `#` on a track: `Play next` then `Add to queue` on two other tracks: `queue` shows them in the right places. `#` on an album: `Add to queue` adds its tracks; `Shuffle play` on an artist. `#` on `All songs`, the main menu and Settings rows does nothing. `#` closes the menu.
5. Queue full: add `Songs` (1,028) repeatedly until the queue cannot take a whole list: `Queue full`, nothing half-added (`queue` count unchanged), everything still works.
6. Release semantics (Review Focus 8): `key 6` skips once; `long 6` (press, long, release) does not skip. Same for `4`.
7. Resume: with the music track playing at about 40 s, shuffle on, loop all, volume 12: wait for a 30-second save, reset the board. After boot: `screen` shows Now Playing with the music track's title, the pause symbol and a time within 30 s of where it was; `stat` shows idle (no sound); `queue` and the marks are as before; volume 12. `key D`: `STARTED` and the first `POSITION` is near the saved time. Then repeat with a pause before the reset (the saved time is then exact to within a second).
8. Rescan with a queue (Review Focus 6): `scan` (same card, same index): after it, `queue` is unchanged and `D` resumes. Then force a different index (a temporary build or command that drops one track from the scan, not committed): the queue is cleared.
9. Saving is not heard: the music track from start to end while a script presses `1`, `3`, `A`, `B` and adds to the queue every few seconds: `ENDED frames=13453440 expected=13453440`, `underruns=0`.
10. Stage 5's behaviour still holds: menus, letter jumps, `hold 8 500` on Songs lands on 502 in about the same time as before, `Can't play` handling, the race with play numbers (ten runs).

- [ ] **Step 1:** Queue in the UI, keys, marks, confirmations.
- [ ] **Step 2:** Options menu.
- [ ] **Step 3:** Saving hooks and resume at power-on.
- [ ] **Step 4:** Board checks. Commit (one or several, each buildable): final subject `UI: play queue, shuffle, loop, options menu and resume at power-on`.

---

## Stage 6 hardware check (run by the user)

On the phone charger.

1. **Next and previous** on Now Playing with `6` and `4`: a tap skips; previous restarts the track if it is more than a few seconds in.
2. **Shuffle** with `1`: an `S` appears; the album plays in a mixed order; `1` again returns to album order.
3. **Loop** with `3`: off, all, one, each with its mark; loop one repeats the track; loop all goes round the album.
4. **Options** with `#` on a track: `Play next` and `Add to queue` do what they say. On an album: `Shuffle play`.
5. **Power-off memory:** play something to the middle, set a volume, unplug, plug in again: Now Playing shows the same track, paused, at about the same time; pressing `D` carries on from there; volume, shuffle and loop are as you left them.
6. **Nothing is heard when it saves:** no click every 30 seconds.
