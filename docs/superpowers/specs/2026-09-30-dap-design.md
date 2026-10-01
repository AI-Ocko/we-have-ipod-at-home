# ESP32 Digital Audio Player — Design

Date: 2026-09-30
Status: awaiting review

## 1. Goal

Turn the `sd_test` project into a standalone digital audio player (DAP) on the
Freenove ESP32-WROVER CAM board. Music lives on the SD card. The user browses a
library by artist, album, song and playlist on a 16x2 character LCD using a 4x4
keypad, and controls playback: play/pause, previous/next, volume, shuffle, loop.
Playlists can be created and edited on the device.

Success means: with only the board, DAC, LCD, keypad and a card of tagged FLAC
and MP3 files, the user can find and play any track, build a playlist, and get
clean audio with no clicks or dropouts while doing so.

### What the user asked for

- Library read from the SD card; select songs, playlists, artists.
- LCD1602 screen and 4x4 keypad from the Freenove kit.
- Volume, previous/next, shuffle, loop.
- Full on-device playlist editing.
- Tags read from files and cached in an index (any folder layout).
- FLAC and MP3.
- No extra hardware for now.

### Decisions made during design (assumptions the user approved)

- Plain ESP-IDF v6.0.2, own LCD and keypad drivers, keeping the existing
  decode path (`espressif/esp_audio_codec` 2.6.2).
- LCD powered from 5V with I2C lines direct to the ESP32, as in Freenove's own
  tutorial.
- Non-Latin text cannot be shown on this LCD; it is simplified or replaced.
- True 24-bit output: all audio is sent to the DAC in 32-bit frames.
- Backlight timeout defaults to 30 s. The backlight is estimated at about
  20 mA at 5V, small next to the rest of the player; stage 2's hardware check
  includes an optional multimeter measurement to confirm.
- Software extras: resume at power-on, backlight timeout, sleep timer, and
  fast-forward/rewind subject to a feasibility test (section 14, stage 1).

### Out of scope

Gapless playback, cover art, equaliser, Bluetooth, Wi-Fi, battery monitoring,
USB mass storage, formats other than FLAC and MP3, potentiometer, IR remote,
LED level meter. The pin plan keeps four pins free so hardware extras can be
added later.

## 2. Hardware

### Pin map

| Part | Signal | GPIO | Notes |
|---|---|---|---|
| SD card | CLK / CMD / D0 | 14 / 15 / 2 | 1-bit SDMMC, unchanged |
| DAC (PCM5102A) | BCK / LCK / DIN | 26 / 25 / 22 | unchanged; DAC SCK tied to GND |
| LCD (PCF8574 backpack) | SDA | 21 | |
| LCD | SCL | 27 | not 22, which is the DAC data line |
| LCD | VCC / GND | 5V / GND | |
| Keypad | rows 1–4 | 32, 33, 4, 13 | outputs |
| Keypad | columns 1–4 | 34, 35, 36, 39 | input-only pins; each needs an external 10k pull-up to 3.3V |
| Spare | — | 5, 18, 19, 23 | the VSPI group, reserved for future hardware |
| Do not use | — | 0, 12 | strapping pins; 12 high at reset can prevent boot |

All pin numbers live in one header, `pins.h`. No other file contains a GPIO
number.

Assumption to verify at wiring time: nothing from the SD card holder is wired
to GPIO 4 or 13. If something is, keypad rows 3 and 4 move to GPIO 18 and 19.

### Keypad electrical scheme

One row is driven low at a time; the other three rows are high-impedance (not
driven high), so two keys pressed together can never short two outputs. A
pressed key pulls its column low through the active row.

### LCD

HD44780 controller in 4-bit mode behind a PCF8574 I2C expander. The driver
probes address 0x27, then 0x3F. I2C at 100 kHz using the ESP-IDF v6
`i2c_master` driver. The expander's backlight bit is used for the backlight
timeout.

### Power

Found by test on 2026-09-30: with the LCD connected, powering the board from
a computer's USB port leaves the SD card unresponsive from plug-in (it never
answers its first command, and stays that way through resets until the card
itself is re-inserted). Powered from a phone charger, the same board, wiring
and firmware start correctly every time. The LCD is the load that tips it:
with the LCD's power wire removed, the computer's port works too. The ESP32
itself reports a normal power-on in both cases.

The mechanism was not measured. The working explanation is that the weaker
source lets the 3.3V supply rise badly at plug-in and the card fails its own
power-on reset.

Consequences:

- The finished player must be powered from a supply at least as stiff as a
  phone charger. Any future battery design has to be checked against this.
- During development the board is on a computer's USB port for flashing and
  logs. After each plug-in there, take the card out and click it back in.
  From then on it survives resets and flashing.
- The firmware's card retry (section 11) must make re-inserting the card
  enough by itself, with no reset needed.

## 3. Architecture

Four long-running tasks communicate only through queues. No task calls into
another task's module state directly.

```
 keypad task ──key events──┐
                           ▼
                      ┌──────────┐  commands   ┌──────────────┐
                      │ UI task  │ ──────────► │ decoder task │
                      │          │ ◄────────── │              │
                      └────┬─────┘   events    └──────┬───────┘
                           │                          │ PCM
                         LCD                    ring buffer (PSRAM)
                                                      │
                                               ┌──────▼───────┐
                                               │ output task  │──► I2S ──► DAC
                                               └──────────────┘
```

| Task | Priority | Core | Stack | Job |
|---|---|---|---|---|
| output | 20 | 1 | 4 KB | Drain the ring buffer into I2S; apply volume and fades |
| decoder | 10 | 1 | 16 KB | Read file, decode, convert to output format, fill ring buffer; handle commands |
| keypad | 6 | 0 | 3 KB | Scan every 10 ms, debounce, post key events |
| UI | 5 | 0 | 8 KB | Own all screens, the play queue, and settings; drive the LCD |

The decoder task needs 16 KB because the FLAC decoder overflows smaller stacks
(found the hard way; it crashes inside the heap code with no clear trace).

A library scan runs in a temporary worker task (8 KB) that posts progress
events to the UI queue and exits when done.

### Code layout

```
main/
  app_main.c        start-up order, task creation
  pins.h            every GPIO number
  player.c/.h       decoder task + output task, command/event queues
  ui.c/.h           UI task: screen state machine, glue to queue and settings
  settings.c/.h     NVS-backed settings and resume state
components/
  dap_core/         pure C, no ESP-IDF includes; builds on the host too
    tags.c/.h         FLAC and ID3 tag readers
    translit.c/.h     UTF-8 to LCD character conversion
    library.c/.h      index build, save, load, query
    play_queue.c/.h   track order, shuffle, loop, next/previous rules
    playlists.c/.h    M3U read/write and edit operations
    textentry.c/.h    multi-tap text input state machine
    menu.c/.h         list navigation model (cursor, scrolling, letter jump)
    screen.c/.h       in-memory 16x2 frame; diff to the fewest LCD writes
    keys.c/.h         raw keypad scans to key events (debounce, long, repeat)
    keytest.c/.h      contents of the keypad test screen
  dap_hw/           ESP-IDF dependent drivers
    storage.c/.h      SD mount/unmount and retry
    audio_out.c/.h    I2S setup, reconfiguration, clean stop
    lcd.c/.h          HD44780 over PCF8574
    keypad.c/.h       matrix scan and the scanning task
test/host/          CMake project: builds dap_core with gcc and runs Unity tests
reference/          the original example main file, Kconfig and sd_card
                    component, kept unbuilt for reference
```

`dap_core` uses only the C standard library (`stdio`, `stdlib`, `string`,
`stdint`). File access goes through `FILE *`, which works on the device via the
SD mount and on the host against fixture files. This is what makes it testable
without the board.

The original `sd_card_example_main.c` is moved to `reference/`, not deleted.
Its commented-out `--christopher` blocks stay as they are.

### Performance rules

The code is C on a small machine and is written to get the most out of it.
These rules bind every module and are checked in review.

**Audio path (runs for every sample):**
- Integer and fixed-point arithmetic only. No floating point, no division and
  no function call per sample; work on whole blocks in tight loops.
- No heap allocation, logging or locking in the steady-state decode and output
  loops. Buffers are allocated once per track or once at start-up.
- Data moves between decoder and output in large blocks through the ring
  buffer, never a sample at a time.

**Library and menus (must stay instant with 5,000 tracks):**
- Nothing the user does at the keypad may trigger a scan over all tracks.
  Lists are served from arrays sorted once at scan time; "tracks of this
  album" and "albums of this artist" are contiguous ranges found by index,
  not by filtering.
- Lookups by name or path use binary search over a sorted array or a hash
  table: O(log n) or O(1), never a linear search inside a loop.
- Building the index is O(n log n): artist and album names are de-duplicated
  through a hash table while scanning, then sorted once with `qsort`.
- Strings are stored once in a single pool and referred to by offset. No
  per-track `malloc`.
- Shuffle is a Fisher-Yates shuffle: O(n), unbiased.

**Storage:**
- Tag reading touches only header bytes and seeks past embedded pictures.
- The index and playlists are read and written in large buffered blocks, not
  byte by byte.

**Screen and keys:**
- The LCD driver keeps a copy of what is on screen and sends only the
  characters that changed. The I2C link to the LCD is the slowest thing in the
  system (about 1 ms per character), so redrawing unchanged text is the main
  thing to avoid.
- Tasks block on queues or timers. No busy-wait loops and no polling faster
  than the 10 ms key scan.

**Measured, not assumed:** the real limits on this hardware are the 1-bit SD
link (about 2.5 MB/s at best) and the LCD's I2C link, not the processor. Where
a rule above costs clarity for no measurable gain, clarity wins; where there
is doubt, measure on the board.

## 4. Audio path

### Output format

The DAC is always fed **32-bit stereo frames at the file's own sample rate**.
Each sample is left-justified in its 32-bit word:

- 24-bit sources are shifted left 8 bits, so all 24 bits reach the DAC.
- 16-bit sources are shifted left 16 bits.
- Mono sources are duplicated to both channels.
- Sample rates from 8 kHz to 96 kHz are accepted. Anything else is reported as
  "Can't play".

The PCM5102A accepts 32-bit I2S frames and uses the top 24 bits. A single
output format keeps volume scaling and fades simple, delivers true 24-bit
output, and means software volume no longer discards detail from 16-bit
sources (the attenuated signal uses the spare low bits).

This doubles the I2S bit clock compared with the 16-bit configuration that was
first verified by ear (2.8 MHz instead of 1.4 MHz at 44.1 kHz). Fallback if the
faster clock is noisy on breadboard wiring: 16-bit frames for 16-bit sources,
32-bit frames only for 24-bit sources.

### Where conversion happens

Measured in stage 3: PSRAM is slow enough to matter. With the converted
32-bit buffer in PSRAM, conversion plus volume cost 21% of real time for
96 kHz 24-bit audio; in internal RAM the same work costs 1.5%. So:

- the decoder's output buffer is in internal RAM;
- the ring buffer holds the decoder's own PCM, unconverted (16-bit, or 24-bit
  packed in 3 bytes; mono or stereo);
- the output task reads a small block from the ring into internal RAM,
  converts it to 32-bit stereo, applies volume and fades, and writes it to
  I2S.

### Ring buffer

768 KB in PSRAM between decoder and output, holding native PCM: about 4.4
seconds at 44.1 kHz 16-bit stereo, 1.3 seconds at 96 kHz 24-bit stereo. It
absorbs SD card stalls, including playlist and resume-state writes during
playback.

The compressed-input buffer (32 KB) must be internal DMA-capable RAM
(`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`); the SDMMC driver cannot read into
PSRAM. Input bytes are never moved while the decoder still holds unconsumed
data, because the decoder keeps pointers into that buffer.

Measured decode speed on this board (`-O2`, 240 MHz, including SD reads):
about 4 times real time for 44.1 kHz 16-bit music, 1.6 times for 96 kHz
24-bit. 96 kHz 24-bit is the case with little headroom.

### I2S

Philips format, 32-bit data in 32-bit slots, no MCLK. DMA: `dma_desc_num = 16`,
`dma_frame_num = 500`. A DMA descriptor holds at most 4092 bytes and a 32-bit
stereo frame is 8 bytes, so 500 frames is near the ceiling; 16 descriptors
keep the same ~180 ms of DMA depth at 44.1 kHz that cured the clicking in the
16-bit configuration (8 x 1000). When a track's sample rate differs from the
previous one, the channel is disabled, its clock reconfigured, and re-enabled.

### Volume

21 levels, 0 to 20. Level 0 is mute, level 20 is full scale, each step is
2 dB. Applied in the output task by multiplying each 32-bit sample by a table
value using 64-bit intermediate arithmetic. Default on first boot: 10.

### Starting, pausing, stopping

- Every start and resume fades in over 400 frames; every pause, stop and track
  skip fades out over 400 frames.
- After a fade-out with nothing more to play, the output task writes silence,
  waits 50 ms, and disables the I2S channel. Leaving it enabled and idle makes
  the DMA replay stale data as a buzz.
- At a natural end of track, every real frame must have left the I2S DMA
  buffers (up to 8000 frames, 181 ms at 44.1 kHz) before the channel is
  disabled or retuned, so the last moments of a track are not cut off.
- Between consecutive tracks of the same sample rate the channel stays enabled;
  there is a short silence but no click. Gapless playback is not attempted.

### Player interface

Commands (UI → decoder): `PLAY(path, start_ms)`, `PAUSE`, `RESUME`, `STOP`,
`SEEK_REL(ms)`, `SET_VOLUME(level)`.

Events (decoder → UI): `STARTED(duration_ms, sample_rate)`,
`POSITION(ms)` every 500 ms, `ENDED`, `ERROR(reason)`.

The player knows nothing about queues or the library. On `ENDED`, the UI task
asks `play_queue` what is next and sends a new `PLAY`.

Position is counted from frames delivered to I2S, not from file offset.

### Seeking

`SEEK_REL` and a non-zero `start_ms` seek by byte offset: the target position
is `audio_start + (file_size - audio_start) * target_ms / duration_ms`, so the
landing point is approximate for variable-bitrate audio. Position reporting
restarts from `target_ms`.

**FLAC (tested 2026-09-30, stage 1, branch `spike/flac-seek`, log in
`docs/superpowers/plans/stage1-flac-seek.log`):**

| Method | 50% | 90% |
|---|---|---|
| A: reset, jump | fail: errors=20 first_err=-7 blocks=0 | fail: errors=20 first_err=-7 blocks=0 |
| B: reset, replay header, jump | pass: errors=0 first_err=0 blocks=60 | pass: errors=0 first_err=0 blocks=60 |
| C: reopen, jump | fail: errors=20 first_err=-7 blocks=0 | fail: errors=20 first_err=-7 blocks=0 |
| D: reopen, replay header, jump | pass: errors=0 first_err=0 blocks=60 | pass: errors=0 first_err=0 blocks=60 |

**Decision:** FLAC seeks using method B (reset, replay header, jump). Stage 3 implements SEEK_REL and start_ms for FLAC this way; stage 8 adds fast-forward, rewind and mid-track resume.

Confirmed by ear on 2026-09-30: the jumps for methods B and D are clean cuts
with no noise. Not yet measured: the cost of replaying the header for files
with large embedded cover art, and behaviour on more than one file.

**MP3:** not yet tested; the MP3 parser resynchronises on frame headers, so a
byte-offset jump after `esp_audio_simple_dec_reset()` is expected to work.
Stage 3 verifies it when MP3 playback is added.

## 5. Tags

`tags_read(path, out)` fills title, artist, album, album artist, track number,
disc number, duration and format. It reads only headers and never loads cover
art; picture blocks and frames are skipped by seeking.

| Format | Source | Duration |
|---|---|---|
| FLAC | Vorbis comment block | Exact, from STREAMINFO |
| MP3 | ID3v2.3 and v2.4 text frames, in ISO-8859-1, UTF-16 (with BOM) or UTF-8; ID3v1 at the end of the file as fallback | From the Xing/Info header if present; otherwise estimated from first-frame bitrate and file size |

File type is decided by extension (`.flac`, `.mp3`, case-insensitive).

Fallbacks: no title → file name without extension. No artist → "Unknown
Artist". No album → "Unknown Album". Album artist, if present, is used for
grouping instead of artist.

### Text conversion for the LCD

`translit` converts UTF-8 to the LCD's character set:

- ASCII passes through, except `\` → `/` and `~` → `-` (the LCD's character
  ROM shows other glyphs for those two codes).
- Accented Latin letters (Latin-1 and Latin Extended-A) map to their base
  letter; `ß` → `ss`, `æ` → `ae`, `ø` → `o`.
- Common typographic punctuation maps to ASCII: curly quotes to `'` and `"`,
  dashes to `-`, the ellipsis to `...`, the bullet to `*`, the non-breaking
  space to a space.
- Everything else, including any invalid UTF-8 byte, becomes `?`.

If more than half of a title converts to `?`, the file name (converted the same
way) is used instead. The index stores the already-converted text, so
conversion happens once at scan time.

## 6. Library

### Scan

Walks the whole card recursively. Skips directories whose names start with `.`
(which covers the player's own `/.dap/`) and `System Volume Information`.
Reads tags for each `.flac` and `.mp3`. Playback is stopped before a scan
starts.

Triggers: no usable index at boot (automatic), or **Settings → Rescan library**.
The screen shows a running count of tracks found.

### Index

Saved to `/.dap/library.idx` and loaded whole into PSRAM at boot. Contains:

- header: magic, format version, counts, CRC32 of the body;
- track table: path, title, artist id, album id, track number, disc number,
  duration, format;
- artist table and album table (album → owning artist);
- three sort orders: artists A–Z, albums A–Z, songs A–Z (case-insensitive);
- a string pool holding all text.

Tracks within an album are ordered by disc number, then track number, then
file name.

Written by saving to `library.idx.tmp` and renaming. A bad magic, version or
CRC at boot triggers an automatic rescan.

### Limits

Design target: 5,000 tracks (about 650 KB at ~130 bytes per track). Hard cap:
1.5 MB of index memory. If a scan reaches the cap, it stops, keeps what it has
indexed, saves it, and shows "Library full".

### Queries

List artists; albums for an artist; tracks for an album; all tracks for an
artist; all albums; all songs; look up a track by path (used by playlists and
resume). All queries read memory only and never touch the SD card.

## 7. Play queue

`play_queue` holds an ordered list of track ids, a current position, the
shuffle flag and the loop mode (off, all, one).

- **Selecting a track** in a list replaces the queue with that whole list (the
  album, the artist's songs, the playlist, or all songs) and starts at the
  chosen track.
- **Play next** inserts after the current track. **Add to queue** appends.
- **Shuffle on** randomises the order of everything except the current track,
  which becomes first. **Shuffle off** restores the original order, keeping the
  current track current.
- **Next** at the end: loop all → wraps to the start; loop off → playback
  stops and the queue stays.
- **Loop one** repeats the current track on natural end. A manual Next still
  advances.
- **Previous**: more than 3 seconds into the track → restart it; otherwise go
  to the track before (wrapping only when loop all is on).
- **Failures**: a track that errors is skipped. Three consecutive errors stop
  playback.

## 8. Playlists

Standard M3U files in `/playlists/`, UTF-8, one path per line relative to the
card root, `#` lines ignored. Files made on a computer are accepted, including
absolute paths and backslash separators, which are normalised on load.

Entries are resolved against the index by path. Entries with no match are kept
in the file, shown as "(missing)", and skipped in playback.

Every edit is saved immediately by writing `<name>.m3u.tmp` and renaming it
over the original.

Operations: create, rename, delete (with confirmation), add track, add all
tracks of an album or artist, save the current queue as a playlist, remove an
entry, move an entry up or down.

Names: capitals, digits, space and `- ' & .`; 1 to 16 characters; must not
duplicate an existing playlist (shows "Name in use"). Limit: 1,000 entries per
playlist.

## 9. User interface

### Keys

```
 1  2  3  A        A  volume up         (every screen)
 4  5  6  B        B  volume down       (every screen)
 7  8  9  C        C  toggle Now Playing / menus
 *  0  #  D        D  play / pause      (every screen)
```

| Key | Lists and menus | Now Playing |
|---|---|---|
| 2 / 8 | Up / down; hold to repeat, accelerating | — |
| 5 or 6 | Open / select | 5 play/pause; 6 next (hold: fast-forward) |
| 4 or * | Back | 4 previous (hold: rewind); * back to menus |
| 1 / 3 | Jump to previous / next starting letter | 1 shuffle on/off; 3 loop mode |
| # | Options for highlighted item | Options for current track |

Key events: press, long press (held 600 ms), repeat (every 150 ms after a long
press, dropping to 50 ms once the key has been held 2 seconds), release.
Debounce: 3 consecutive matching scans. Only one key at a time is recognised:
a second key pressed while one is held is ignored until the first is released
(then it registers, so rolling from key to key works), and several keys going
down together from rest produce nothing until all are released.

On Now Playing, 4 and 6 act on release if no long press occurred, so holding
to seek does not also skip a track.

### Screens

Lists: top line shows the list name and position; bottom line shows the
highlighted item, scrolling sideways if longer than 15 characters.

```
Artists    14/87        Kid A       3/10
>Radiohead              >The National A>
```

Now Playing: top line scrolls "Title - Artist"; bottom line shows play/pause
symbol, elapsed/total time, shuffle and loop indicators. A volume change
replaces the bottom line with a bar for 1.5 seconds.

```
Everything In I>        Everything In I>
> 1:23/4:11  S L        Vol =======-- 14
```

Play, pause, shuffle, loop-all, loop-one and the bar segments are custom LCD
characters (the controller allows eight).

### Menu tree

- Now Playing
- Artists → albums of that artist, preceded by "All songs" → tracks
- Albums → tracks
- Songs
- Playlists → "New playlist", then each playlist → entries
- Settings
  - Rescan library
  - Backlight: 15 s / 30 s / 60 s / always on (default 30 s)
  - Sleep timer: off / 15 / 30 / 60 min
  - Keypad test
  - About: track count, card free space, firmware build date

### Options menus (#)

| On | Options |
|---|---|
| Track | Play, Play next, Add to queue, Add to playlist |
| Album or artist | Play, Shuffle play, Add to queue, Add to playlist |
| Playlist | Play, Shuffle play, Rename, Delete |
| Entry inside a playlist | Play, Move up, Move down, Remove |
| Now Playing | Add to playlist, Save queue as playlist |

"Add to playlist" opens the playlist list with "New..." first.

### Text entry

```
Name:
ROAD TRIP_
```

Multi-tap: 2–9 cycle their letters then the digit (2 → A, B, C, 2); 0 cycles
space then 0; 1 cycles `- ' & .` then 1. A character is committed after 1
second or when a different key is pressed. `*` deletes the last character, `#`
saves, `C` cancels. A, B and D keep their global meaning.

### Backlight and sleep

Any key press while the backlight is off turns it on and is otherwise ignored.
The sleep timer, when it expires, fades out, stops playback and turns the
backlight off; it then resets to off.

## 10. Settings and resume

Stored in NVS: volume, shuffle, loop mode, backlight timeout.

Resume state: the queue (as paths) is written to `/.dap/queue.m3u` whenever it
changes. The current queue position and elapsed time are written to NVS on
track change, on pause, and every 30 seconds during playback.

At boot, if the queue file loads and its current track still exists in the
index, the player opens Now Playing in the **paused** state on that track, at
the saved time if seeking is supported for its format, otherwise at 0. It does
not start making sound until the user presses play.

## 11. Failure handling

| Problem | Behaviour |
|---|---|
| No card or mount fails | "Insert SD card"; retry every 2 seconds |
| Read error during playback | Stop, "Card error", unmount, return to the retry loop |
| File missing since last scan | "File missing" for 1.5 s, skip to next |
| Decode failure or unsupported format | "Can't play" for 1.5 s, skip to next; stop after 3 in a row |
| Index missing, corrupt or wrong version | Automatic rescan |
| Index memory cap reached | Stop scan, keep partial index, "Library full" |
| Playlist entry not in index | Kept in file, shown "(missing)", skipped |
| Allocation failure at start-up | Message on LCD and serial log; halt |
| LCD not found at 0x27 or 0x3F | Logged on serial; player runs without a screen |

No `ESP_ERROR_CHECK` on anything that can fail at run time because of the card
or a bad file; those paths return errors and are handled as above.
`ESP_ERROR_CHECK` remains acceptable for one-time driver initialisation.

## 12. Build configuration

Known-good settings currently exist only in the working `sdkconfig`. They move
into `sdkconfig.defaults` so a clean build reproduces them:

- PSRAM enabled, `CONFIG_SPIRAM_USE_MALLOC`, 4 MB mapped (existing).
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` (existing).
- `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255` (existing).
- `CONFIG_FATFS_API_ENCODING_UTF_8=y` (**new**; currently ANSI/OEM code page
  437, which mangles non-ASCII file names).
- `CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096` (existing).

Performance settings (**new**; the project was still on the example's
debug-oriented defaults):

- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`: compile with `-O2` instead of `-Og`.
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`: run the processor at its full
  240 MHz instead of 160 MHz. Costs roughly 20 to 30 mA more current.
- `CONFIG_ESP32_REV_MIN_3=y`: declare the minimum chip revision as 3. This
  board's chip is revision 3.1. It removes `CONFIG_SPIRAM_CACHE_WORKAROUND`,
  a fix for a bug in older chips that slows every access to PSRAM and uses
  about 15 KB of fast memory. Firmware built this way refuses to boot on
  older chips, with a clear message.

Not changed: SD card speed stays at the default 20 MHz, because the 1 GB test
card does not support high-speed mode. A modern SDHC card would allow 40 MHz;
revisit when the card is replaced.

`MINIMAL_BUILD` stays on, so each component's `REQUIRES` lists what it uses
(`esp_driver_i2s`, `esp_driver_i2c`, `esp_driver_gpio`, `nvs_flash`, `fatfs`,
`esp_timer`, and so on).

The example's Kconfig options (`CONFIG_EXAMPLE_*`) are retired; SD pins and bus
width come from `pins.h`.

## 13. Testing

### Automated, on the host

`test/host` builds `dap_core` with gcc and runs Unity tests (Unity is shipped
with ESP-IDF). Tests are written before the code they cover.

- `tags`: small fixture files for FLAC, ID3v2.3, ID3v2.4, ID3v1-only, UTF-16
  frames, files with large picture blocks, untagged and truncated files.
- `translit`: accents, unsupported scripts, the half-`?` fallback rule.
- `library`: build from a fixture tree, save, reload, every query, sort order,
  album-artist grouping, CRC rejection, the memory cap.
- `play_queue`: every rule in section 7.
- `playlists`: load, normalise paths, each edit operation, atomic save,
  missing entries, name validation.
- `textentry`: multi-tap cycling, timeout commit, delete, length limit.
- `menu`: cursor movement, wrap, letter jump, scroll offsets.

### On the board, by the user

Agents cannot hear audio or see the LCD. Each stage ends with a written
checklist the user runs on hardware; the next stage starts only after it
passes.

## 14. Build stages

Each stage ends with firmware that works and does more than the last.

1. **Foundation.** Move known-good settings into `sdkconfig.defaults`; move the
   example files to `reference/`; split the current code into `storage`,
   `audio_out` and a minimal `player` with no change in behaviour (still plays
   the first FLAC). Set up the host test project. Run the FLAC seek feasibility
   test and record the result in this spec.
   *Check: the same FLAC plays as cleanly as before.*
2. **LCD and keypad.** Drivers, plus the keypad test screen as the temporary
   main screen. Includes a label-by-label wiring list.
   *Check: text is readable; every one of the 16 keys shows correctly; long
   press and repeat work. Optional: measure backlight current with a
   multimeter.*
3. **Player.** First, find and fix a defect inherited from the example and
   measured in stage 1: on the test track the decoder logs
   `AUD_Dec_Parse: Not found frame in max frame size` about 4 seconds in, and
   2,718 blocks are decoded where the track's length implies about 2,750, so
   roughly 3 seconds of audio are skipped. Stage 1 preserved this on purpose.
   Then: decoder and output tasks, ring buffer, command and event queues,
   volume, pause/resume with fades, MP3 support, conversion of 16-bit, 24-bit
   and mono sources to 32-bit stereo frames.
   Temporary key bindings: D play/pause, A/B volume.
   *Check: a FLAC and an MP3 both play cleanly and sound at least as good as
   the 16-bit baseline; a 24-bit FLAC plays; pause and volume cause no
   clicks.*
4. **Tags and library.** `tags`, `translit`, `library`, the scan worker and its
   progress screen.
   *Check: scan completes; serial log lists artists, albums and counts that
   match the card.*
5. **Browsing.** `menu`, the menu tree, Now Playing, selecting a track to play.
   *Check: any track can be found and played; long names scroll.*
6. **Queue and resume.** `play_queue`, shuffle, loop, previous/next rules,
   options menus for Play next / Add to queue, `settings`, resume at boot.
   *Check: each rule in section 7; unplug and replug restores the track.*
7. **Playlists.** `playlists`, `textentry`, all playlist screens and options.
   *Check: create, fill, reorder, rename, delete; a computer-made M3U loads; a
   device-made one opens on the computer.*
8. **Finishing.** Backlight timeout, sleep timer, About screen, and
   fast-forward/rewind plus mid-track resume if stage 1's test passed.
   *Check: each feature; an hour of continuous play with no dropouts.*
