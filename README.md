# ESP32 Digital Audio Player

A standalone music player for the Freenove ESP32-WROVER CAM board: FLAC and
MP3 from an SD card, a PCM5102A I2S DAC, a 16x2 LCD and a 4x4 keypad.

**To wire it up and use it, read the user guide: [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md).**

## Features

- FLAC and MP3, 8 to 96 kHz, sent to the DAC as 24-bit audio; volume in 21
  steps.
- A library index of the whole card (tags, any folder layout), browsed by
  artist, album, song and playlist; letter jumps in A-to-Z lists. A card with
  more tracks than the index holds (about 7,400) is scanned until it is full
  and says `Library full`.
- Recovery from a card pulled out while running, with no reset: the player
  waits for it, reloads the library and carries on with the same queue.
- A play queue: play next, add to queue, shuffle, loop all or one.
- M3U playlists made and edited on the device (create, add, rename, delete,
  reorder, remove, save the queue), and computer-made playlists read with
  most path styles.
- Resume at power-on: volume, shuffle, loop, backlight, the queue and the
  place in the track.
- Fast-forward and rewind by holding a key, a backlight timeout, a sleep
  timer with a fade, an About screen and a keypad test.
- A serial console for testing and fault-finding (`main/debug_console.h`).

## Build and flash

```bash
source /opt/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

ESP-IDF v6.0.2. `sdkconfig` is generated from `sdkconfig.defaults`; change
settings there. The serial port needs the `uucp` group. More in the user
guide.

## Tests

Hardware-free logic lives in `components/dap_core` and is tested on the host:

```bash
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure
```

## Serial log

```bash
python3 tools/serial_capture.py /dev/ttyUSB0 60
```

## Where things are

- User guide: `docs/USER_GUIDE.md`
- Wiring notes: `docs/wiring.md`; pin assignments: `main/pins.h`
- Design: `docs/superpowers/specs/2026-09-30-dap-design.md`
- Stage plans: `docs/superpowers/plans/` (the stage 6 and 7 plans record,
  under "Amendments made during implementation", where the code departs
  from the plan)
- The hour-long soak test: `docs/superpowers/plans/stage8-soak.md`, run with
  `tools/soak.py` and summed up with `tools/soak_report.py`
- The ESP-IDF example this grew from: `reference/`
