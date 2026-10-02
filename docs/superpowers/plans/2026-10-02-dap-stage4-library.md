# DAP Stage 4 (Tags and Library) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read the tags of every FLAC and MP3 on the card, build an index of artists, albums and songs that loads in well under a second at boot, and show the playing track's title and artist on the LCD as a marquee.

**Architecture:** Three hardware-free modules do the work and are tested on the host: `tags` reads one file's tags and length; `library` builds, saves, loads and queries the index inside a caller-supplied block of memory; `libscan` walks a directory tree and feeds the two together. On the device a scan task runs `libscan` into a PSRAM arena and reports progress to the UI task. A small `marquee` module computes which 16 characters of a long text are visible at a given moment.

**Tech Stack:** ESP-IDF v6.0.2 (C), POSIX `dirent` for the directory walk (available on the device through the VFS and on the host), Unity host tests under AddressSanitizer, ffmpeg/flac/metaflac for fixtures.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read section 3 "Performance rules", sections 5 and 6 in full, section 9 "Screens", section 11, and stage 4 of section 14).

**How this plan works:** as in stage 3, this plan fixes interfaces, behaviour, test cases and acceptance checks; implementation bodies and test code are the implementer's. Headers given here are binding. If an interface cannot be implemented as written, stop and report NEEDS_CONTEXT; do not change it silently.

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port a cold plug-in leaves the SD card unresponsive until the user re-inserts it. The firmware now shows `Insert SD card` and retries every 2 seconds, so the log will show `Mount attempt N failed`. If that goes on for more than a minute, STOP and report BLOCKED: only the user can re-insert the card.
- **Player facts that must not be disturbed** (`main/player.c`; see its design notes and `docs/superpowers/plans/stage3-*-investigation.md`): the decode loop's stall guard, the `eos` handling, the registered FLAC parser wrapper, the pinned codec library version. Do not modify `main/player.c` in this stage except where a task says so.
- Every FLAC that plays from start to end must still log `ENDED frames=X expected=X` (tag `player`). The music track is `/sdcard/music/16 - Kanye West - Hey Mama.flac` (13453440 frames, 305 s). Test tones are in `/sdcard/test/`: `t01_44k_16_st.flac` (441000 frames), `t06_tone_30s.flac` (1323000), and others.
- **Text for the LCD:** the index stores display text already converted with `translit` (ASCII only). File paths are stored exactly as on disk (UTF-8), because they are needed to open the file.
- **Fallbacks (spec section 5):** no title: the file name without extension. No artist: `Unknown Artist`. No album: `Unknown Album`. If an album-artist tag is present it is used for grouping instead of the artist.
- **Ordering (spec section 6):** artists, albums and songs A to Z ignoring case. Within an album: disc number, then track number, then file name.
- **Limits:** design target 5,000 tracks. The index lives in one arena of 1.5 MB (1572864 bytes) of PSRAM. If the arena fills during a scan, the scan stops, keeps what it has, saves it, and reports "Library full".
- **Index file:** `/.dap/library.idx` on the card, written to `/.dap/library.idx.tmp` first. A bad magic, version or CRC at load time means "no usable index".
- **Scan:** walks the whole card; skips any directory whose name starts with `.` and `System Volume Information`; takes `.flac` and `.mp3` files (case-insensitive). Playback is stopped before a scan starts.
- **Performance rules (spec section 3):** nothing the user does at the keypad may scan all tracks; lists are contiguous ranges of arrays sorted once at build time; lookups are O(log n) or O(1); building is O(n log n) with hash-table de-duplication of names; strings are stored once in a pool and referred to by offset; no `malloc` per track; tag reading touches only header bytes and seeks past pictures; files are read and written in large blocks. The LCD driver already sends only changed characters.
- **Tasks:** UI task priority 5 core 0 (owns the LCD; the only task that may call `lcd_*`). Scan task priority 3, core 0, 8 KB stack. No busy-waiting anywhere; a wait with a timeout on a queue is fine.
- `components/dap_core` is pure C using only the C standard library. The new `components/dap_fs` may also use POSIX `dirent.h` and `sys/stat.h`, and must build on the host as well as the device.
- Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan, and must not allocate from the heap (static buffers and `tmpfile()` are fine).
- `MINIMAL_BUILD` stays on; each component lists what it uses in `REQUIRES`/`PRIV_REQUIRES`.
- No compiler warnings from `main/` or `components/`. Code style as existing: 2-space indent, `static const char *TAG`, comments that say *why*.
- Nothing that can fail at run time because of a file or the card may use `ESP_ERROR_CHECK`, abort or restart.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- Agents cannot hear the output or see the LCD. The `screen` console command (Task 1) prints what the LCD has been told to show; that is the nearest an agent gets. Anything only a person can judge goes in the hardware check; do not claim it.
- Tools: `tools/serial_script.py <port> [--no-reset] <step>...` with steps `expect:<text>[:<timeout_s>]`, `send:<line>`, `sleep:<seconds>` (pipe `2>&1`). The firmware's debug console accepts `play <path> [start_ms]`, `pause`, `resume`, `stop`, `vol <n>`, `seek <ms>`, `key <char>`, `long <char>`, `stat`, and replies with `dbg: ok ...` or `dbg: err ...`.

## Review Focus

1. **Tags in the wild are messy:** UTF-16 text with and without a byte-order mark, a tag frame longer than the buffer, a size field that points past the end of the file, an embedded picture of several megabytes, empty strings, a track number written `3/12`, a file with both ID3v2 and ID3v1. None may crash, hang, read out of bounds, or cost more than a few reads. Pinned in Task 3 by named tests and by AddressSanitizer.
2. **A file that is not what its extension says,** or is zero bytes, or is cut off. The scan must skip or fall back, never stop. Pinned in Tasks 3 and 5.
3. **Two artists or albums that differ only in case or surrounding spaces** ("Radiohead" and "radiohead ") must group together, and two different artists' albums with the same name ("Greatest Hits") must not. Pinned in Task 4.
4. **A full arena** must end the scan cleanly with a usable, saved index. Pinned in Task 4 (host, at scale) and Task 5.
5. **A damaged or half-written index file** (power pulled during a save) must be rejected at load and trigger a rescan, never be trusted. Pinned in Task 4 by corrupting every region of a saved file.
6. **Very long names and deep folders:** a path longer than the buffer, a folder tree deeper than the limit, a name with non-Latin characters. Skipped and counted, not truncated into a wrong path. Pinned in Task 5.
7. **Scale:** 5,000 tracks must fit the arena, build in a time that is reported, and answer every query without a linear scan. Pinned in Task 4 on the host and measured on the board in Task 5 with 1,000 real files.
8. **A marquee that never rests or never moves:** text of exactly 16 characters must not scroll; 17 must; an empty title must not crash; the screen must not be redrawn when nothing changed. Pinned in Task 1.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `components/dap_core/marquee.c/.h` | Which characters of a long text are visible at a given time | 1 |
| `main/app_main.c`, `main/debug_console.c` | Marquee on the playing screen; `screen` console command | 1 |
| `tools/make_test_audio.sh` | Tagged fixtures and two on-card test libraries | 2 |
| `test/host/fixtures/tags/`, `test/host/fixtures/lib/` | Tag fixtures; a small tagged library tree (committed) | 2 |
| `components/dap_core/tags.c/.h` | Read one file's tags and length | 3 |
| `components/dap_core/library.c/.h` | Build, save, load and query the index | 4 |
| `components/dap_fs/libscan.c/.h` | Walk a tree, read tags, feed the index builder | 5 |
| `main/library_task.c/.h` | Load or scan at boot; scan task; progress events | 5 |
| `main/app_main.c`, `main/debug_console.c` | Scan progress screen; `scan` and `lib` console commands; title and artist from the index | 5, 6 |

---

### Task 1: Marquee on the playing screen, and a `screen` console command

**Files:**
- Create: `components/dap_core/marquee.h`, `components/dap_core/marquee.c`
- Test: `test/host/test_marquee.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`, `main/app_main.c`, `main/debug_console.c`

**Interfaces:**
- Produces (`marquee.h`, binding):

```c
#pragma once

#include <stddef.h>
#include <stdint.h>

// A marquee: text too long for its space moves through it from right to left,
// round and round, with a gap between the end and the start, and rests for a
// moment each time the start comes back to the left edge. Text that fits does
// not move.
//
// It is a pure function of elapsed time: nothing to keep between calls.

#define MARQUEE_REST_MS 1500 // how long it rests with the start at the left
#define MARQUEE_STEP_MS 350  // time per one-character step
#define MARQUEE_GAP 3        // spaces between the end and the start coming round

// Write exactly `width` characters to `out` (no terminator): the part of
// `text` visible `elapsed_ms` after it was first shown. Text of `width`
// characters or fewer is shown as it is, padded with spaces. `text` may be
// NULL or empty (all spaces).
void marquee_window(const char *text, size_t width, uint32_t elapsed_ms,
                    char *out);

// How many milliseconds from `elapsed_ms` until the window next changes, or
// UINT32_MAX if it never will (the text fits). Lets the caller sleep until
// there is something new to draw.
uint32_t marquee_next_change_ms(const char *text, size_t width,
                                uint32_t elapsed_ms);
```

Behaviour: one cycle is a rest of `MARQUEE_REST_MS` at offset 0, then `len + MARQUEE_GAP` steps of `MARQUEE_STEP_MS`, the last of which arrives back at offset 0, where the next cycle's rest begins. At offset `k` the window shows characters `k, k+1, ...` of the text followed by the gap followed by the text again, wrapping. No division inside a per-character loop.

Required tests (binding names):

| Test | Expectation |
|---|---|
| `test_short_text_is_padded_and_never_moves` | `"Kid A"`, width 16: `"Kid A           "` at 0 ms and at 60000 ms; next change is `UINT32_MAX` |
| `test_text_of_exactly_the_width_does_not_move` | 16 characters, width 16: unchanged at any time |
| `test_one_character_too_long_moves` | 17 characters: at 0 ms the first 16; after the rest plus one step, characters 1..16 |
| `test_it_rests_at_the_start` | long text: same window at 0, 1 and `MARQUEE_REST_MS - 1` ms; different at `MARQUEE_REST_MS` |
| `test_each_step_moves_one_character` | windows at rest + k steps equal the text rotated by k, for k = 1..5 |
| `test_the_gap_separates_end_and_start` | when the text's last character is at column 0, columns 1..3 are spaces and column 4 is the text's first character |
| `test_it_comes_round_and_rests_again` | at one full cycle the window equals the window at 0 ms, and stays for `MARQUEE_REST_MS` |
| `test_next_change_during_rest_and_during_steps` | during the rest: time left of the rest; mid-step: time left of the step; exact at boundaries |
| `test_null_and_empty_text` | all spaces; next change `UINT32_MAX`; no crash |
| `test_width_one_and_width_zero` | width 1 shows one character at a time; width 0 writes nothing |
| `test_elapsed_time_near_wraparound` | `elapsed_ms` of `UINT32_MAX` gives a valid window and a next change of at least 1 |

**On the playing screen** (`main/app_main.c`): the top row shows the track's name through the marquee (16 columns), restarting from the beginning whenever a new track starts. The UI task must not poll: it waits on its queue set with a timeout equal to the marquee's next change (and for as long as it likes when the text fits), and redraws only when the window or the bottom row changed. While paused the marquee keeps moving. The bottom row is unchanged.

**`screen` console command** (`main/debug_console.c`): logs the two rows the UI task last sent to the LCD, each in quotes, as `dbg: screen "<16 chars>" "<16 chars>"`, with custom-character codes (below 0x20) shown as `?`. It must read the UI task's copy safely (the UI task owns it): for example a snapshot the UI task publishes under a short lock, or a request the UI task answers. This is how agents check what the LCD shows.

Board checks (via `tools/serial_script.py`):
1. After boot, `screen` shows the start of the music track's name on the top row and a time on the bottom row. Sampling `screen` every 400 ms for 12 s shows the top row resting, then stepping one character at a time, the gap coming through, and the start returning.
2. `play /sdcard/test/t01_44k_16_st.flac`: the top row restarts at the new name's first character. Also show a name that fits and does not move, if any test file's name is 16 characters or shorter; if none is, say so.
3. With the music track playing, count the UI task's LCD updates over 20 s (a temporary counter is fine, not committed): about one per marquee step plus two per second for the clock, not more.
4. The music track from start to end still gives `ENDED frames=13453440 expected=13453440` with no `UNDERRUN` (the LCD traffic must not disturb audio).

- [ ] **Step 1:** `marquee`: header, stub, tests; fail; implement; pass. Add to the firmware build.
- [ ] **Step 2:** Playing screen and `screen` command. Build without warnings.
- [ ] **Step 3:** Board checks. Commit: `Marquee for long titles on the playing screen; add the screen console command`.

---

### Task 2: Tagged fixtures and test libraries

**Files:**
- Modify: `tools/make_test_audio.sh`, `.gitignore`
- Create (generated, committed): `test/host/fixtures/tags/*`, `test/host/fixtures/lib/**`

**Interfaces:**
- Produces `test/host/fixtures/tags/` (each file under 64 KB unless stated; audio is 0.1 s of the usual tone):

| File | What it is |
|---|---|
| `flac_full.flac` | Vorbis comments: TITLE=`Idioteque`, ARTIST=`Radiohead`, ALBUM=`Kid A`, ALBUMARTIST=`Radiohead`, TRACKNUMBER=`8`, DISCNUMBER=`1` |
| `flac_lowercase_keys.flac` | same values with keys in lower and mixed case (`title=`, `Artist=`) |
| `flac_track_of_total.flac` | TRACKNUMBER=`3/12`, DISCNUMBER=`2/2` |
| `flac_utf8.flac` | TITLE=`Sigur Rós – Hoppípolla`, ARTIST=`Björk`, ALBUM=`你好` |
| `flac_no_tags.flac` | no Vorbis comment block at all |
| `flac_empty_values.flac` | TITLE= and ARTIST= present but empty |
| `flac_picture_first.flac` | a 200 KB PICTURE block before the Vorbis comment block (this one may be up to 260 KB) |
| `flac_long_title.flac` | a 600-character TITLE |
| `mp3_id3v23_latin1.mp3` | ID3v2.3, text encoding 0: TIT2, TPE1, TALB, TPE2, TRCK=`5/10`, TPOS=`1/2` |
| `mp3_id3v23_utf16.mp3` | ID3v2.3, text encoding 1 (UTF-16 with BOM), accented and non-Latin text |
| `mp3_id3v24_utf8.mp3` | ID3v2.4, text encoding 3, sync-safe frame sizes |
| `mp3_id3v24_utf16be.mp3` | ID3v2.4, text encoding 2 (UTF-16 big-endian, no BOM) |
| `mp3_id3v1_only.mp3` | no ID3v2; an ID3v1.1 tag (title, artist, album, track 7) |
| `mp3_both.mp3` | ID3v2.4 and ID3v1 with different titles (v2 must win) |
| `mp3_no_tags.mp3` | no tags; has a Xing/Info header |
| `mp3_no_xing_cbr.mp3` | no tags, no Xing/Info header, constant 128 kbps |
| `mp3_apic.mp3` | ID3v2.3 with a 200 KB APIC picture frame before the text frames (up to 260 KB) |
| `mp3_bad_size.mp3` | an ID3v2 header whose size points past the end of the file |

- Produces `test/host/fixtures/lib/`: a small library tree, committed, total under 200 KB, used by host tests and copied to the card. Exactly these 14 audio files plus the non-audio ones (0.1 s tones; tags as listed):

| Path | Title / Artist / Album / Album artist / Disc / Track |
|---|---|
| `Radiohead/Kid A/01 Everything.flac` | Everything In Its Right Place / Radiohead / Kid A / – / 1 / 1 |
| `Radiohead/Kid A/02 Kid A.flac` | Kid A / Radiohead / Kid A / – / 1 / 2 |
| `Radiohead/Kid A/10 Motion.flac` | Motion Picture Soundtrack / Radiohead / Kid A / – / 1 / 10 |
| `Radiohead/OK Computer/01 Airbag.mp3` | Airbag / Radiohead / OK Computer / – / – / 1 |
| `Radiohead/OK Computer/02 Paranoid.mp3` | Paranoid Android / radiohead (lower case) / OK Computer / – / – / 2 |
| `Bjork/Homogenic/01 Hunter.flac` | Hunter / Björk / Homogenic / – / – / 1 |
| `Bjork/Homogenic/02 Joga.flac` | Jóga / Björk / Homogenic / – / – / 2 |
| `Various/Hits/01 a.flac` | Alpha / The Alphas / Greatest Hits / Various Artists / 1 / 1 |
| `Various/Hits/02 b.flac` | Beta / The Betas / Greatest Hits / Various Artists / 1 / 2 |
| `Various/Hits/01 c.flac` | Gamma / The Gammas / Greatest Hits / Various Artists / 2 / 1 |
| `Queen/Hits/01 Bohemian.mp3` | Bohemian Rhapsody / Queen / Greatest Hits / – / – / 1 |
| `loose/untagged song.flac` | (no tags) |
| `loose/also untagged.mp3` | (no tags) |
| `deep/a/b/c/d/Deep Track.flac` | Deep Track / Deep Artist / Deep Album / – / – / 1 |
| `.hidden/secret.flac` | tagged, but must be skipped |
| `System Volume Information/x.flac` | tagged, but must be skipped |
| `notes.txt`, `cover.jpg` (tiny), `loose/empty.flac` (0 bytes), `loose/text.mp3` (text) | must be skipped or fall back without stopping the scan |

- Produces, not committed (`test-audio/lib1000/`): 1,000 tagged 0.1 s FLAC files in 10 artists x 10 albums x 10 tracks (`Artist 00/Album 00/01 Track.flac`...), for measuring scan speed on the board. Under 4 MB in total.
- The existing files and fixtures must stay byte-identical. The script keeps checking its own output and fails loudly.

**Then a pause for the user:** copy `test/host/fixtures/lib` to the card as `/testlib/` and `test-audio/lib1000` as `/lib1000/`. (`/lib1000/` is for measurement and can be deleted from the card after this stage.)

- [ ] **Step 1:** Extend the generator; run it; verify every fixture structurally (`metaflac --list`, a hex dump of each ID3 header and text frame, `ffprobe`) and put the evidence in the report.
- [ ] **Step 2:** Commit: `Test audio: tag fixtures and two test libraries`.

---

### Task 3: `tags`

**Files:**
- Create: `components/dap_core/tags.h`, `components/dap_core/tags.c`
- Test: `test/host/test_tags.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Consumes: `flac_header` (stage 3), `id3` and `mp3_xing` (stage 3), fixtures from Task 2.
- Produces (`tags.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TAGS_TEXT_MAX 128 // bytes per field, including the terminator

typedef enum {
  TAGS_FORMAT_FLAC,
  TAGS_FORMAT_MP3,
} tags_format_t;

// What a file says about itself. Text is UTF-8, exactly as stored in the
// file apart from being cut to TAGS_TEXT_MAX - 1 bytes at a character
// boundary; a field the file does not have is an empty string. Converting
// for the LCD and filling in "Unknown Artist" is the caller's business.
typedef struct {
  char title[TAGS_TEXT_MAX];
  char artist[TAGS_TEXT_MAX];
  char album[TAGS_TEXT_MAX];
  char album_artist[TAGS_TEXT_MAX];
  uint16_t track_no;    // 0 if not given
  uint16_t disc_no;     // 0 if not given
  uint32_t duration_ms; // 0 if not known
} tags_t;

// Read tags from an open file of the given format. Returns false only if the
// file could not be read at all or is plainly not that format; a file with no
// tags returns true with empty fields. Leaves the file position unspecified.
bool tags_read(FILE *f, tags_format_t format, tags_t *out);

// The format a file name's extension claims (.flac, .mp3; any case).
// Returns false for anything else.
bool tags_format_from_name(const char *name, tags_format_t *format);
```

What to read:
- **FLAC:** the VORBIS_COMMENT block (type 4): a little-endian vendor string, a count, then `KEY=value` entries each with a little-endian length. Keys are case-insensitive: TITLE, ARTIST, ALBUM, ALBUMARTIST (also `ALBUM ARTIST`), TRACKNUMBER, DISCNUMBER. Duration from STREAMINFO via `flac_header`. If a key appears more than once, the first wins.
- **MP3:** ID3v2.3 and ID3v2.4 at the start of the file: frames TIT2, TPE1, TALB, TPE2, TRCK, TPOS. Text encodings 0 (ISO-8859-1), 1 (UTF-16 with BOM), 2 (UTF-16BE), 3 (UTF-8), all converted to UTF-8. Frame sizes are plain 32-bit in v2.3 and sync-safe in v2.4. Skip an extended header. A tag with the unsynchronisation flag set, or of any other version, is treated as absent. Then, for any field still empty, ID3v1 or v1.1 in the last 128 bytes (ISO-8859-1, space or NUL padded). Duration from the Xing/Info header if present, otherwise from the first frame's bitrate and the audio byte count (file size minus tags).
- Numbers: `TRCK`/`TRACKNUMBER` of the form `3/12` gives 3; leading zeros and spaces are tolerated; anything unparseable gives 0.

Performance: bounded reads only. Never read a picture: step over any block or frame that is not wanted with `fseek`. A Vorbis comment or text frame longer than the field is read only as far as needed. Report, for `flac_picture_first.flac` and `mp3_apic.mp3`, the bytes read and the number of reads and seeks (the stage 3 `flac_header` test shows one way to count them).

Required tests (binding names; expectations from the Task 2 tables):

`test_flac_full`, `test_flac_keys_are_case_insensitive`, `test_flac_track_of_total`, `test_flac_utf8_is_kept_as_utf8`, `test_flac_without_tags_gives_empty_fields_and_a_duration`, `test_flac_empty_values`, `test_flac_picture_is_stepped_over` (asserts bytes read under 2 KB), `test_flac_long_title_is_cut_at_a_character_boundary` (127 bytes or fewer, valid UTF-8, a prefix of the original), `test_mp3_id3v23_latin1`, `test_mp3_id3v23_utf16`, `test_mp3_id3v24_utf8`, `test_mp3_id3v24_utf16be`, `test_mp3_id3v1_only`, `test_mp3_id3v2_wins_over_v1`, `test_mp3_without_tags_has_a_duration_from_xing`, `test_mp3_cbr_duration_without_xing` (within 2% of 100 ms), `test_mp3_apic_is_stepped_over` (bytes read under 4 KB), `test_mp3_bad_tag_size_does_not_read_out_of_bounds` (returns, with whatever it could read or empty fields), `test_wrong_format_returns_false` (a FLAC read as MP3 with no MP3 frame, text read as FLAC), `test_empty_file_returns_false`, `test_format_from_name` (`a.flac`, `A.FLAC`, `b.Mp3`, `c.txt`, `noext`, `.flac`).

- [ ] **Step 1:** Header, stub, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Implement. Pass, nothing on stderr.
- [ ] **Step 3:** Add to the firmware build; no warnings. Commit: `Add tags: read title, artist, album and length from FLAC and MP3`.

---

### Task 4: `library`, the index

**Files:**
- Create: `components/dap_core/library.h`, `components/dap_core/library.c`
- Test: `test/host/test_library.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Produces (`library.h`, binding):

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The music library index: every track with its title, artist and album,
// grouped and sorted so that each list the player shows is a contiguous
// range. It is built once by a scan, saved to the card, and loaded whole.
//
// All of it lives in one block of memory (the arena) that the caller
// supplies; nothing here allocates. After a build or a load the arena holds
// the index in the same form as the file, so loading is one read and a
// checksum.

typedef enum {
  LIBRARY_OK,
  LIBRARY_FULL,     // the arena cannot hold any more
  LIBRARY_BAD_FILE, // not an index: wrong magic, version, size or checksum
  LIBRARY_IO,       // reading or writing failed
} library_result_t;

// One track as the scan hands it over. Display text is already converted for
// the LCD; `path` is the file's path as on disk, relative to the scan root,
// with '/' separators.
typedef struct {
  const char *path;
  const char *title;
  const char *artist;       // the track's own artist, for display
  const char *album;
  const char *group_artist; // the artist the album is filed under
  uint16_t track_no;
  uint16_t disc_no;
  uint32_t duration_ms;
  uint8_t format; // tags_format_t
} library_track_in_t;

typedef struct library_builder library_builder_t; // lives inside the arena
typedef struct library library_t;                 // lives inside the arena

// Start building in `arena`. Returns NULL if the arena is too small to
// start.
library_builder_t *library_builder_begin(void *arena, size_t arena_bytes);

// Add one track. LIBRARY_FULL means it was not added and nothing more will
// fit; what was added before is intact and can still be finished.
library_result_t library_builder_add(library_builder_t *b,
                                     const library_track_in_t *track);

// Group, sort and lay the index out in its final form at the start of the
// arena. The builder is finished after this.
const library_t *library_builder_finish(library_builder_t *b);

// Save to an open file, or load one into `arena`. A loaded index is checked
// before it is trusted.
library_result_t library_save(const library_t *lib, FILE *f);
library_result_t library_load(void *arena, size_t arena_bytes, FILE *f,
                              const library_t **out);

uint32_t library_bytes(const library_t *lib); // size of the index
uint32_t library_track_count(const library_t *lib);
uint32_t library_artist_count(const library_t *lib);
uint32_t library_album_count(const library_t *lib);

// Artists are numbered in A to Z order. An artist's albums are a contiguous
// range of album numbers, A to Z within the artist; its tracks are a
// contiguous range of track numbers.
const char *library_artist_name(const library_t *lib, uint32_t artist);
void library_artist_albums(const library_t *lib, uint32_t artist,
                           uint32_t *first_album, uint32_t *count);
void library_artist_tracks(const library_t *lib, uint32_t artist,
                           uint32_t *first_track, uint32_t *count);

// Albums are numbered by artist, then name. An album's tracks are a
// contiguous range, ordered by disc, track number, then file name.
const char *library_album_name(const library_t *lib, uint32_t album);
uint32_t library_album_artist(const library_t *lib, uint32_t album);
void library_album_tracks(const library_t *lib, uint32_t album,
                          uint32_t *first_track, uint32_t *count);

// The n-th album, and the n-th song, in A to Z order over the whole library.
uint32_t library_album_by_name(const library_t *lib, uint32_t n);
uint32_t library_song_by_title(const library_t *lib, uint32_t n);

typedef struct {
  const char *title;
  const char *artist; // the track's own artist
  uint32_t album;
  uint32_t group_artist;
  uint16_t track_no;
  uint16_t disc_no;
  uint32_t duration_ms;
  uint8_t format;
} library_track_t;

// False if `track` is out of range.
bool library_track(const library_t *lib, uint32_t track, library_track_t *out);

// Write the track's path (as given to the builder) into `out`. Returns the
// length it needs, like snprintf; 0 if `track` is out of range.
size_t library_track_path(const library_t *lib, uint32_t track, char *out,
                          size_t out_size);

// Find a track by its path. O(log n).
bool library_find_path(const library_t *lib, const char *path,
                       uint32_t *track);
```

Required properties:
- **Grouping:** artists are matched ignoring case and leading or trailing spaces; the name kept is the first one seen. Albums are matched the same way *within* a group artist: the same album name under two artists is two albums.
- **Sorting:** ignoring case; ties broken so that the order is always the same for the same input, whatever order the tracks were added in. Add the same tracks in two different orders and the saved files must be byte-identical.
- **Memory:** nothing but the arena. Directory names are stored once per directory, not once per track. Strings are stored once. A realistic library of 5,000 tracks (paths about 70 bytes, titles 25, 500 albums, 150 artists) must build inside 1.5 MB; report bytes per track for the finished index and the peak arena use during the build.
- **Speed:** building is O(n log n): names are de-duplicated through a hash table, and sorting uses `qsort` or an equivalent. No query loops over all tracks; ranges come from stored offsets, `library_find_path` is a binary search or a hash lookup. Report the host build time for 5,000 tracks.
- **Trust nothing in a file:** `library_load` checks magic, version, the stated size against the arena and the file, and a CRC-32 of the body, and then every count and offset it will later follow, so that a file that passes the checksum but is internally inconsistent still cannot make a query read outside the arena.
- The index refers to things by offset, not by pointer, so the same bytes are valid wherever the arena is.

Required tests (binding names):

`test_empty_library`, `test_one_track`, `test_artists_are_sorted_ignoring_case`, `test_artist_names_differing_in_case_or_spaces_are_one_artist`, `test_first_spelling_seen_is_kept`, `test_albums_group_under_their_artist`, `test_same_album_name_under_two_artists_is_two_albums`, `test_compilation_is_one_album_under_its_group_artist` (three track artists, one album, each track keeps its own artist), `test_album_tracks_are_ordered_by_disc_then_track_then_file_name`, `test_tracks_without_numbers_fall_back_to_file_name_order`, `test_artist_tracks_are_a_contiguous_range`, `test_albums_by_name_and_songs_by_title_are_sorted`, `test_track_path_round_trips` (including a path with spaces, UTF-8 and a deep folder), `test_track_path_reports_needed_length_when_the_buffer_is_small`, `test_find_path_finds_every_track_and_rejects_others`, `test_out_of_range_numbers_are_safe`, `test_order_of_adding_does_not_change_the_result` (byte-identical saves), `test_save_and_load_round_trip` (every query gives the same answer from the loaded copy, in a different arena), `test_load_rejects_bad_magic`, `test_load_rejects_wrong_version`, `test_load_rejects_a_truncated_file` (cut at several points), `test_load_rejects_a_flipped_bit_anywhere` (flip one bit in each of 64 places spread over the file), `test_load_rejects_an_index_larger_than_the_arena`, `test_load_rejects_inconsistent_offsets_with_a_valid_checksum`, `test_full_arena_keeps_what_was_added` (a small arena: adds until `LIBRARY_FULL`, finishes, every added track is present and queryable), `test_five_thousand_tracks_fit_and_answer` (builds 5,000 synthetic tracks in a 1.5 MB static arena; spot-checks ranges, order and `find_path`; prints bytes per track and build time).

- [ ] **Step 1:** Header, stub, tests, CMake. Fail for the right reason.
- [ ] **Step 2:** Design the layout (write it up as the comment at the top of `library.c`: the file format, each table, how a build uses the arena), then implement. Pass, nothing on stderr.
- [ ] **Step 3:** Add to the firmware build; no warnings. Commit: `Add library: the index of artists, albums and songs`.

---

### Task 5: Scanning on the device

**Files:**
- Create: `components/dap_fs/CMakeLists.txt`, `components/dap_fs/libscan.h`, `components/dap_fs/libscan.c`, `main/library_task.h`, `main/library_task.c`
- Test: `test/host/test_libscan.c`
- Modify: `test/host/CMakeLists.txt`, `main/CMakeLists.txt`, `main/app_main.c`, `main/debug_console.c`

**Interfaces:**
- Consumes: `tags` (3), `library` (4), `translit` (stage 1), fixtures `test/host/fixtures/lib/` (2).
- Produces (`libscan.h`, binding):

```c
#pragma once

#include "library.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t tracks;     // added to the index
  uint32_t unreadable; // audio files that could not be opened or read
  uint32_t skipped;    // paths too long or folders too deep
  bool full;           // stopped because the index was full
} libscan_stats_t;

// Called after each track is added. Return false to abandon the scan.
typedef bool (*libscan_progress_fn)(void *ctx, const libscan_stats_t *stats);

#define LIBSCAN_MAX_DEPTH 8  // folders below the root
#define LIBSCAN_PATH_MAX 256 // bytes, including the terminator

// Walk the tree under `root`, read each .flac and .mp3 file's tags, and add
// it to `builder` with its path relative to `root`. Skips folders whose names
// start with '.' and "System Volume Information". Returns false if `root`
// cannot be opened or the scan was abandoned.
bool libscan_run(const char *root, library_builder_t *builder,
                 libscan_progress_fn progress, void *ctx,
                 libscan_stats_t *stats);
```

- `libscan` applies the fallbacks and the text conversion: title through `translit_title` (falling back to the file name), artist and album through `translit` with `Unknown Artist` / `Unknown Album` when empty, group artist = album artist if present, otherwise the artist.
- A file whose tags cannot be read (wrong contents, zero bytes) is still added, by file name, under the Unknown names, unless it cannot be opened at all (then it counts as unreadable).
- The walk must not recurse without a bound, must not `stat` every entry when the directory entry already says whether it is a folder, and must reuse one path buffer.

Host tests (binding names), run against `test/host/fixtures/lib/`:
`test_scan_finds_the_fourteen_tracks`, `test_hidden_and_system_folders_are_skipped`, `test_non_audio_files_are_ignored`, `test_artists_and_albums_match_the_fixture` (artists A to Z: `Bjork`, `Deep Artist`, `Queen`, `Radiohead`, `Unknown Artist`, `Various Artists`; Radiohead has two albums; `Greatest Hits` exists twice, under Queen and under Various Artists; Radiohead's `OK Computer` has two tracks although one is tagged `radiohead`), `test_album_order_within_kid_a` (tracks 1, 2, 10), `test_compilation_disc_and_track_order` (disc 1 tracks 1 and 2, then disc 2 track 1), `test_untagged_files_use_their_file_names`, `test_zero_byte_and_text_files_fall_back_or_are_counted`, `test_paths_are_relative_and_open_the_right_file`, `test_too_deep_folders_are_skipped_and_counted` (a temporary tree deeper than the limit), `test_progress_can_abandon_the_scan`, `test_missing_root_returns_false`, `test_full_index_stops_the_scan_cleanly` (a tiny arena).

Note on the fixture count: the tree has 14 tagged or untagged audio files that must be indexed, plus `loose/empty.flac` and `loose/text.mp3`, which have audio extensions. Decide from the rule above whether those two are added by file name or counted as unreadable, state it, and make `test_scan_finds_the_fourteen_tracks` and `test_zero_byte_and_text_files_fall_back_or_are_counted` agree with each other and with the rule; rename the first test if the number changes.

**On the device** (`main/library_task.c`): after the card is mounted, try to load `/sdcard/.dap/library.idx` into a 1.5 MB PSRAM arena allocated once. If it loads, log `library: loaded <tracks> tracks, <albums> albums, <artists> artists in <ms> ms`. If not, scan. A scan stops playback, shows progress, builds, saves (write `library.idx.tmp`, then replace `library.idx`; FAT cannot rename onto an existing file, so decide and document the order of operations and what a power cut at each point leaves behind; at load, a missing `library.idx` with a valid `.tmp` should be used), and logs `library: scanned <tracks> tracks in <ms> ms (<n> unreadable, <n> skipped)` and, if it applies, `library: full`. The library pointer handed to the rest of the firmware changes only when a load or scan has finished, and readers must never see a half-built index: say how you guarantee that.

**Screen while scanning:** top row `Scanning...`, bottom row `<n> tracks`, updated no more than four times a second. When done, the playing screen returns.

**Console:** `scan` (start a rescan), `lib` (counts and index size), `lib artists`, `lib albums <artist#>`, `lib tracks <album#>`, `lib songs <from> <count>`, `lib find <path>`, each printing one `dbg:` line per item in a fixed, greppable form you document in `debug_console.h`.

Board checks:
1. First boot with no index: it scans; `lib` reports at least 1,014 tracks plus the card's other audio files; the artists from `/testlib/` are present with the same albums and track order as the host test expects (check with `lib artists`, `lib albums`, `lib tracks`). The two skipped folders contribute nothing.
2. **Measure and report:** scan time and tracks per second for the whole card; index size in bytes and bytes per track; free PSRAM and internal heap before and after.
3. Second boot: the index loads; report the load time. `lib` gives identical counts. Boot to `STARTED` time against the 1935 ms before this stage.
4. Corrupt the index (a temporary console command or build that flips a byte in the file; not committed): next boot rejects it and rescans.
5. `scan` while the music track is playing: playback stops, the scan completes, and a `play` afterwards works; `t01` then ends with `frames=441000 expected=441000`.
6. During a scan, `key 5`, `stat` and `screen` are answered, and `screen` shows the progress screen.

- [ ] **Step 1:** `libscan` on the host: tests, implementation, pass.
- [ ] **Step 2:** `library_task`, progress screen, console commands. Build without warnings.
- [ ] **Step 3:** Board checks and measurements. Commit: `Scan the card into the library index; load it at boot`.

---

### Task 6: Title and artist from the index

**Files:**
- Modify: `main/app_main.c`

**Interfaces:**
- Consumes: the library pointer from Task 5; `library_find_path`, `library_track`, `library_artist_name`; the marquee from Task 1.
- Produces: the playing screen's top row shows `<Title> - <Artist>` for a track that is in the index (through the marquee when longer than 16 characters), and the file name as now for one that is not.

Requirements: the lookup is by the playing file's path relative to the card root, done once when a track starts, with no file access. If the index is replaced by a rescan while a track is showing, nothing may dangle: copy the text into the UI task's own buffer at track start.

Board checks: `play` a `/testlib/` track and `screen` shows its title and artist from its tags; `play /sdcard/test/t01_44k_16_st.flac` (untagged, but indexed under the fallbacks) shows what the fallback rule gives: state it; the music track shows its tag title and artist, scrolling. The music track start to end: `ENDED frames=13453440 expected=13453440`, no `UNDERRUN`.

- [ ] **Step 1:** Implement; build; board checks.
- [ ] **Step 2:** Commit: `Show title and artist from the library on the playing screen`.

---

## Stage 4 hardware check (run by the user)

1. **Marquee.** A long title rests for a moment, then scrolls smoothly right to left, with a gap before it comes round again. A short title stays still.
2. **Title and artist** on the top row are the track's real tags, not the file name.
3. **Rescan** (the coordinator triggers it from the console): the LCD shows `Scanning...` with a rising count, then returns to the player.
4. **Start-up** is not noticeably slower than before.
5. **Music is undisturbed** by the scrolling text.
6. **Optional:** put more of your own music on the card and tell the coordinator; it will rescan and list what it found for you to compare with what you copied.
