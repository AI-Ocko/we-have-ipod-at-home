# DAP Stage 8 (Finishing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the player: backlight timeout, sleep timer, the About screen, fast-forward and rewind, the items earlier stages deferred to now, an hour of continuous play with no dropouts, and a user guide.

**Architecture:** The timing rules are hardware-free and tested on the host: `standby` (backlight timeout, the key that wakes it, the sleep timer and its fade) and `scrub` (where holding `4`/`6` has got to). `browse` shows the two new settings with their values. On the device the UI applies them; the backlight setting is saved with the other settings; the card's free space for About is read off the UI task. Deferred fixes are made where they live. A soak run proves the whole player over an hour and watches memory.

**Tech Stack:** ESP-IDF v6.0.2 (C), FreeRTOS, NVS, Unity host tests under AddressSanitizer.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md`. Read section 3 ("Performance rules"), section 4 ("Seeking"), section 9 ("Keys", "Screens", "Menu tree", "Backlight and sleep", amended for this stage), section 10, section 11 and stage 8 of section 14. The "Amendments made during implementation" sections of the stage 6 and stage 7 plans (`docs/superpowers/plans/2026-10-02-dap-stage6-queue.md`, `docs/superpowers/plans/2026-10-04-dap-stage7-playlists.md`) describe the queue, saving and playlist rules this stage keeps, and list the deferred items Task 4 fixes.

**How this plan works:** it fixes interfaces, behaviour, test cases and acceptance checks; implementation bodies and test code are the implementer's. Headers given here are binding. If an interface cannot be implemented as written, or forces an inefficiency, stop and report NEEDS_CONTEXT; do not change it silently.

## Scope

In this stage: backlight timeout (15 s, 30 s, 60 s, always on; default 30 s; saved); the sleep timer (off, 15, 30, 60 min; a 10-second fade, then stop and backlight off; not saved); both as rows of Settings whose value `5`/`6` cycles; the About screen (track count, card free space, firmware build date); fast-forward and rewind by holding `6`/`4` on Now Playing; the deferred fixes listed in Task 4; an hour-long soak with a memory watch, fixing what it finds; a user guide.

Not in this stage: battery monitoring (out of scope in the spec); writing playlists as `.m3u8` or reading ANSI playlists (declined: the owner's computer is Linux and the test library is ASCII); a sector cache under FatFs for very large flat folders (declined: album folders of 10 to 20 files scan at 160 tracks a second); SD at 40 MHz (needs a modern card).

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port the SD card sometimes fails to mount; the firmware shows `Insert SD card` and retries every 2 seconds. If the retries go on for more than a minute, STOP and report BLOCKED: only the user can re-insert the card.
- **Library reader rule** (`main/library_task.h`, binding): `library_task_get()` may return NULL; every non-NULL get is followed promptly by `library_task_put()` in the same task; no pointer into the index survives a put; track numbers are valid only within one index, which `library_checksum()` identifies.
- **Backlight and sleep (spec section 9, as amended):**
  - The backlight goes off when no key has been pressed for the chosen time (15 s, 30 s, 60 s), or never (`always on`). Default 30 s. Saved in NVS with the other settings.
  - Any key press while the backlight is off turns it on and is otherwise ignored: that key's long press, repeats and release are ignored too. Nothing else turns it on (a track change, a notice, a scan finishing).
  - The sleep timer is off, 15, 30 or 60 minutes, counted from when it was chosen. When it expires during playback the volume fades to silence over 10 seconds (`STANDBY_FADE_MS`), playback stops as the console's `stop` does (the track stays on Now Playing and `D` resumes it from where it stopped), the volume returns to its setting for next time (the fade is never saved), and the backlight goes off. With nothing playing it just turns the backlight off. A key press during the fade cancels it: the volume returns and the timer is off. After it expires the timer is off. It is not saved.
  - Settings shows them as rows with their values: `Backlight: 30 s` (`15 s`, `60 s`, `on`), `Sleep: off` or `Sleep: 27 min` (minutes left, rounded up). `5`/`6` on a row moves to the next value; for the sleep timer that is the next of off, 15, 30, 60 after the one last chosen, counted from now.
- **About (spec section 9):** the track count, the card's free space and the firmware build date. The free space is read off the UI task.
- **Fast-forward and rewind (spec section 9 "Keys", section 4):** on Now Playing, holding `6` moves a target time forward and holding `4` back: 2 s per key repeat for the first 2 s after the long press, 5 s per repeat after that (repeats come every 150 ms, then every 50 ms). The bottom row shows the target time while the key is held, with `>>` or `<<` in place of the play/pause symbol. On release, playback jumps to the target (one seek); a paused track stays paused at the target. The target stops at 0:00 and, when the length is known, at one second before the end. A tap still means previous/next (stage 6: on release, only if no long press happened).
- **Saving (stage 6 rules kept):** the queue goes to the state task only when its contents change; a seek or a stop saves the position in NVS like any position change.
- **The UI task never waits on the card or flash.**
- **Performance rules (spec section 3):** no polling (the backlight, sleep, fade and commit times are deadlines the UI loop already waits for); no allocation in the UI loop; the LCD is written only where characters change.
- **Memory:** internal data RAM while playing is about 38 KB, largest block about 30 KB. New buffers live in PSRAM and are allocated once at start-up.
- **Player facts that must not be disturbed** (`main/player.c`): the decode loop's stall guard, `eos` handling, the registered FLAC parser wrapper, the pinned codec version, the two-task design, play numbers. Every FLAC that plays from start to end still logs `ENDED frames=X expected=X` (tag `player`). Only Task 5 may change `player.c`, and only to fix what the soak finds.
- `components/dap_core` is pure C using only the C standard library. Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan and must not allocate from the heap.
- No compiler warnings from `main/` or `components/`. Nothing that can fail at run time because of a file, the card or flash may use `ESP_ERROR_CHECK`, abort or restart.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Agents cannot hear the output, see the LCD or press keys. Console: `key <char>`, `long <char>`, `hold <char> <n>` inject keys; `screen` prints what the LCD shows; `stat`, `queue`, `state`, `pl ...`, `lib ...`, `play`, `pause`, `resume`, `stop`, `seek`, `scan`. Replies are `dbg: ok ...` / `dbg: err ...`. Anything only a person can judge goes in the hardware check.
- On the card: `/music/` (two real tracks; `16 - Kanye West - Hey Mama.flac` is 13453440 frames, 305 s), `/test/` (tones of each format), `/lib/`, `/lib1000/`, an empty `/playlists/`. The index has 1,028 tracks.

## Review Focus

1. **The key that wakes the backlight does nothing else.** Pressing `6` on Now Playing with the backlight off must not skip a track on release, and holding it must not fast-forward; pressing `B` must not change the volume. Pinned in Task 1 (`standby`) and Task 2 (board).
2. **The sleep fade is never saved and never left half-done.** The volume after a fade, a cancelled fade, a fade interrupted by a track ending, a stop or a pause, or a reset during the fade, is the user's setting. Pinned in Task 1 and Task 2.
3. **Seeking past either end and on short or unknown-length tracks.** Rewind below 0:00, fast-forward to the end of a 0.1 s track or a 10 s tone, an MP3, a paused track, a track that ends while the key is held. Nothing crashes, no second track starts by itself mid-hold, the position shown matches where playback lands. Pinned in Task 1 (`scrub`) and Task 3.
4. **Memory over many tracks.** The largest free internal block must not shrink as tracks change (stage 7 saw 14 KB after an MP3 then a FLAC). Pinned in Task 5.
5. **Timers across a scan, a card removal and Now Playing/menu switches.** The backlight and sleep timers keep counting; nothing they do blocks or waits on the card. Pinned in Task 2.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/standby.c/.h` | Backlight timeout, wake key, sleep timer, fade | 1 |
| `components/dap_core/scrub.c/.h` | Where holding `4`/`6` has got to | 1 |
| `components/dap_core/browse.c/.h` | Settings rows with values; new actions | 1 |
| `main/ui*.c`, `main/state_task.c/.h`, `main/playlist_task.c/.h`, `components/dap_hw/storage.c/.h` | Backlight, sleep, settings, saved backlight, About with free space | 2 |
| `main/ui*.c` | Fast-forward and rewind | 3 |
| `main/state_task.c`, `main/playlist_task.c`, `main/ui*.c`, `components/dap_core/playlist_catalog.c` | Deferred fixes | 4 |
| `main/player.c` (only if the soak finds a fault), soak notes | Hour-long soak, memory watch | 5 |
| `docs/USER_GUIDE.md`, `README.md` | How to wire, power, flash and use the player | 6 |

---

### Task 1: `standby`, `scrub`, and the Settings rows

**Files:**
- Create: `components/dap_core/standby.h`, `standby.c`, `components/dap_core/scrub.h`, `scrub.c`
- Test: `test/host/test_standby.c`, `test/host/test_scrub.c`
- Modify: `components/dap_core/browse.c/.h`, `test/host/test_browse.c`, CMake files; `main/ui.c` only as far as a warning-free build needs (new browse actions do nothing until Task 2)

**Interfaces:**
- Produces (`standby.h`, binding):

```c
#pragma once

#include "keys.h"
#include <stdbool.h>
#include <stdint.h>

// When the backlight goes off, which key press only wakes it, and the sleep
// timer with its fade. Times are milliseconds from any one clock.

#define STANDBY_FADE_MS 10000

typedef enum {
  STANDBY_BACKLIGHT_15S,
  STANDBY_BACKLIGHT_30S, // the default
  STANDBY_BACKLIGHT_60S,
  STANDBY_BACKLIGHT_ON,  // never goes off
  STANDBY_BACKLIGHT_COUNT,
} standby_backlight_t;

typedef enum {
  STANDBY_SLEEP_OFF,
  STANDBY_SLEEP_15,
  STANDBY_SLEEP_30,
  STANDBY_SLEEP_60,
  STANDBY_SLEEP_COUNT,
} standby_sleep_t;

typedef struct {
  standby_backlight_t backlight;
  bool lit;
  int64_t last_key_ms;
  char swallowing;        // the key whose press woke the backlight, or 0
  standby_sleep_t sleep;  // the value last chosen
  int64_t sleep_at_ms;    // when the timer expires; INT64_MAX when off
  int64_t fade_from_ms;   // when the fade began; INT64_MAX when not fading
} standby_t;

// Backlight on, timer off.
void standby_init(standby_t *s, standby_backlight_t backlight, int64_t now_ms);

// Every key event goes through here first. True if the UI should act on
// it; false if it is the press that woke the backlight or an event of that
// same press (long, repeat, release). Any press also cancels a fade (the
// timer is then off) and counts as activity.
bool standby_key(standby_t *s, char key, key_event_type_t type,
                 int64_t now_ms);

void standby_set_backlight(standby_t *s, standby_backlight_t backlight,
                           int64_t now_ms);

// Choose a value; anything but OFF counts from now.
void standby_set_sleep(standby_t *s, standby_sleep_t sleep, int64_t now_ms);

// The value after the one last chosen, for the Settings row: off, 15, 30,
// 60, off, ...
standby_sleep_t standby_next_sleep(const standby_t *s);

// Minutes left, rounded up; 0 when the timer is off (or fading).
uint32_t standby_sleep_minutes(const standby_t *s, int64_t now_ms);

typedef enum {
  STANDBY_ACT_LIGHT_OFF = 1, // turn the backlight off
  STANDBY_ACT_FADE = 2,      // the fade has begun: apply standby_fade_level
  STANDBY_ACT_STOP = 4,      // the fade is over: stop playback, restore the
                             // volume setting, backlight off
} standby_act_t;

// Move time on. `playing` says whether a track is playing (a timer that
// expires with nothing playing just turns the backlight off). Returns a set
// of standby_act_t to carry out.
uint32_t standby_tick(standby_t *s, bool playing, int64_t now_ms);

// During the fade: the volume to play at, from `volume` down to 0 in a
// straight line over STANDBY_FADE_MS. `volume` when not fading.
int standby_fade_level(const standby_t *s, int volume, int64_t now_ms);

// The next time standby_tick has something to do (including each step of
// the fade's volume), or INT64_MAX.
int64_t standby_deadline(const standby_t *s, int volume, int64_t now_ms);
```

- Produces (`scrub.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fast-forward and rewind by holding 6 or 4: where the target time has got
// to. Times in milliseconds.

#define SCRUB_STEP_MS 2000       // per repeat at first
#define SCRUB_FAST_STEP_MS 5000  // per repeat once held SCRUB_FAST_AFTER_MS
#define SCRUB_FAST_AFTER_MS 2000 // after the long press
#define SCRUB_END_MARGIN_MS 1000 // the target stops this far before the end

typedef struct {
  bool active;
  int direction;        // +1 forward, -1 back
  uint32_t target_ms;
  uint32_t duration_ms; // 0: unknown, no end limit
  int64_t since_ms;     // when the long press came
} scrub_t;

// The long press: start from where playback is.
void scrub_start(scrub_t *s, int direction, uint32_t elapsed_ms,
                 uint32_t duration_ms, int64_t now_ms);

// One key repeat.
void scrub_repeat(scrub_t *s, int64_t now_ms);

// The release: the time to jump to. The scrub is then over.
uint32_t scrub_finish(scrub_t *s);
```

- Produces in `browse.h` (binding additions; `browse.h` includes `standby.h` for `standby_backlight_t`):

```c
// The Settings screen becomes: Rescan library, Backlight, Sleep timer,
// Keypad test, About. Its rows show values (spec section 9).

// New actions (add to browse_action_t):
//   BROWSE_BACKLIGHT   move the backlight setting on
//   BROWSE_SLEEP       move the sleep timer on

// What the Settings rows show. It belongs to the caller, who keeps it up to
// date; browse only reads it.
typedef struct {
  standby_backlight_t backlight;
  uint32_t sleep_minutes; // 0: off
} browse_settings_t;

void browse_set_settings(browse_t *b, const browse_settings_t *s);
```

Required behaviour:
- `standby` follows the "Backlight and sleep" constraint exactly. The backlight goes off at `last_key_ms` plus 15, 30 or 60 s; `STANDBY_BACKLIGHT_ON` never goes off. A fade ends at `fade_from_ms + STANDBY_FADE_MS` with `STANDBY_ACT_STOP`; a timer that expires with nothing playing gives `STANDBY_ACT_LIGHT_OFF` and no fade. `standby_deadline` gives each moment the fade level changes (not a fixed tick), so the UI wakes once per volume step.
- `scrub` follows the "Fast-forward and rewind" constraint: steps measured from `since_ms`, clamped at 0 and at `duration_ms - SCRUB_END_MARGIN_MS` (or 0 if the track is shorter than the margin) when the length is known.
- Settings rows: `Rescan library`, `Backlight: 30 s` (`15 s`, `60 s`, `on`), `Sleep: off` / `Sleep: 27 min`, `Keypad test`, `About`, every text within 15 characters. Without attached settings the two rows show their names alone (`Backlight`, `Sleep timer`).

Required tests (binding names):
- `test_standby.c`: `test_backlight_goes_off_after_each_timeout`, `test_always_on_never_goes_off`, `test_a_key_keeps_it_lit`, `test_the_waking_press_and_its_events_are_ignored` (press, long, repeats and release of that key all false; the next press true), `test_a_different_key_after_waking_is_acted_on`, `test_sleep_counts_from_when_chosen`, `test_sleep_minutes_round_up`, `test_next_sleep_cycles_after_the_last_chosen`, `test_expiry_while_playing_fades_then_stops`, `test_expiry_with_nothing_playing_only_turns_the_light_off`, `test_fade_level_is_a_straight_line_to_zero`, `test_a_key_during_the_fade_cancels_it`, `test_timer_is_off_after_it_expires`, `test_deadline_wakes_for_each_fade_step_and_each_timeout`, `test_changing_the_backlight_setting_counts_from_now`.
- `test_scrub.c`: `test_forward_two_seconds_per_repeat_then_five`, `test_back_stops_at_zero`, `test_forward_stops_before_the_end`, `test_track_shorter_than_the_margin`, `test_unknown_length_has_no_end_limit`, `test_finish_returns_the_target_and_ends_the_scrub`.
- `test_browse.c`: `test_settings_rows_show_values`, `test_settings_rows_without_values`, `test_select_backlight_and_sleep_rows`, and the existing Settings tests updated for the new rows.

- [ ] **Step 1:** Headers, stubs, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement. Whole host suite passes, nothing on stderr. Firmware builds without warnings.
- [ ] **Step 3:** Commit: `Add standby and scrub; Settings rows show the backlight and sleep timer`.

---

### Task 2: Backlight, sleep timer, saved setting and About on the device

**Files:**
- Modify: `main/ui.c`, `main/ui_play.c`, `main/ui_screens.c`, `main/ui_state.h`, `main/state_task.c/.h` (the backlight setting), `components/dap_hw/storage.c/.h` (free space), `main/playlist_task.c/.h` (a request that reads it), `main/debug_console.c/.h` (`stat` or `state` shows the new values)

**Interfaces:**
- Consumes: `standby`, `browse_set_settings`, `BROWSE_BACKLIGHT`, `BROWSE_SLEEP` (Task 1); `lcd_set_backlight` (`components/dap_hw/lcd.h`); `state_task` settings (stage 6); the playlist task's request/event pattern (stage 7).
- Produces: the stage's backlight, sleep and About behaviour.

Required behaviour:
- Every key event passes through `standby_key` before anything else; the backlight is switched with `lcd_set_backlight` only when its state changes.
- The sleep timer's actions are carried out as the constraint says. The fade changes the player's volume step by step (`player_set_volume`) without saving it; `STANDBY_ACT_STOP` stops playback the way `ui_stop` does, puts the player's volume back to the setting, and turns the backlight off. If the track ends, is stopped or paused during the fade, the volume is put back at once and the timer is off.
- The backlight setting is saved and loaded with volume, shuffle and loop (`state_settings_t` gains it; a new NVS key; the default for a value not yet saved is 30 s). The sleep timer is not saved.
- About: top row the track count (`1028 tracks`), bottom row a marquee `Free 912 MB  Built Oct  7 2026` (free space in MB below 10 GB, GB with one decimal above; the build date from the compiler). The free space is asked for when About opens and read by the playlist task (one new request kind, following stage 7's request/event/copy-out pattern; `storage` gains a function returning the free bytes through ESP-IDF's FAT information call). Until it arrives the bottom row shows `Free ...`; if it fails, `Free ?`.
- Timers keep counting across scans, card removal and screen changes; nothing here waits on the card.

Board checks:
1. Backlight at 15 s: no key for 15 s, the log shows the backlight off (add a log line when it changes); `key 6` on Now Playing while it is off: backlight on, no skip (`queue` position unchanged); `long 6` while off: no skip on release; `key B` while off: volume unchanged.
2. Each setting: cycle `Backlight` through 15 s, 30 s, 60 s, on, 15 s with `5`; `screen` shows each value; reset the board: the setting is kept (`state`). `on`: no backlight-off for two minutes.
3. Sleep: choose 15 min, `screen` shows `Sleep: 15 min`, then minutes counting down (a temporary build may shorten the minutes to seconds: say so, and re-check the committed build's row text). Expiry while the music track plays: the volume steps down over 10 s (log each step), playback stops, `stat` idle, the backlight goes off, `Sleep: off`, the volume setting unchanged (`state`), `key D` resumes from where it stopped at the setting's volume. Expiry with nothing playing: only the backlight. A key during the fade: volume back at once, timer off. A reset during the fade: after boot the volume is the setting.
4. About: `screen` shows the track count, then `Free ...` replaced by the free space and build date; compare the free space with the card's size.
5. A scan and a card removal while the backlight and sleep timers run: they keep counting; no errors.
6. Saving and timers not heard: the music track from start to end with the backlight going off and on (keys every 20 s) and the sleep row changed twice: exact `ENDED`, `underruns=0`.
7. Leave the saved state cleared (`state clear`) and say so.

- [ ] **Step 1:** Backlight and the waking key.
- [ ] **Step 2:** Sleep timer and fade; saved backlight setting.
- [ ] **Step 3:** About with free space.
- [ ] **Step 4:** Board checks. Commit (one or several, each buildable): final subject `UI: backlight timeout, sleep timer and the About screen`.

---

### Task 3: Fast-forward and rewind

**Files:**
- Modify: `main/ui.c`, `main/ui_play.c`, `main/ui_screens.c`, `main/ui_state.h`, `main/ui.h` (the LCD allows eight custom characters and seven are taken, so `>>`/`<<` are plain characters)

**Interfaces:**
- Consumes: `scrub` (Task 1); `player_seek_rel` and `player_play(path, start_ms, ...)` (`main/player.h`); the stage 6 release logic for `4`/`6`; play numbers.
- Produces: holding `4`/`6` on Now Playing seeks.

Required behaviour:
- The "Fast-forward and rewind" constraint, on Now Playing only, for the queue's current track whether playing or paused. A long press of `6`/`4` starts a scrub from the current elapsed time; each repeat moves it; the bottom row shows `>>` or `<<` and the target time; the release jumps there with one seek (`player_seek_rel` by the difference from where playback then is, or for a paused track, by whatever keeps it paused and lands it at the target when `D` is pressed: say which). A tap still skips (stage 6).
- If the track ends while the key is held, the scrub is dropped and the queue moves on as usual; the release then does nothing.
- The elapsed time shown, and the position saved, never exceed the track's length (stage 7 saw 307 s on a 305 s track after a seek near the end).
- The position is saved after a seek (NVS, as any position change).

Board checks:
1. Music track playing: `hold 6 20` (one press, one long, 20 repeats, one release): `screen` during the hold shows `>>` and a target moving 2 s per repeat; after release the first `POSITION` is near the target. `hold 6 60`: the step becomes 5 s after 2 s. `hold 4 30` from 1:00: back towards 0:00, stops at 0:00.
2. Fast-forward to the end of the music track: target stops one second before the end; after release the track plays its last second and the queue moves on; exact `ENDED`.
3. A 0.1 s track and a 10 s tone: no crash, no double start, `queue` sane. An MP3: the jump lands near the target.
4. Paused: `hold 6 20` then `key D`: plays from the target.
5. Release semantics still hold: `key 6` skips; `long 6` (no repeats) does not skip and does not move playback.
6. Backlight off: `hold 6 20` only wakes the backlight (Task 2's rule).
7. Resume: seek to about 2:00, pause, reset: Now Playing paused near 2:00.
8. The stage 5-7 behaviour still holds (menus, `hold 8 500` on Songs as fast as before, queue keys, playlists open and play).

- [ ] **Step 1:** Implement and build without warnings.
- [ ] **Step 2:** Board checks. Commit: `UI: fast-forward and rewind by holding 6 and 4`.

---

### Task 4: Deferred fixes from stages 6 and 7

**Files:**
- Modify: `main/state_task.c`, `main/playlist_task.c`, `main/ui_play.c`, `main/ui_options.c`, `main/ui_playlists.c`, `components/dap_core/playlist_catalog.c` and `test/host/test_playlist_catalog.c`, and whichever files hold the long lines below

Each fix below is small; each gets a test where it lives in `dap_core`, and a board check otherwise.

1. **State task: a superseded failed queue save leaves its retry deadline armed,** so a newer queue can be written early, before the settling time and without back-off. Clear the armed retry when a newer hand-over supersedes the failed copy.
2. **State task: a whole `.tmp` queue file that could not be read** (an I/O or allocation failure) is discarded. Discard it only when the bytes were read and `play_queue_restore` refused them; otherwise leave it for the next boot.
3. **State task comments:** correct the explanation of unbuffered writes (an unbuffered stream writes in pieces no larger than newlib's `BUFSIZ`) and the "saved again in N s" log with no card.
4. **UI: a refused `player_play`** (the player's command queue full) leaves the UI showing the new track stopped while the old one plays on. Send `player_stop()` in that branch so the two agree.
5. **UI: confirmations linger** for the rest of their 1.5 s after the options menu closes or the mode changes. Clear a confirmation (not the volume bar) when the options menu, picker or confirmation closes and when switching between Now Playing and the menus.
6. **Playlist task: with no index loaded** (after a failed scan) every request answers `library busy`, even listing, renaming and deleting, which need no index. Answer `library busy` only while a scan is running (the library status says so); list, rename and delete work with no index; requests that carry or resolve track numbers answer `library busy` as now.
7. **Playlist task: a failed create can leave a whole-looking `.tmp`** that the next listing renames into a playlist (when `playlist_edit` fails after writing, `fclose` still records the length). Truncate the stream before closing it on any failure.
8. **Names:** also refuse `CON.X`-style names (a reserved name followed by `.` and anything), which Windows also cannot open. Add the cases to `test_name_check_each_rule`.
9. **An append to a playlist deleted meanwhile** shows nothing. Show `Not found` (fits 16) for a `no such entry` result of an append or create-with-tracks.
10. **Long lines:** reflow lines over 80 columns in `test/host/test_playlist.c` and `main/playlist_task.c`.

Board checks: 4 (fill the player's command queue with a temporary hook, not committed: the UI and player agree afterwards); 5 (`screen` after `#` > `Add to queue` then closing: the confirmation is gone at once); 6 (a temporary hook that makes the index load fail: `pl ls`, `pl mv`, `pl del` work, `pl show` answers library busy); 7 and 1-2 by constructed cases with temporary hooks, as stages 6 and 7 did; 9 (append to a playlist deleted from the console while the picker is open). Host suite passes. Leave the card and saved state clean.

- [ ] **Step 1:** Fixes 1-3 (state task), with their checks.
- [ ] **Step 2:** Fixes 4-5 (UI), with their checks.
- [ ] **Step 3:** Fixes 6-10 (playlists), with tests and checks.
- [ ] **Step 4:** Commit (one per step is fine): final subject `Fix the items deferred from stages 6 and 7`.

---

### Task 5: An hour of continuous play, and memory over many tracks

**Files:**
- Create: `docs/superpowers/plans/stage8-soak.md` (the run's notes and figures)
- Modify: `main/player.c` and others only to fix what the run finds; `main/debug_console.c/.h` if a committed heap report helps (`stat` already prints internal free and largest block)

Required:
- **The run:** at least 60 minutes of continuous play with loop all over a queue mixing every format on the card (both `/music/` tracks, every FLAC and MP3 in `/test/`, some `/lib/` tracks), shuffle on part of the time, with a script that every few minutes changes volume, pauses and resumes, seeks (Task 3), skips, adds to the queue, edits a playlist and opens About. The backlight timeout is left at 30 s throughout.
- **What it records:** every `STARTED` and `ENDED` (each FLAC's `ENDED frames=X expected=X` exact), `underruns` (0 at the end), and after every track the internal free bytes, the largest free internal block and the free PSRAM.
- **Memory:** if the largest free internal block or free PSRAM shrinks over the run (not just between formats), find out what allocates and fix it so that it is flat; if it drops between formats but recovers, explain it. Stage 7 saw the largest block at 14 KB after an MP3 then a FLAC: find out why, and if it can fall low enough to make an allocation fail (the decoder's, the SD bounce buffer's, a FatFs `FIL`), fix it (for example by allocating the decoder's buffers once at start-up, or in PSRAM where the codec allows). Any change to `player.c` keeps every fact in the "Player facts" constraint and is re-checked with a full `ENDED` run of every file in `/test/`.
- **The notes** (`stage8-soak.md`): what ran, the figures (a table of memory per track, or its minimum, maximum and trend), what was found and fixed, and what was not.

Board checks: the run itself, on the committed build, and, after any fix, a second run of at least 30 minutes showing flat memory.

- [ ] **Step 1:** Run, record, investigate.
- [ ] **Step 2:** Fix what it finds; re-run. Commit (one or several): final subject `Soak: an hour of play with flat memory`.

---

### Task 6: User guide

**Files:**
- Create: `docs/USER_GUIDE.md`
- Modify: `README.md` (it already describes the player: bring it up to date, link the guide, keep build and flash instructions and where the spec and plans are)

Required content of `docs/USER_GUIDE.md`, written for the owner, in plain words, checked against the code (not the plans) where they differ:
- **Parts and wiring:** the board, DAC, LCD, keypad; a label-by-label wiring table for each (from `main/pins.h` and the spec: DAC `SCK` to GND, `BCK` 26, `DIN` 22, `LCK` 25, `VCC` 3.3 V; LCD on 5 V; keypad rows and columns with their 10k pull-ups). Wire by label, never by position.
- **Power:** a phone charger or a USB power bank (one with a low-current mode if it switches off when paused); not a computer's USB port with the LCD attached unless the card is re-seated after plugging in.
- **The card:** FAT, music anywhere, `/playlists/` for M3U files, `/.dap/` holds the index and queue (do not edit), the first boot scans; Rescan after changing music.
- **Keys and screens:** the key map for lists and Now Playing, letter jumps, options (`#`), text entry, the volume bar, the marks, fast-forward and rewind.
- **Playlists:** making and editing them on the device; using computer-made ones (paths that work); device-made ones on a computer.
- **Settings:** Rescan, Backlight, Sleep timer, Keypad test, About; what is remembered at power-off (volume, shuffle, loop, backlight, the queue and the place in the track).
- **Building and flashing:** `source /opt/esp-idf/export.sh`, `idf.py build`, `idf.py -p /dev/ttyUSB0 flash` (a full flash after a partition-table change), the `uucp` group.
- **Troubleshooting:** `Insert SD card` (re-seat the card; the computer-port power quirk), `Can't play`, `File missing`, `(missing)` playlist entries, `Library full`, `Card error`, no LCD text (5 V), a click or buzz (report it).
- **The serial console:** what it is for and its main commands, pointing to `main/debug_console.h`.

- [ ] **Step 1:** Write the guide and update the README; check every key, text and setting against the code.
- [ ] **Step 2:** Commit: `Add the user guide; bring the README up to date`.

---

## Stage 8 hardware check (run by the user)

On a phone charger or a power bank.

1. **Backlight:** leave it alone 30 s: the backlight goes off. Press `6` on Now Playing: the light comes on and the track does not change. In Settings, try each `Backlight` value; unplug and replug: it is kept.
2. **Sleep timer:** set `Sleep: 15 min` while music plays: at the end the music fades out over about 10 seconds and stops, and the light goes off; `D` carries on from there at your normal volume. Press a key during a fade: it stops fading.
3. **Fast-forward and rewind:** hold `6` on Now Playing: the time races forward, faster after two seconds, and playback jumps there when you let go. Hold `4` to go back. A quick tap still skips.
4. **About:** shows the number of tracks, free space on the card and the build date.
5. **An hour of play:** play for an hour or more with loop all: no dropouts, clicks or buzz.
6. **The guide:** read `docs/USER_GUIDE.md`: it matches what the player does.
