# DAP Stage 7 (Playlists) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Playlists on the device: M3U files in `/playlists/` that can be created, filled, reordered, renamed, deleted and played from the keypad, with names typed by multi-tap, and files made on a computer accepted.

**Architecture:** Everything with rules is hardware-free and tested on the host: `textentry` (multi-tap), `playlist` (reading M3U lines into tracks, and copying a file with one edit applied, streaming through a fixed work area), `playlist_catalog` (the names on the card, sorted, and the naming rules), and additions to `options`, `browse` and `play_queue`. On the device a low-priority playlist task does all reading and writing of `/playlists/`, so the UI task never waits on the card; the UI keeps only the catalog and the open playlist's entries as track numbers.

**Tech Stack:** ESP-IDF v6.0.2 (C), FatFs through stdio, FreeRTOS, Unity host tests under AddressSanitizer.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md`. Read section 3 ("Code layout", "Performance rules"), section 8 in full (amended for this stage), section 9 ("Keys", "Screens", "Menu tree", "Options menus", "Text entry"), section 11 and stage 7 of section 14. The stage 6 plan's "Amendments made during implementation" (`docs/superpowers/plans/2026-10-02-dap-stage6-queue.md`) describes the queue and state rules this stage builds on.

**How this plan works:** it fixes interfaces, behaviour, test cases and acceptance checks; implementation bodies and test code are the implementer's. Headers given here are binding. If an interface cannot be implemented as written, or forces an inefficiency, stop and report NEEDS_CONTEXT; do not change it silently.

## Scope

In this stage: multi-tap text entry; reading and editing M3U files; the Playlists screen and a playlist's entries; the options menus for a playlist, an entry, Now Playing, and `Add to playlist` on tracks, albums and artists; the playlist picker (`New...` first); creating, renaming, deleting (with confirmation), appending, moving and removing; `Save queue as playlist`; playing and shuffle-playing a playlist; the console commands agents need to check all of it.

Not in this stage (do not build): backlight timeout, sleep timer, the About screen's figures, fast-forward and rewind (stage 8). There is no screen that lists the queue. Playlists are not part of the library index and are not scanned.

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port the SD card sometimes fails to mount; the firmware shows `Insert SD card` and retries every 2 seconds. If the retries go on for more than a minute, STOP and report BLOCKED: only the user can re-insert the card.
- **Library reader rule** (`main/library_task.h`, binding): `library_task_get()` may return NULL; every non-NULL get is followed promptly by `library_task_put()` in the same task; no pointer into the index survives a put; track numbers are valid only within one index, which `library_checksum()` identifies. Anything that carries track numbers between tasks carries the checksum with them, and the receiver refuses numbers whose checksum is not the loaded index's.
- **Playlist files (spec section 8, as amended):**
  - Files in `/playlists/` whose names end in `.m3u` or `.m3u8` (any case) are playlists; the name shown is the file name without that ending. The device creates `<NAME>.m3u`, UTF-8, LF line endings, one entry per line written as `../` followed by the track's path from the index (relative to the playlist's folder, so a computer opens it).
  - Reading: a UTF-8 byte-order mark and a CR before the LF are ignored; empty lines and lines starting `#` are not entries. Backslashes count as `/`. A line that is absolute (a leading `/`, a drive letter such as `E:`, or a `file://` URL, whose `%XX` escapes are decoded) is a path from the card root. A relative line is tried from the card root first, then from `/playlists/` (where `..` goes up). The case of ASCII letters is ignored, as the file system ignores it. A line that matches no track is an entry shown `(missing)` and skipped in playback.
  - An entry is a block: its path line and every line after the previous entry's path line (comments such as `#EXTINF`, blank lines), except a first line `#EXTM3U`, which stays first. Lines after the last entry stay last. Edits move or remove whole blocks and copy every other byte as it was. Appended entries come last, after a newline if the file did not end with one.
  - At most 1,000 entries (`PLAYLIST_ENTRIES_MAX`). A computer-made file with more shows and plays its first 1,000 and keeps the rest; an edit that would leave more than 1,000 adds nothing and says `Playlist full`.
  - Every edit is saved at once by writing `<file>.tmp`, then putting it in place, with the care `main/library_task.c` documents for the index (read its power-cut analysis and follow it). A power cut leaves the old playlist or the new one, never a mixture; a leftover `.tmp` is tidied the next time the folder is listed.
- **Names (spec section 8, as amended):** capitals, digits, space and `- ' & .`; 1 to 16 characters after spaces at either end are trimmed; must not end with `.` or start with `.`; must not equal an existing playlist's name, ignoring case and the `.m3u`/`.m3u8` ending (`Name in use`). Names of computer-made files are shown as they are, whatever they contain.
- **The UI task never waits on the card.** All reading and writing of `/playlists/` happens in the playlist task. A request that cannot be queued is refused at once and the UI says `Busy`; no request that was accepted is lost.
- **Performance rules (spec section 3):** playlist files are read and written in blocks of at least 4 KB, never a byte or a line at a time through stdio; resolving a file is one pass with O(log n) lookups per line; the catalog is sorted once per listing; moving and removing entries in the UI's copy is O(n) in the playlist's length at worst; nothing the user does in a playlist scans the library. No allocation in the UI loop; no polling.
- **Memory:** internal data RAM while playing is about 39 KB, largest block about 31 KB. Every buffer this stage adds lives in PSRAM and is allocated once at start-up, except task stacks; each task's internal-RAM cost is measured and reported. Allocation failure at start-up is handled as spec section 11 says.
- **Player facts that must not be disturbed** (`main/player.c`): this stage does not modify `player.c`. Every FLAC that plays from start to end still logs `ENDED frames=X expected=X` (tag `player`); saving playlists while music plays causes no `UNDERRUN`.
- `components/dap_core` is pure C using only the C standard library. Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan and must not allocate from the heap.
- No compiler warnings from `main/` or `components/`. Nothing that can fail at run time because of a file, the card or flash may use `ESP_ERROR_CHECK`, abort or restart.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Agents cannot hear the output, see the LCD or press keys. Console: `key <char>`, `long <char>`, `hold <char> <n>` inject keys; `screen` prints what the LCD shows; `stat`, `queue`, `state`, `lib ...`, `play`, `pause`, `resume`, `stop`, `seek`, `scan`. Replies are `dbg: ok ...` / `dbg: err ...`. Anything only a person can judge goes in the hardware check.
- On the card: `/music/` (two real tracks; `16 - Kanye West - Hey Mama.flac` is 13453440 frames, 305 s), `/test/` (untagged tones), `/lib/` (14 tagged 0.1 s tracks plus two that cannot play), `/lib1000/` (1,000 tracks of 0.1 s: 10 artists x 10 albums x 10). The index has 1,028 tracks. There is no `/playlists/` folder yet: the device creates it when it first saves a playlist.

## Review Focus

1. **Computer-made files.** A BOM, CRLF endings, `#EXTM3U` and `#EXTINF` lines, absolute Windows paths with a drive letter and backslashes, `file:///E:/...` URLs with `%20`, paths relative to `/playlists/` and to the card root, letters in the wrong case, UTF-8 names, blank lines, a last line without a newline, a line longer than any path, more than 1,000 entries, a binary file named `.m3u`. Each loads without harm: what is on the card plays, the rest shows `(missing)`. Pinned in Task 2 by fixture files, and on the board in Task 4.
2. **Edits change only what they say.** After any edit every byte outside the edited block is as it was, a block's comment lines travel with it, `#EXTM3U` stays first, and an edit of a computer-made file leaves its other lines verbatim. Pinned in Task 2.
3. **Power pulled during a save** leaves the old file or the new one and no stray playlist; a leftover `.tmp` is tidied. Pinned in Task 4 by constructed cases on the board.
4. **Edits in quick succession.** Ten `Move up`s pressed as fast as keys allow all reach the file, in order, and the list on screen matches the file afterwards; if the card fails mid-way the screen reloads what the file holds. Pinned in Task 4 (console burst) and Task 5 (keys).
5. **The index changes under a playlist.** A rescan while a playlist is open, while the picker holds tracks to add, or while text entry is open for `Save queue`: no track number from the old index is ever written to a file. Pinned in Task 4 (stale checksum refused) and Task 5.
6. **Names.** Every rule refused with its message, including `Name in use` against a computer-made `rock.m3u8` when typing `ROCK`, and renaming a playlist to its own name. Pinned in Task 3 and Task 5.
7. **Text entry timing.** Commit after one second, a different key commits, the 16-character limit, `*` while a letter is pending, a held key not cycling. Pinned in Task 1 and Task 5.
8. **Saving must not be heard.** Pinned in Task 4: edits of a 1,000-entry playlist during the music track, exact frame count, `underruns=0`.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/textentry.c/.h` | Multi-tap text entry | 1 |
| `components/dap_core/crc32.c/.h` | One CRC-32 for `library` and `play_queue` (carried from stage 6) | 1 |
| `components/dap_core/playlist.c/.h` | M3U lines to tracks; copy a file with one edit applied | 2 |
| `components/dap_core/playlist_catalog.c/.h` | The playlists on the card, sorted; the naming rules | 3 |
| `components/dap_core/options.c/.h`, `browse.c/.h`, `play_queue.c/.h` | Playlist options and targets; playlist levels; loading the queue from an array | 3 |
| `main/playlist_task.c/.h` | All reading and writing of `/playlists/`, off the UI task | 4 |
| `main/debug_console.c/.h` | `pl` commands | 4 |
| `main/ui*.c`, `main/ui_state.h`, `main/ui.h` | Playlist screens, picker, text entry, confirmation, Now Playing options | 5 |

---

### Task 1: `textentry`, and two cleanups carried from stage 6

**Files:**
- Create: `components/dap_core/textentry.h`, `textentry.c`, `components/dap_core/crc32.h`, `crc32.c`
- Test: `test/host/test_textentry.c`, `test/host/test_crc32.c`
- Modify: `components/dap_core/library.c`, `play_queue.c` (use `crc32`), `test/host/test_play_queue.c` (the `/dev/full` branch), CMake files

**Interfaces:**
- Produces (`textentry.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Typing a name on the keypad by multi-tap: each digit key cycles through
// its letters and then the digit; a character is committed when another key
// is pressed or after TEXTENTRY_COMMIT_MS.

#define TEXTENTRY_MAX 16        // characters of text
#define TEXTENTRY_COMMIT_MS 1000
#define TEXTENTRY_ROW 16        // width of the bottom row

typedef enum {
  TEXTENTRY_CONTINUE, // keep typing
  TEXTENTRY_DONE,     // '#': the text is finished (see textentry_text)
  TEXTENTRY_CANCEL,   // 'C': give up
} textentry_result_t;

typedef struct {
  char text[TEXTENTRY_MAX + 1]; // committed characters, terminated
  uint32_t len;
  char key;         // the key whose character is pending, or 0
  uint32_t cycle;   // which of that key's characters is pending
  int64_t since_ms; // when `key` was last pressed
} textentry_t;

// Start with `initial` (may be NULL): letters are made capitals, characters
// a name cannot have are dropped, and it is cut to TEXTENTRY_MAX.
void textentry_init(textentry_t *t, const char *initial);

// A key press at `now_ms`. '0'..'9' type, '*' deletes, '#' finishes, 'C'
// cancels; any other key does nothing. Call textentry_tick first if time
// may have passed.
textentry_result_t textentry_key(textentry_t *t, char key, int64_t now_ms);

// Commit the pending character if TEXTENTRY_COMMIT_MS have passed since its
// key was pressed. True if the text changed.
bool textentry_tick(textentry_t *t, int64_t now_ms);

// When textentry_tick next needs calling, or INT64_MAX if nothing is pending.
int64_t textentry_deadline(const textentry_t *t);

// The text as '#' would finish it: the committed characters and the pending
// one. Always terminated; `size` of at least TEXTENTRY_MAX + 1 always fits.
void textentry_text(const textentry_t *t, char *out, size_t size);

// The bottom row: the text, then the pending character or, if none is
// pending and there is room, a '_' cursor; the last TEXTENTRY_ROW characters
// if that is longer than the row; padded with spaces. No terminator.
void textentry_row(const textentry_t *t, char out[TEXTENTRY_ROW]);
```

- Produces (`crc32.h`): `uint32_t crc32_update(uint32_t crc, const void *data, size_t len);` — standard reflected CRC-32 (polynomial 0xEDB88320); start with 0, feed pieces in order (`crc32_update(crc32_update(0, a, n), b, m)` equals the CRC of `a` followed by `b`); `crc32_update(0, "123456789", 9) == 0xCBF43926`.

Required behaviour (spec section 9 "Text entry"): `2` cycles `A B C 2`, `3` `D E F 3`, `4` `G H I 4`, `5` `J K L 5`, `6` `M N O 6`, `7` `P Q R S 7`, `8` `T U V 8`, `9` `W X Y Z 9`, `0` space then `0`, `1` `- ' & .` then `1`; after the digit the cycle starts again. The same key again within `TEXTENTRY_COMMIT_MS` of its last press moves the cycle on; later, or a different key, commits the pending character first. `*` with a character pending drops it; otherwise it deletes the last committed character. `#` commits the pending character and returns `TEXTENTRY_DONE`. At `TEXTENTRY_MAX` committed characters a typing key does nothing; with `TEXTENTRY_MAX - 1` committed and one pending, a different typing key commits the pending one and is then ignored.

`crc32`: `library.c` and `play_queue.c` both use it and drop their own copies; the files they write are byte-for-byte unchanged (their existing tests prove it). The `/dev/full` branch of `test_play_queue.c` closes the file it replaces, and reports a skip with `TEST_IGNORE_MESSAGE` where `/dev/full` does not exist.

Required tests (binding names): in `test_textentry.c`: `test_each_key_cycles_its_letters_then_its_digit`, `test_same_key_within_a_second_cycles_and_after_a_second_commits`, `test_a_different_key_commits_the_pending_character`, `test_tick_commits_after_the_deadline_and_not_before`, `test_deadline_is_max_with_nothing_pending`, `test_star_drops_the_pending_character_or_deletes_the_last`, `test_hash_commits_and_finishes`, `test_c_cancels`, `test_other_keys_do_nothing`, `test_length_limit`, `test_init_cleans_the_initial_text` (lower case, characters a name cannot have, too long), `test_row_shows_the_cursor_the_pending_character_and_the_end_of_a_long_text`. In `test_crc32.c`: `test_check_value`, `test_pieces_equal_the_whole`, `test_empty_is_zero`.

- [ ] **Step 1:** Headers, stubs, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement; switch `library.c` and `play_queue.c` to `crc32`; fix the `/dev/full` branch. Whole host suite passes, nothing on stderr. Firmware builds without warnings; report the change in binary size.
- [ ] **Step 3:** Commit: `Add textentry; one CRC-32 for library and play_queue`.

---

### Task 2: `playlist` (M3U files)

**Files:**
- Create: `components/dap_core/playlist.h`, `playlist.c`
- Test: `test/host/test_playlist.c`, fixture files under `test/host/fixtures/playlists/`
- Modify: CMake files

**Interfaces:**
- Consumes: `library_find_path`, `library_track_path`, `library_track_count` (`library.h`).
- Produces (`playlist.h`, binding):

```c
#pragma once

#include "library.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Playlists are M3U files: one track per line. This module reads a file into
// the tracks of one library index, and copies a file with one edit applied.
// Both stream through a work area the caller provides, in large blocks; the
// rules for lines and blocks are in the spec, section 8.

#define PLAYLIST_ENTRIES_MAX 1000
#define PLAYLIST_MISSING UINT16_MAX // an entry the index does not have
#define PLAYLIST_LINE_MAX 1024      // longest line, end of line excluded
#define PLAYLIST_PATH_MAX 256       // a path from the card root, terminated

// Scratch space for reading and editing: allocate one (PSRAM on the device)
// and pass it to every call; the module keeps nothing in it between calls.
#define PLAYLIST_WORK_BYTES (16 * 1024)
typedef struct {
  _Alignas(8) unsigned char bytes[PLAYLIST_WORK_BYTES];
} playlist_work_t;

typedef enum {
  PLAYLIST_OK,
  PLAYLIST_TRUNCATED, // read: more than PLAYLIST_ENTRIES_MAX entries; the
                      // first PLAYLIST_ENTRIES_MAX were read
  PLAYLIST_FULL,      // edit: the result would have more than
                      // PLAYLIST_ENTRIES_MAX entries; nothing was written
  PLAYLIST_NO_ENTRY,  // edit: the entry named does not exist, or cannot
                      // move that way; nothing was written
  PLAYLIST_BAD_FILE,  // a line longer than PLAYLIST_LINE_MAX
  PLAYLIST_IO_ERROR,  // a read or a write failed
} playlist_result_t;

// The paths from the card root that an M3U line may mean, most likely first:
// 0 (a comment, an empty line, a URL that is not file://, a path that goes
// above the card root), 1 (an absolute path) or 2 (a relative one: from the
// card root, then from "playlists/"). `line` excludes the end of line.
uint32_t playlist_line_paths(const char *line, size_t len,
                             char paths[2][PLAYLIST_PATH_MAX]);

// Read a playlist: the track of each entry in file order, PLAYLIST_MISSING
// for one that matches nothing in `lib`. `tracks` holds `max` entries
// (normally PLAYLIST_ENTRIES_MAX).
playlist_result_t playlist_read(FILE *in, const library_t *lib,
                                uint16_t *tracks, uint32_t max,
                                uint32_t *count, playlist_work_t *work);

typedef enum {
  PLAYLIST_EDIT_APPEND,    // add `tracks` at the end
  PLAYLIST_EDIT_REMOVE,    // remove entry `entry`
  PLAYLIST_EDIT_MOVE_UP,   // swap entry `entry` with the one before
  PLAYLIST_EDIT_MOVE_DOWN, // swap entry `entry` with the one after
} playlist_edit_kind_t;

typedef struct {
  playlist_edit_kind_t kind;
  uint32_t entry;         // REMOVE, MOVE_UP, MOVE_DOWN: from 0, file order
  const uint16_t *tracks; // APPEND: track numbers of `lib`
  uint32_t count;
} playlist_edit_t;

// Write to `out` the playlist `in` holds with `edit` applied. `in` may be
// NULL: an empty playlist (so APPEND with in == NULL makes a new file).
// Appended tracks are written as "../" and their path in `lib`, one per
// line, LF. On any result but PLAYLIST_OK what was written to `out` must be
// thrown away.
playlist_result_t playlist_edit(FILE *in, FILE *out, const library_t *lib,
                                const playlist_edit_t *edit,
                                playlist_work_t *work);
```

Required properties:
- No allocation, no global state. `playlist_read` and `playlist_edit` read and write only in blocks (`fread`/`fwrite` of at least 4 KB, or the whole rest of the file if less); say how the work area is divided.
- `playlist_read` takes the index's track count once and looks each line up with at most two `library_find_path` calls.
- `playlist_edit` APPEND counts the entries already in `in` and refuses with `PLAYLIST_FULL` before writing anything that would exceed the limit, or writes nothing it must take back (say which).
- A track number at or above `library_track_count(lib)` in an APPEND is a programming error the function refuses with `PLAYLIST_NO_ENTRY`.
- Fixtures are real files in `test/host/fixtures/playlists/` built against the existing `lib16.h` test library (read it; its paths are what the fixtures name). Bytes such as the BOM, CRLF and a missing final newline are written with escapes in a small generator or committed as binary-safe files; say which.

Required tests (binding names):

`test_line_paths_absolute_relative_and_comments` (leading `/`, `E:\`, `e:/`, `file:///E:/Music/A%20B.flac`, `../x`, `./x`, `a/../b`, `../../x` above the root, `#EXTINF`, empty, `http://`), `test_line_paths_backslashes_and_case`, `test_read_device_made_file`, `test_read_computer_made_file` (BOM, CRLF, `#EXTM3U`, `#EXTINF`, Windows absolute paths, `file://`, relative to `/playlists/` and to the root, wrong case, UTF-8), `test_read_marks_missing_entries`, `test_read_more_than_a_thousand_entries_is_truncated`, `test_read_rejects_a_line_that_is_too_long`, `test_read_binary_junk_is_all_missing`, `test_read_empty_file`, `test_read_io_error`, `test_append_to_nothing_makes_a_device_file`, `test_append_after_a_file_without_a_final_newline`, `test_append_refuses_to_pass_the_limit`, `test_remove_first_middle_last`, `test_move_up_and_down_swap_whole_blocks` (an `#EXTINF` line travels with its entry), `test_move_past_either_end_is_refused`, `test_extm3u_stays_first`, `test_lines_after_the_last_entry_stay_last`, `test_edits_keep_every_other_byte` (for each edit kind on the computer-made fixture: the output equals the input with exactly the edited block changed), `test_edit_io_error_on_write`, `test_device_file_reads_back_as_written` (append then read: the same tracks), `test_thousand_entries_timing` (read and edit a 1,000-entry file; prints the time; checks nothing is lost).

- [ ] **Step 1:** Header, stubs, fixtures, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement. Pass, nothing on stderr. Add to the firmware build; no warnings.
- [ ] **Step 3:** Commit: `Add playlist: read M3U files into tracks; copy a file with one edit applied`.

---

### Task 3: The model for the screens: catalog, names, options, browse, queue

**Files:**
- Create: `components/dap_core/playlist_catalog.h`, `playlist_catalog.c`; test `test/host/test_playlist_catalog.c`
- Modify: `components/dap_core/options.c/.h`, `browse.c/.h`, `play_queue.c/.h` and their tests `test_options.c`, `test_browse.c`, `test_play_queue.c`; CMake files

**Interfaces:**
- Consumes: `PLAYLIST_MISSING`, `PLAYLIST_ENTRIES_MAX` (Task 2). `browse.h` includes `playlist.h` and `playlist_catalog.h` for the types it names.
- Produces (`playlist_catalog.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The playlists in /playlists/ by name, A to Z, and the rules for a name the
// device gives one.

#define PLAYLIST_CATALOG_MAX 200      // playlists listed
#define PLAYLIST_CATALOG_POOL 16384   // bytes of file names
#define PLAYLIST_NAME_MAX 16          // a name typed on the device

typedef struct {
  uint32_t count;
  bool truncated; // some playlists did not fit and are not listed
  uint32_t pool_used;
  uint16_t offset[PLAYLIST_CATALOG_MAX]; // into `pool`, A to Z by name
  char pool[PLAYLIST_CATALOG_POOL];      // file names, each terminated
} playlist_catalog_t;

void playlist_catalog_clear(playlist_catalog_t *c);

// Add a file found in the folder. False, and nothing added, if it is not a
// playlist (no .m3u or .m3u8 ending, any case; a name starting with '.'; a
// .tmp) or there is no room (which sets `truncated`).
bool playlist_catalog_add(playlist_catalog_t *c, const char *file_name);

// Put the names A to Z, ignoring the case of ASCII letters, the way the
// library sorts. Call once after the last add.
void playlist_catalog_sort(playlist_catalog_t *c);

// The file name of playlist `i`, or "" if there is none.
const char *playlist_catalog_file(const playlist_catalog_t *c, uint32_t i);

// The name shown for playlist `i`: the file name without its ending.
// Always terminated.
void playlist_catalog_name(const playlist_catalog_t *c, uint32_t i, char *out,
                           size_t out_size);

// Which playlist has `name` (compared as names are: ignoring ASCII case and
// the ending). O(log n).
bool playlist_catalog_find(const playlist_catalog_t *c, const char *name,
                           uint32_t *i);

typedef enum {
  PLAYLIST_NAME_OK,
  PLAYLIST_NAME_EMPTY,    // nothing left after trimming spaces
  PLAYLIST_NAME_TOO_LONG, // more than PLAYLIST_NAME_MAX after trimming
  PLAYLIST_NAME_BAD_CHAR, // a character that is not allowed
  PLAYLIST_NAME_BAD_END,  // ends with '.'
} playlist_name_t;

// Check a typed name and write the name to use (spaces at both ends
// trimmed) to `out` (at least PLAYLIST_NAME_MAX + 1 bytes). Whether it is in
// use is playlist_catalog_find's business.
playlist_name_t playlist_name_check(const char *typed, char *out);

// "<name>.m3u", the file a new playlist gets. Always terminated.
void playlist_file_for(const char *name, char *out, size_t out_size);
```

- Produces in `play_queue.h` (binding):

```c
// Replace the queue with the entries of `tracks` that are below
// `index_tracks` (the others, such as a playlist's missing entries, are left
// out), current at the one at position `start`. With shuffle on the rest is
// shuffled and the chosen track is first. Returns how many were taken; 0,
// and the queue unchanged, if `start` is not below `count` or tracks[start]
// is left out.
uint32_t play_queue_load_tracks(play_queue_t *q, const uint16_t *tracks,
                                uint32_t count, uint32_t start,
                                uint32_t index_tracks);
```

- Produces in `options.h` (binding; replaces the stage 6 enum and comment):

```c
typedef enum {
  OPTION_PLAY,
  OPTION_PLAY_NEXT,
  OPTION_ADD_TO_QUEUE,
  OPTION_SHUFFLE_PLAY,
  OPTION_ADD_TO_PLAYLIST,
  OPTION_RENAME,
  OPTION_DELETE,
  OPTION_MOVE_UP,
  OPTION_MOVE_DOWN,
  OPTION_REMOVE,
  OPTION_SAVE_QUEUE,
  OPTION_COUNT, // not an option: how many there are
} option_t;
```

  `options_for` keeps its signature. Track: `Play`, `Play next`, `Add to queue`, `Add to playlist`. Album or artist: `Play`, `Shuffle play`, `Add to queue`, `Add to playlist`. Playlist: `Play`, `Shuffle play`, `Rename`, `Delete`. Entry: `Play`, `Move up`, `Move down`, `Remove`. Now Playing: `Add to playlist`, `Save queue`. Every text fits 15 characters.

- Produces in `browse.h` (binding additions):

```c
// New screens (add to browse_screen_t):
//   BROWSE_PLAYLISTS  "New playlist", then every playlist A to Z
//   BROWSE_PLAYLIST   id = the playlist's number in the catalog: its entries
// The main menu becomes: Now Playing, Artists, Albums, Songs, Playlists,
// Settings.

// New actions (add to browse_action_t):
//   BROWSE_NEW_PLAYLIST   "New playlist" was chosen
//   BROWSE_OPEN_PLAYLIST  a playlist's level is now current; `index` is its
//                         number in the catalog: load its entries
//   BROWSE_PLAY_ENTRIES   play the open playlist from entry `index`
//   BROWSE_MISSING        the entry chosen is missing: nothing to play

// New targets (add to browse_target_kind_t):
//   BROWSE_TARGET_PLAYLIST     `index` is the playlist's number in the catalog
//   BROWSE_TARGET_ENTRY        `index` is the entry in the open playlist
//   BROWSE_TARGET_NOW_PLAYING  for options_for only; browse_target never
//                              gives it

// What the playlist screens show. It belongs to the caller, who keeps it up
// to date and tells browse with browse_refresh; browse only reads it.
typedef struct {
  const playlist_catalog_t *catalog; // NULL: no listing yet
  const uint16_t *tracks; // the open playlist's entries, PLAYLIST_MISSING
                          // for a missing one; NULL while it loads
  uint32_t count;
  uint32_t playlist;      // which catalog entry `tracks` belongs to
} browse_playlists_t;

void browse_set_playlists(browse_t *b, const browse_playlists_t *p);
```

Required behaviour:
- `BROWSE_PLAYLISTS` is alphabetical apart from its first row (letter jumps skip `New playlist`; say how `browse_group` treats row 0). Its name is `Playlists`. `BROWSE_PLAYLIST`'s name is the playlist's name; its items are the entries' titles from the index, `(missing)` for a missing entry; it is not alphabetical; while its entries are not loaded (tracks NULL or `playlist` not its id) it has no items.
- `browse_refresh` brings both playlist levels' counts up to date from the attached data; a catalog that shrank below an open playlist's id leaves that level with no items (the UI then goes back). `browse_library_changed` behaves as now.
- `browse_select` derives what it does for the existing screens from `browse_target` where they overlap, so the two cannot drift (carried from stage 6), and the playlist screens give the actions above. `browse_target` gives PLAYLIST for a playlist row, ENTRY for any entry row (missing ones too: they can be moved and removed), NONE for `New playlist`.
- `play_queue_load_tracks` follows the same rules as `play_queue_load` (shuffle, the window when longer than the capacity) and is one pass over `tracks`.
- `test_options`' text-fit check runs to `OPTION_COUNT`.

Required tests (binding names):
- `test_playlist_catalog.c`: `test_add_only_playlist_files`, `test_sorted_ignoring_case`, `test_name_drops_the_ending`, `test_find_ignores_case_and_ending`, `test_full_catalog_sets_truncated`, `test_name_check_each_rule`, `test_name_check_trims_spaces`, `test_file_for_a_name`.
- `test_options.c`: `test_track_options` and `test_album_and_artist_options` updated; `test_playlist_options`, `test_entry_options`, `test_now_playing_options`; `test_texts` covering every option to `OPTION_COUNT`.
- `test_browse.c`: `test_main_menu_has_playlists`, `test_playlists_level_lists_new_then_names`, `test_playlists_letter_jump_skips_new`, `test_open_playlist_gives_its_number`, `test_playlist_level_is_empty_until_loaded`, `test_playlist_entries_show_titles_and_missing`, `test_select_entry_plays_and_missing_says_so`, `test_target_of_playlist_and_entry_rows`, `test_no_target_for_new_playlist`, `test_catalog_shrinking_empties_the_open_playlist`, and the stage 6 browse tests still pass unchanged apart from the main menu's new row.
- `test_play_queue.c`: `test_load_tracks_skips_missing_and_starts_at_the_chosen_one`, `test_load_tracks_refuses_a_missing_start`, `test_load_tracks_while_shuffle_is_on`.

- [ ] **Step 1:** Headers, stubs, tests. Fail for the right reason.
- [ ] **Step 2:** Implement. Whole host suite passes, nothing on stderr. Firmware builds without warnings (the UI does not use the new screens yet; the main menu's `Playlists` row may do nothing until Task 5, and says so in a comment).
- [ ] **Step 3:** Commit: `Model for playlists: catalog and names, options, browse levels, queue from an array`.

---

### Task 4: The playlist task

**Files:**
- Create: `main/playlist_task.h`, `main/playlist_task.c`
- Modify: `main/app_main.c`, `main/CMakeLists.txt`, `main/debug_console.c/.h`

**Interfaces:**
- Consumes: `playlist_read`, `playlist_edit` (Task 2); `playlist_catalog_*`, `playlist_file_for` (Task 3); `library_task` (stage 4); `storage` (mounted or not).
- Produces (`playlist_task.h`; define the exact signatures, document them fully in the style of `state_task.h`, and keep to these roles):
  - a start function creating the task (priority 2, core 0) and allocating its buffers once, taking the queue its results are posted to (the UI adds it to its queue set);
  - requests, each returning at once with a request number, or refused at once if it cannot be queued: **list** the folder; **load** a playlist (by file name); **create** a playlist with a name and tracks (possibly none); **append** tracks to a playlist; **remove**, **move up**, **move down** an entry; **rename**; **delete**. Requests carrying track numbers carry the index checksum they belong to; the task refuses them if that is not the loaded index's;
  - results: a small event per request (its number, what it was, and how it ended: done, name in use, playlist full, no such entry, bad file, card error, library busy, stale index), and calls that copy out a finished listing (`playlist_catalog_t`) or a finished load (the entries as track numbers, their count, whether truncated, and the checksum they belong to) under a lock held only for the copy;
  - requests are carried out one at a time in the order made; none accepted is lost or reordered.

Requirements:
- **The folder:** created when the first playlist is written. Listing tidies leftover `.tmp` files as the power-cut analysis requires, then lists, then sorts.
- **Saving:** every edit and every create writes `<file>.tmp` with `playlist_edit` and puts it in place following `main/library_task.c`'s pattern and its power-cut analysis; write that analysis for playlists at the top of `playlist_task.c`. Create and rename refuse a name already in the folder (ignoring case and ending): `Name in use`. Delete removes the file. Rename to the same name (ignoring case) is done and changes nothing else.
- **Memory:** one `playlist_work_t`, the request queue (requests can carry up to `PLAYLIST_ENTRIES_MAX` track numbers; size it so a burst of at least 16 small edits is accepted), and the result buffers, in PSRAM. Say how big each is, and the task's stack and where it is (if it can live in PSRAM because the task never touches flash, say so and do it; otherwise measure its internal cost). stdio streams the task opens must not take buffers from internal RAM (stage 6 found a 4 KB stdio buffer is a real share of what is left); use your own blocks.
- **The card absent or a scan running:** requests answer card error or library busy rather than wait; nothing retries by itself.
- **Console (committed, documented in `debug_console.h` in a fixed greppable form):** `pl ls` (the catalog: number, name, file name), `pl show <n>` (entries of catalog playlist `n`: position, track number or `missing`, and the path), `pl cat <n>` (the file's bytes, with non-printing bytes escaped), `pl new <name> [track ...]`, `pl add <n> <track> [track ...]`, `pl rm <n> <entry>`, `pl up <n> <entry>`, `pl down <n> <entry>`, `pl mv <n> <name>`, `pl del <n>`. They use the same requests as the UI and print the result event. A temporary command for writing raw bytes to a file, used to construct computer-made files and power-cut cases, is not committed.

Board checks:
1. `pl new ROAD TRIP` with five tracks from `/lib/`: `pl ls` lists it; `pl cat` shows five `../lib/...` lines, LF only; `pl show` shows the five tracks.
2. Each edit: `pl add`, `pl rm` (first, middle, last), `pl up`, `pl down` (including past either end: refused, file unchanged), `pl mv`, `pl del`; after each `pl cat` shows exactly the change.
3. A computer-made file (constructed with the temporary command) with a BOM, CRLF, `#EXTM3U`, `#EXTINF` lines, a Windows absolute path with a drive letter and backslashes, a `file:///` URL with `%20`, a path relative to `/playlists/`, a path in the wrong case, and two lines that match nothing: `pl show` resolves the real ones and shows two missing; `pl up`/`pl rm` on it change only the edited block (`pl cat` before and after).
4. Names: `pl new` with each bad name and with a name in use (`rock.m3u8` constructed, then `ROCK`): refused with the right result. Rename to its own name: done.
5. Full: a playlist of 1,000 entries (`pl new` from `/lib1000/` in bursts), then `pl add` one more: `playlist full`, file unchanged. A constructed file of 1,200 lines: `pl show` lists 1,000 and says truncated.
6. Stale index: a request with a checksum that is not the index's (temporary hook) is refused `stale index` and the file is unchanged.
7. Power cuts, constructed as in the library task's analysis: only `<file>.tmp` present; both present; a truncated `.tmp`: after `pl ls` each case is sane (the right file, no `.tmp`, no stray playlist).
8. Burst: 16 `pl up`/`pl down` requests sent back to back on a 1,000-entry playlist: all accepted, applied in order (`pl show` matches the expected order).
9. Saving not heard: the music track from start to end while a script makes edits of a 1,000-entry playlist every few seconds: `ENDED frames=13453440 expected=13453440`, `underruns=0`. Report how long a load, a listing and an edit of a 1,000-entry playlist take (typical and worst), the PSRAM used, free PSRAM after start-up, and the internal RAM used.
10. Leave the card with no test playlists (delete them) and say so.

- [ ] **Step 1:** Implement the task, saving, the console commands. Build without warnings.
- [ ] **Step 2:** Board checks; remove temporary commands; re-verify a normal boot. Commit: `Add the playlist task: list, load and edit playlists off the UI task`.

---

### Task 5: Playlists in the UI

**Files:**
- Modify: `main/ui.c`, `main/ui_play.c`, `main/ui_options.c`, `main/ui_screens.c`, `main/ui_state.h`, `main/ui.h`, `main/debug_console.c/.h` if a report needs it
- Create (if `ui.c` or `ui_options.c` would become unwieldy): `main/ui_playlists.c` (playlist screens, picker, confirmation) and `main/ui_text.c` (the text entry screen), each with one responsibility

**Interfaces:**
- Consumes: everything from Tasks 1-4; `play_queue_load_tracks`; the stage 6 queue and state rules (the stage 6 plan's amendments); play numbers.
- Produces: the stage's behaviour on the device.

Required behaviour:
- **Playlists screen** (`Main` > `Playlists`): `New playlist`, then the playlists A to Z, with letter jumps. Entering it asks for a listing; until the first listing arrives the list shows `Loading`. `5`/`6` on `New playlist` opens text entry; on a playlist opens it.
- **A playlist** shows its entries (title, or `(missing)`) with the playlist's name and position on the top row; `Loading` until its entries arrive. `5`/`6` on an entry plays the playlist from there (the queue is loaded from the playable entries, `play_queue_load_tracks`); on a missing entry it shows `File missing` for 1.5 s and does nothing else. An empty playlist shows `(empty)`.
- **Options (`#`)**, using the stage 6 options overlay:
  - track, album, artist rows: the stage 6 actions plus `Add to playlist`;
  - a playlist row: `Play` and `Shuffle play` (load its entries, then play from the first playable entry, or turn shuffle on and start from a random playable one; `Nothing to play` if none), `Rename` (text entry starting with its name), `Delete` (confirmation);
  - an entry: `Play`; `Move up`/`Move down`, after which the options stay open on the moved entry so the next `5` moves it again, and the list under them follows it (at either end the move does nothing); `Remove`, after which the options close and the cursor stays at the same position (or the last entry);
  - Now Playing: `Add to playlist` (the current track; nothing to add when there is no current track: `Nothing playing`), `Save queue` (text entry; saves the queue in the order the tracks were added, the first 1,000 if longer, saying `Saved first 1000`; `Queue empty` if empty).
- **The picker** (`Add to playlist`): over the current screen, the top row `Add to playlist`, the bottom row `>New...`, then the playlists A to Z; `2`/`8` move, `5`/`6` choose, `4`, `*` or `#` close. Choosing a playlist appends the tracks (a track: one; an album or an artist: all its tracks, refused at once with `Playlist full` if there are more than 1,000); `New...` opens text entry and creates the playlist with them. The UI builds the track numbers when the picker opens and sends them with the index's checksum.
- **Text entry screen:** top row `Name:`, bottom row `textentry_row`. Only key presses type (long presses and repeats do nothing); `A`, `B` and `D` keep their global meaning; `C` cancels back to where it was opened. `#` checks the name: `Name empty`, `Name too long`, `Bad character`, `Can't end in .`, `Can't start w/ .` or `Name in use` shows for 1.5 s and typing goes on; otherwise the request is sent and the screen returns to where it was opened. The listing used for `Name in use` is asked for when text entry opens.
- **Delete confirmation:** top row `Delete` and the name (a marquee if long), bottom row `>No` or `>Yes`, starting on `No`; `2`/`8` move, `5`/`6` choose, `4`/`*` cancel.
- **Results** of requests show for 1.5 s on the bottom row through the overlay mechanism: `Saved`, `Added`, `Renamed`, `Deleted`, `Playlist full`, `Name in use`, `Card error`, `Library busy`, `Try again` (stale index), `Busy` (a request refused). After a create, rename or delete the catalog is listed again; the Playlists screen keeps its cursor on the playlist just created or renamed. After a move or remove, the UI's copy of the entries changes at once; if the request fails, the playlist is loaded again from the card.
- **A new index:** stage 5's rule holds (back to the main menu); a picker or a pending play holding track numbers of the old index is closed, and a text entry for `Save queue` or `New...` with tracks is cancelled with `Try again`. No old track number reaches a request (the checksum check in Task 4 is the second line of defence, not the first).
- **The open playlist is identified by its file name.** A listing that no longer has it goes back to the Playlists screen.
- The UI loop's rules hold: one index take per key press, all waiting events applied before one redraw, no polling (the text entry's commit time is one more deadline the loop already waits for), no allocation, nothing blocking on the card.
- The catalog copy, the open playlist's entries and the picker's track numbers are allocated once at start-up, in PSRAM; say how big.

Board checks:
1. `Playlists` from the main menu: `Loading` then `New playlist` alone (no folder yet). `New playlist`, type `ROAD TRIP` with key presses (`7 7 7` > `R`, and so on; use `screen` to check each step and the commit after 1 s), `#`: `Saved`, the list shows `ROAD TRIP` with the cursor on it.
2. `Add to playlist` from a track, an album of `/lib/` and Now Playing into `ROAD TRIP`: `Added`; open it: the entries in order. `New...` from the picker on an artist of `/lib1000/` (100 tracks): a new playlist with 100 entries.
3. In a playlist: `Move up` five times in a row with the options open: the entry climbs five places, `pl show` matches the screen. `Move down` at the bottom does nothing. `Remove` the middle entry: gone on screen and in `pl show`.
4. `Play` on a playlist row and selecting an entry: `queue` shows the playable entries in order, starting at the right one; `Shuffle play`: shuffle on, the playlist's tracks. A playlist with a missing entry (computer-made, constructed as in Task 4): the missing entry shows `(missing)`, selecting it shows `File missing`, playing skips it.
5. `Rename` to a bad name, to a name in use, to its own name, and to a new name: each says the right thing; the list follows. `Delete` with `No` then `Yes`: gone.
6. Text entry: `C` cancels; `*` drops a pending letter and deletes a committed one; the 16-character limit; a held key does not cycle.
7. `Save queue` with a queue of `Songs` (1,028): `Saved first 1000`; the playlist has 1,000 entries in the queue's order.
8. A rescan with the picker open and with a playlist open: back to the main menu, nothing written; then the same operations work again.
9. Saving not heard: the music track from start to end while a script adds to and reorders playlists every few seconds: exact `ENDED`, `underruns=0`.
10. Stage 5 and 6 behaviour still holds: menus, letter jumps, `hold 8 500` on Songs in about the same time as before, next/previous/shuffle/loop, options, resume at power-on.
11. Leave the card with no test playlists and the saved state cleared, and say so.

- [ ] **Step 1:** Playlists screen and a playlist's entries; playing from them.
- [ ] **Step 2:** Options, picker, delete confirmation, Now Playing options.
- [ ] **Step 3:** Text entry screen; create, rename, save queue.
- [ ] **Step 4:** Board checks. Commit (one or several, each buildable): final subject `UI: playlists: screens, picker, text entry, edits and save queue`.

---

## Stage 7 hardware check (run by the user)

On the phone charger.

1. **Create:** `Playlists` > `New playlist`, type a name by multi-tap (letters cycle, a pause or another key commits, `*` deletes, `#` saves).
2. **Fill:** `#` > `Add to playlist` on a track, an album and from Now Playing; `New...` from the picker.
3. **Reorder:** in the playlist, `#` > `Move up`/`Move down` (repeat with `5`), `Remove`.
4. **Rename and delete:** `#` on a playlist > `Rename`, then `Delete` (answer `No`, then `Yes`).
5. **Play:** `Play` and `Shuffle play` on a playlist; select an entry to play from there.
6. **Save queue:** on Now Playing, `#` > `Save queue`.
7. **A computer-made M3U loads:** make one on a computer (from a music player's export, or by hand with paths like `E:\music\...` or `../music/...`), put it in `/playlists/` on the card: it is listed, its tracks play, anything not on the card shows `(missing)`.
8. **A device-made one opens on the computer:** put the card in the computer and open a playlist the player made in a music player there: the tracks are found.
9. **Nothing is heard when it saves:** no click while editing playlists during playback.
