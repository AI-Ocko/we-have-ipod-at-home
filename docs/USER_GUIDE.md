# User guide

A music player built from a Freenove ESP32-WROVER board, a PCM5102A DAC, a
16x2 LCD and a 4x4 keypad. It plays FLAC and MP3 files from the board's SD
card. You find music by artist, album, song or playlist, and you can make and
edit playlists on the player itself.

Contents: [Parts and wiring](#parts-and-wiring) ·
[Power](#power) · [The card](#the-card) ·
[Keys and screens](#keys-and-screens) · [Playlists](#playlists) ·
[Settings](#settings) · [What is remembered](#what-is-remembered-at-power-off) ·
[Building and flashing](#building-and-flashing) ·
[Troubleshooting](#troubleshooting) · [The serial console](#the-serial-console)

---

## Parts and wiring

| Part | What it is |
|---|---|
| Board | Freenove ESP32-WROVER (with the camera connector). Its built-in SD card slot holds the music. |
| DAC | PCM5102A module. It turns the music into sound; headphones or an amplifier go on its output. |
| LCD | LCD1602 (16 characters, 2 rows) with an I2C backpack on the back (PCF8574). |
| Keypad | 4x4 membrane keypad, keys `1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D`. |
| Resistors | Four 10k resistors (brown, black, orange) for the keypad. |

**Wire by the label printed on each part, never by its position in a table or
a photo.** A table in the wrong order once caused a short. Unplug the USB
cable before adding or moving any wire.

Pin numbers below are the numbers printed beside the pins on the board. Two
pins have names instead: **VP is GPIO 36** and **VN is GPIO 39**.

### DAC (PCM5102A)

On the module the pins read, left to right, `SCK BCK DIN LCK VCC GND`. Go by
the labels:

| DAC pin label | Board pin |
|---|---|
| SCK | GND (the DAC makes its own clock) |
| BCK | 26 |
| DIN | 22 |
| LCK | 25 |
| VCC | 3.3V |
| GND | GND |

### LCD (LCD1602 with I2C backpack)

| LCD pin label | Also seen labelled | Board pin |
|---|---|---|
| VSS | GND | GND |
| VDD | VCC | **5V** |
| SDA | SDA | 21 |
| SCK | SCL | 27 |

- The LCD must be on **5V**. On 3.3V the backlight comes on but no text can be
  seen. (Some modules also have a small contrast screw on the back that may
  need turning.)
- The clock wire goes to **27**, not 22 as in Freenove's tutorial: 22 is the
  DAC's data line.
- The player looks for the LCD at I2C address 0x27, then 0x3F; both are
  common for these backpacks.

### Keypad (4x4)

Hold the keypad with the keys facing you and the ribbon at the bottom. Its
eight pins are numbered 1 to 8 from left to right.

| Keypad pin | What it is | Board pin | Also needs |
|---|---|---|---|
| 1 | Row 1 (keys 1 2 3 A) | 32 | |
| 2 | Row 2 (keys 4 5 6 B) | 33 | |
| 3 | Row 3 (keys 7 8 9 C) | 4 | |
| 4 | Row 4 (keys * 0 # D) | 13 | |
| 5 | Column 1 (keys 1 4 7 *) | 34 | 10k resistor to 3.3V |
| 6 | Column 2 (keys 2 5 8 0) | 35 | 10k resistor to 3.3V |
| 7 | Column 3 (keys 3 6 9 #) | VP (36) | 10k resistor to 3.3V |
| 8 | Column 4 (keys A B C D) | VN (39) | 10k resistor to 3.3V |

Pins 34, 35, VP and VN can only read and have no built-in pull-up, so each
column wire needs a 10k resistor from that wire to **3.3V**. On a breadboard:
put each column wire in its own row and bridge each of those rows to the 3.3V
rail with one resistor. **The resistors go to 3.3V, never 5V**: 5V on these
pins can damage the chip. Without them the keys read as random presses (the
player checks at start-up and writes which column is affected to the serial
log).

Getting the keypad's eight wires in the wrong order cannot damage anything.
Use **Settings → Keypad test** to see which key the player thinks you
pressed.

### SD card

The card goes in the board's own slot; there is nothing to wire. The board
can only use the slot in 1-bit mode (the camera connector shares the other
lines), which is plenty for music.

### Pins to leave alone

- **0 and 12**: used by the chip while it starts. 12 held high at reset stops
  the board from booting.
- **5, 18, 19, 23**: free, kept for future hardware.

The full pin list is in `main/pins.h`.

---

## Power

- **Use a phone charger or a USB power bank.**
- A power bank that switches itself off when the current is low will switch
  off while the player is paused. Use one with a low-current (always-on)
  mode. The player draws roughly 60 to 90 mA paused and about 130 mA playing
  (estimates, not measured).
- **A computer's USB port** is fine for flashing and reading the log, but with
  the LCD attached the SD card may not start after you plug in: the screen
  says `Insert SD card`. Take the card out and click it back in once; from
  then on it survives resets and flashing until the next time you unplug.
- There is no battery built in.

---

## The card

- **Format:** FAT32 (FAT). Cards over 32 GB usually come formatted as exFAT
  and need reformatting as FAT32.
- **Music:** `.flac` and `.mp3` files, anywhere on the card, in any folders
  (up to 8 folders deep, and a path from the card root of at most 255
  bytes; longer ones are left out). Artist and album
  folders are best: a single folder with hundreds of files scans much more
  slowly.
- **Tags** give the title, artist and album. A file with no tags is listed by
  its file name under `Unknown Artist` and `Unknown Album`. The LCD shows only
  plain letters: accented letters are simplified, and characters it cannot
  show become `?`.
- Files and folders whose names start with `.` are ignored, and so is
  `System Volume Information`.
- **`/playlists/`** holds playlists (`.m3u` or `.m3u8` files). The player
  makes the folder when you save your first playlist.
- **`/.dap/`** is the player's own folder: the library index
  (`library.idx`) and the saved queue (`queue.bin`). Do not edit it. Deleting
  it is safe: the player scans the card again at the next start and the
  queue starts empty.

**First start:** with no index on the card the player scans it, showing
`Scanning...` and a count of tracks found (about 160 tracks a second with
album folders). After that the index is loaded at every start in a fraction
of a second.

**After changing the music on the card** (on a computer), choose
**Settings → Rescan library**. The player does not notice new, moved or
deleted files by itself.

**Limits:** the index holds about 7,400 tracks (1.5 MB of memory). The queue
holds 8,192 tracks, a playlist 1,000 entries, and the Playlists screen lists
up to 200 playlists.

---

## Keys and screens

### The keys at a glance

```
 1  2  3  A        A  volume up          (everywhere)
 4  5  6  B        B  volume down        (everywhere)
 7  8  9  C        C  Now Playing <-> menus
 *  0  #  D        D  play / pause       (everywhere)
```

`A`, `B`, `C` and `D` work on every screen except the key test. Holding `A`
or `B` keeps changing the volume.

**The backlight:** it goes off when no key has been used for a while (see
Settings). With the backlight off, the first key press only turns it back on
and does nothing else, even if you hold the key.

### In the menus and lists

The top row shows the list's name and your position (`Artists    14/17`);
the bottom row shows the highlighted item after a `>`. Text too long to fit
scrolls sideways after a moment.

| Key | What it does |
|---|---|
| `2` / `8` | Up / down. Hold to keep moving (it speeds up after 2 seconds). A press at either end goes round to the other end; holding stops at the end. |
| `5` or `6` | Open the item, or play it. |
| `4` or `*` | Back one level. |
| `1` / `3` | Jump to the previous / next first letter (see below). |
| `#` | Options for the highlighted item (see [Options](#options-)). |
| `C` | Go to Now Playing. |

**Letter jumps:** in lists sorted A to Z (Artists, Albums, Songs, Playlists,
and an artist's albums) `3` jumps to the first item of the next starting
letter and `1` to the previous one. In other lists (an album's tracks, an
artist's songs, a playlist's entries, the fixed menus) they move 10 rows down
or up.

### The menu tree

- **Now Playing**
- **Artists** → an artist: `All songs`, then their albums → tracks
- **Albums** → tracks
- **Songs** (every track, A to Z)
- **Playlists** → `New playlist`, then each playlist → its entries
- **Settings** → `Rescan library`, `Backlight`, `Sleep timer`,
  `Keypad test`, `About`

**Choosing a track plays it and the rest of its list.** The list on the
screen (the album, the artist's songs, all songs, or the playlist) becomes
the queue, starting at that track, and Now Playing comes up.

An empty list shows `(empty)`. A playlist screen still being read from the
card shows `Loading`.

### Now Playing

```
Hey Mama - Kanye
> 0:12/5:05  S L
```

- Top row: `Title - Artist`, scrolling if it is long (the file name if the
  track has no title).
- Bottom row: the play symbol (a triangle) or the pause symbol (two bars),
  then the time played and the track's length. Long tracks show minutes only
  (75 minutes is `75:00`).
- `S` at the right means shuffle is on. The last character is the loop mark:
  two arrows in a circle for loop all, an arrow over a `1` for loop one,
  nothing for loop off.
- `Nothing playing` means there is no track. The marks are still shown.

| Key | What it does |
|---|---|
| `5` | Play / pause (the same as `D`). |
| `6` | Next track (when you let go). Hold: fast-forward. |
| `4` | Previous track (when you let go). Hold: rewind. |
| `1` | Shuffle on / off. `Shuffle on` or `Shuffle off` shows for 1.5 s. |
| `3` | Loop: off → all → one → off. `Loop all`, `Loop one` or `Loop off` shows for 1.5 s. |
| `#` | Options: `Add to playlist`, `Save queue`. |
| `*` | Back to the menus. |
| `C` | Back to the menus, where you left them. |

- **Previous** (`4`) in the first 3 seconds of a track goes to the track
  before; later it starts the current track again. On the first track it
  restarts it (with loop all it goes to the last track).
- **Next** (`6`) on the last track stops, unless loop all is on, which goes
  round to the first track. Loop one repeats a track when it ends by itself;
  `6` still moves on.
- **Shuffle** mixes the queue but keeps the current track playing; turning it
  off puts the queue back in its original order.
- When the queue runs out, the last track stays on Now Playing, stopped at
  `0:00`. `D` plays it again.
- **`D` on a stopped track** plays the track shown, from the time shown (for
  example after power-on, after a rescan or after the sleep timer).

### The volume bar

`A` or `B` shows the volume on the bottom row for 1.5 seconds:

```
Vol#####+---- 11
```

Ten cells (each cell is two steps) and the level, 0 (silent) to 20 (full).
The volume starts at 10 on a new player.

### Fast-forward and rewind

On Now Playing, **hold `6`** to fast-forward or **hold `4`** to rewind. The
bottom row shows `>>` or `<<` and the time you will jump to; it races at
about 13 times normal speed, then about 100 times after two seconds. **Let
go** and playback jumps there. A paused track stays paused at the new place;
a stopped track will start from there when you press `D`.

It stops at `0:00` and one second before the end. A quick tap of `4` or `6`
still skips. It does nothing while `Can't play` or `File missing` is shown,
during a scan, or on a track whose length the player does not know.

### Options (`#`)

`#` opens a menu of actions for the highlighted item. The top row shows the
item's name, the bottom row one action.

| On | Actions |
|---|---|
| A track | `Play`, `Play next`, `Add to queue`, `Add to playlist` |
| An album or an artist | `Play`, `Shuffle play`, `Add to queue`, `Add to playlist` |
| A playlist | `Play`, `Shuffle play`, `Rename`, `Delete` |
| An entry in a playlist | `Play`, `Move up`, `Move down`, `Remove` |
| Now Playing | `Add to playlist`, `Save queue` |

In the menu: `2` / `8` move, `5` or `6` does the action, `4`, `*` or `#`
closes it, `C` closes it and switches screens.

- **Play** replaces the queue and starts playing. **Shuffle play** does the
  same with shuffle turned on and a random track first.
- **Play next** puts the track straight after the current one. **Add to
  queue** puts the track, or all of the album's or artist's tracks, at the
  end. Nothing that is playing changes. `Added` shows; `Queue full` if they
  do not all fit (then none are added).
- The playlist actions are described under [Playlists](#playlists).

### Typing a name

Naming a playlist opens:

```
Name:
ROAD TRIP_
```

| Key | Types (press again within a second for the next) |
|---|---|
| `2` | A B C 2 |
| `3` | D E F 3 |
| `4` | G H I 4 |
| `5` | J K L 5 |
| `6` | M N O 6 |
| `7` | P Q R S 7 |
| `8` | T U V 8 |
| `9` | W X Y Z 9 |
| `0` | space 0 |
| `1` | `-` `'` `&` `.` 1 |
| `*` | Delete the last character |
| `#` | Save |
| `C` | Cancel |

A character is fixed after one second, or as soon as you press a different
key. Names are capital letters, digits, space and `- ' & .`, up to 16
characters. `A`, `B` and `D` still change the volume and play/pause.

If a name is not allowed, the reason shows on the bottom row for 1.5 s and
you can carry on typing: `Name empty`, `Name too long`, `Bad character`,
`Can't end in .`, `Can't start w/ .`, `Name reserved` (names Windows keeps
for itself, such as `CON`, `PRN`, `AUX`, `NUL`, `COM1` or `LPT1`), or
`Name in use` (another playlist already has it, ignoring capitals).

---

## Playlists

### Making and editing them on the player

- **A new, empty playlist:** Playlists → `New playlist`, type a name, `#`.
  `Saved` shows.
- **Add a track, album or artist:** highlight it, `#` → `Add to playlist`.
  The `Add to playlist` list opens with `New...` first, then your playlists.
  Choose one (`Added`), or `New...` to type a name for a new playlist holding
  them (`Saved`).
- **Add what is playing:** on Now Playing, `#` → `Add to playlist`.
- **Save the queue as a playlist:** on Now Playing, `#` → `Save queue`, type a
  name. The tracks are saved in the order they were added (not the shuffled
  order). A queue of more than 1,000 tracks saves its first 1,000 and shows
  `Saved first 1000`.
- **Rename:** highlight the playlist, `#` → `Rename`. The old name is filled
  in to edit. `Renamed` shows.
- **Delete:** highlight the playlist, `#` → `Delete`. The screen asks
  `Delete <name>` with `No` first: `2` / `8` switch to `Yes`, `5` or `6`
  chooses, `4` or `*` cancels. `Deleted` shows.
- **Reorder or remove entries:** open the playlist, highlight an entry, `#` →
  `Move up`, `Move down` or `Remove`. The menu stays open on the moved entry,
  so you can press `5` again to keep moving it.
- **Play:** choose an entry to play the playlist from there, or `#` on the
  playlist → `Play` / `Shuffle play`.

Every change is saved to the card at once. A playlist holds up to 1,000
entries: adding more shows `Playlist full` and adds nothing.

Messages you may see: `Saved`, `Added`, `Renamed`, `Deleted`,
`Playlist full`, `Name in use`, `Nothing to play` (no entry of the playlist
is on the card), `Nothing playing`, `Queue empty`, `Not found` (the playlist
was deleted meanwhile), `Bad file` (a playlist with a line too long to
read), `Busy` (too many requests at once; try again), `Library busy` (a scan
is running), `Try again` (the library changed meanwhile), `Card error`.

### Entries marked `(missing)`

An entry whose file is not in the library (deleted, moved, renamed, or never
on this card) shows as `(missing)`. It stays in the playlist file and is
skipped when the playlist plays; choosing it shows `File missing`. You can
still move or remove it. If you have just copied the file to the card, run
**Settings → Rescan library**.

### Playlists made on a computer

Put `.m3u` or `.m3u8` files in `/playlists/` on the card. They are listed by
their file name without the ending, as they are (lower case included). One
track per line; lines starting with `#` (such as `#EXTM3U` and `#EXTINF`) are
ignored. The player accepts:

- paths **relative to the playlist**, such as `../Music/Artist/Album/01 Song.flac`;
- paths **from the card root**, such as `Music/Artist/Album/01 Song.flac` or
  `/Music/Artist/Album/01 Song.flac`;
- **full computer paths**, such as `/run/media/me/CARD/Music/...`,
  `/Volumes/CARD/Music/...`, `E:\Music\...` or `file:///...` URLs: the
  player drops the leading folders until the rest matches a track;
- Windows backslashes, CRLF line ends and a UTF-8 byte-order mark.

Capital and small letters do not matter (for plain A to Z). A computer-made
playlist with more than 1,000 entries shows and plays its first 1,000; the
rest stay in the file.

### Player-made playlists on a computer

The player writes `/playlists/<NAME>.m3u`, one entry per line, each as `../`
and the track's path from the card root (for example
`../Music/Artist/Album/01 Song.flac`). Any music program that reads M3U opens
them from the card. They are written in UTF-8, so a program that expects
Windows' own character set may show accented names wrongly.

---

## Settings

Main menu → **Settings**. Press `5` or `6` on a row.

| Row | What it does |
|---|---|
| `Rescan library` | Scans the whole card again. Playback stops; `Scanning...` and a count show; then the main menu. Do it after changing the music on the card. If the tracks on the card are the same as before, the queue is kept; otherwise it is emptied. |
| `Backlight: 30 s` | How long the backlight stays on after the last key: `15 s`, `30 s` (the default), `60 s` or `on` (never goes off). Each press moves to the next value. |
| `Sleep: off` | The sleep timer: each press moves to the next of `off`, 15, 30 and 60 minutes, counted from that moment. The row shows the minutes left (`Sleep: 27 min`). When it runs out while music plays, the volume fades out over about 10 seconds, playback stops and the backlight goes off; `D` carries on from there at your normal volume. With nothing playing it only turns the backlight off. Any key press during the fade (or pausing) stops the fade. The timer is then off again. |
| `Keypad test` | Shows `Key test` with a count, and the last key and what it did (`press`, `long`, `repeat`, `release`). The triangle in the corner shows the LCD's own symbols work. Every key is shown, none acts; **hold `*`** to leave. |
| `About` | The number of tracks; then, scrolling, the card's free space and the date the firmware was built (`Free 912 MB  Built Oct  7 2026`). Any key but `A`, `B`, `C`, `D` goes back to Settings. |

---

## What is remembered at power-off

- **Kept:** the volume, shuffle, loop, the backlight setting, the queue, the
  track you were on and the time into it.
- **Not kept:** the sleep timer (it is off after power-on).
- At power-on the player shows Now Playing **paused** on the remembered track
  at the remembered time. Press `D` or `5` to play.
- The place in a track is saved when the track changes, when you pause, when
  you fast-forward or rewind, and every 30 seconds while playing. Changes are
  written a second or so after you stop pressing keys (at most 5 seconds).
  To be sure, pause and wait a few seconds before unplugging.
- If a rescan finds different music, the saved queue no longer fits and
  starts empty.

---

## Building and flashing

You need ESP-IDF v6.0.2, installed at `/opt/esp-idf`.

```bash
source /opt/esp-idf/export.sh      # once per terminal
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

- `idf.py flash` writes the bootloader, the partition table and the
  application. After any change to the partition table always use it (not
  `app-flash`). The project uses ESP-IDF's "single app, large" table: a
  1.5 MB application. Flashing does not erase the saved settings.
- Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it.
- **The serial port** needs your user in the `uucp` group
  (`sudo usermod -aG uucp $USER`, then log in again). In a terminal opened
  before that, run `newgrp uucp` first.
- If the board is on the computer's USB port, re-seat the SD card after
  plugging in (see [Power](#power)).
- Host tests of the hardware-free code are described in `README.md`.

---

## Troubleshooting

| What you see | What to do |
|---|---|
| `Insert SD card` | The card did not start. The player tries again every 2 seconds. Take the card out and click it back in. On a computer's USB port this happens after plug-in when the LCD is attached: re-seat the card once, or use a phone charger. Check the card is FAT32. |
| The card was taken out while the player was running | The player only looks for the card when it starts. Put it back and reset the board (the EN button) or unplug and replug the power. |
| `Can't play` (bottom row of Now Playing) | The file would not decode (damaged, not really FLAC/MP3, or an unusual sample rate). The player skips to the next track after 1.5 s. After three in a row it stops. |
| `File missing` | The file has gone since the last scan. The player skips it. Run **Rescan library** after changing the card. |
| `(missing)` in a playlist | See [Entries marked (missing)](#entries-marked-missing). |
| Not all your music is listed | The index holds about 7,400 tracks. A larger card is scanned until the index is full and the rest is left out. The screen does not say so; the serial log says `full`. Also check for files more than 8 folders deep or with very long paths, and that you ran **Rescan library**. |
| `Card error` | Reading or writing a playlist failed. Try again; if it keeps happening, check the card on a computer. On the Playlists screen it may show as `>Card error`: press `5` to try again. |
| `Library busy` | A scan is running (or the last scan failed). Wait for it to finish. |
| No text on the LCD, backlight on | The LCD is on 3.3V: move its VDD wire to **5V**. If it has a contrast screw, turn it. |
| LCD dark, no backlight | Check its four wires. Or the backlight timed out: press any key. |
| LCD blanks or shows garbage for a moment | Check the LCD's wires and connections. The player recovers by itself. |
| Keys do random things | A column's 10k pull-up to 3.3V is missing or loose. The serial log at start-up names the column. |
| Wrong key shows | The keypad wires are in the wrong order. Use **Keypad test** to see. |
| A click, buzz or dropout in the sound | Not expected. Note what was playing and what you pressed, and report it (a serial log helps: `python3 tools/serial_capture.py /dev/ttyUSB0 60`). |
| `Out of memory` | The player could not start. Report it. |

---

## The serial console

For testing and fault-finding, not everyday use. The player writes a log on
the USB serial port (115200 baud) and reads commands from it, one per line;
each is answered with a log line `dbg: ok ...` or `dbg: err ...`.

```bash
idf.py -p /dev/ttyUSB0 monitor        # type commands; Ctrl+] to quit
python3 tools/serial_capture.py /dev/ttyUSB0 60   # reset and log 60 s
```

Main commands:

| Command | What it does |
|---|---|
| `stat` | Player state, position, volume, memory and the UI's numbers (backlight, sleep timer). |
| `screen` | What the LCD shows. |
| `key <k>`, `long <k>`, `hold <k> <n>` | Press a key, long-press it, or hold it for `n` repeats. |
| `play <path> [ms]`, `pause`, `resume`, `stop`, `seek <ms>`, `vol <n>` | Drive the player directly (not saved). |
| `queue [from [count]]` | What the queue holds. |
| `lib`, `lib artists`, `lib find <path>`, ... | What the library index holds. |
| `scan` | Rescan the card. |
| `state` | What is saved for the next power-on. `state clear` erases it. |
| `pl ls`, `pl show <n>`, `pl new <name>`, `pl add`, `pl rm`, `pl mv`, `pl del`, `pl free`, ... | Playlists. |

Every command and the exact form of its answers are described at the top of
`main/debug_console.h`.
