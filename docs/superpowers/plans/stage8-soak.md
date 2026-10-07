# Stage 8, Task 5: an hour of play, and memory over many tracks

Date: 2026-10-07. Board: Freenove ESP32-WROVER, ESP-IDF v6.0.2, `esp_audio_codec`
2.6.2. Two runs of 62 minutes each, the same script both times:

| Run | Firmware | Length |
|---|---|---|
| 1 | `4d6bb23` (stage 7 player + a MEM line per track and heap figures in `stat`) | 62.0 min |
| 2 | `8c2c1a4` (the fix below) | 62.0 min |

The USB link stayed up through both runs, and the card mounted on the first
try each time.

## What ran

`tools/soak.py /dev/ttyUSB0 62 <log>` resets the board and makes a playlist
`SOAK` with `pl new` (18 tracks: both `/music/` tracks, every file in `/test/`
including t07 (empty), t08 (not audio) and t09 (cut short), and six `/lib/`
tracks of 100 to 130 ms, so that tracks change often). It plays the playlist
with the keys (Playlists, `#`, Play), sets loop all with `3`, and opens About
at once (the first free-space read after a boot, during playback). Then,
every 150 s, the next action in this list, round and round (each ran 3 times
per run):

- volume `A A B B`
- pause `D`, 5 s, resume `D`
- `hold 6 12`, then `hold 4 6` (fast-forward, rewind)
- `key 6`, `key 6`, `key 4` (skips)
- an artist's tracks added with `#` > Add to queue (Bjork, Queen, Radiohead in
  turn; the queue grew from 18 to 26)
- console `seek 8000`, `seek -4000`
- a playlist edit of `SOAKEDIT` with `pl add`, `pl up`, `pl rm`, `pl mv`
- About

Shuffle went on at 20.7 min and off at 41.3 min. The backlight stayed at 30 s;
before each key action the script asked `stat` whether it was lit and, when
it was not (18 times in each run), pressed `0`, which logged `woke the
backlight and does nothing else`. A `stat` every 60 s. At the end:
`queue`, both playlists deleted, `stop`, `state clear` (the last command).
`tools/soak_report.py <log>` produces the figures below.

## Results

| | Run 1 | Run 2 |
|---|---|---|
| track starts | 121 | 121 |
| ENDED | 111 | 111 |
| ERROR (t07, t08, every time round) | 12 | 12 |
| cut off by a skip | 8 | 8 |
| FLACs played start to end, no seek | 59, all `frames == expected` | 61, all `frames == expected` |
| t09 (truncated on purpose) | 258048 of 441000, as always | same |
| underruns at the end | 0 | 0 |
| decoder / output / UI stack never used (min) | 11528 / 3012 / 6104 | 11520 / 3008 / 5856 |
| `AUD_Dec_Parse: Not found frame` | 5, all t09 | 5, all t09 |

Memory, internal data RAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`), in bytes,
from the MEM line at each track's STARTED (the state while it plays) and at
ENDED (nothing open):

| | Run 1 (before) | Run 2 (after) |
|---|---|---|
| boot, before the first track: free / largest | 174167 / 61440 | 117367 / 77824 |
| between tracks: free / largest | 102291-107355 / 47104 every time | 45483-50555 / 38912 every time |
| MP3 playing: free / largest | 74135-74567 / 47104 | 45867-46303 / 38912 |
| FLAC, small frames (t01, t06, t09, /lib/): free / largest | 51643-52463 / 14336 | 45879-46167 / 38912 |
| FLAC t03 (mono): free / largest | 68567-68839 / 30720 | 45891-46155 / 38912 |
| FLAC t02 (96 kHz 24-bit, 9 KB frames): free / largest | 42451-42723 / 14336 | 36683-36947 / 29696 |
| FLAC t10 and Hey Mama (13.7 KB frames): free / largest | 37315-37579 / 14336 | 32059-32323 / 24576 |
| least ever free (`int_min`) | 22419 | 23039 |
| codec pool least ever free | - | 4004 of 57344 (Hey Mama) |
| PSRAM free: FLAC / MP3 / idle | 1522992 / 1556792 / 1557308 | the same |
| PSRAM largest: FLAC / idle | 1507328 / 1540096 | the same |

Trend: none in either run. Each file shows the same figures every time it
plays, within a few hundred bytes. Taking the highest free figure between
tracks in each 5-minute window, run 2 goes 50555, 50455, 50423, then stays
between 50283 and 50423 to the end; run 1 behaves the same way at 107355,
then 107071 to 107219. The 100 to 300 bytes lost in the first ten minutes,
then steady, are small caches filling (NVS saves, FatFs, the console); they
do not grow. PSRAM is identical at every STARTED of the same format and at
every ENDED.

## What allocates per track, and why the largest block was 14 KB

Found with ESP-IDF's heap tracing (standalone, leaks mode, around a track)
and a heap walk, in a temporary build, then confirmed by logging every call
to the codec library's allocator:

- **The codec library**, through four weak functions
  (`media_lib_module_malloc/_calloc/_realloc`, `media_lib_free`) whose default
  is plain malloc. With `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, a block of
  up to 16 KB goes to internal RAM wherever there is room (PSRAM if there is
  none), a larger one to PSRAM.
  - A stereo FLAC decoder: state 8416, bit reader 8192, two residual arrays of
    16384 (internal, exactly at the limit), two output arrays of 16400 (so in
    PSRAM, by 16 bytes), rice tables 4 x 256 growing to 4 x 512, and small
    pieces: about 52 KB internal and 33 KB PSRAM.
  - An MP3 decoder: 4224, 2312 and 2304 twice each, 8192, 1968 and small
    pieces: about 28 KB internal.
  - The parser's frame buffer (module `ES_Parse`), reallocated to the
    stream's largest frame: 627 bytes for an MP3, 2518 to 13736 for the FLACs
    here (internal while 16 KB or less).
- **stdio**: the `FILE` (76 + 84 bytes, a lock) and its 4096-byte buffer.
- **The path** copy (`strdup`, about 40 bytes).

All of it is given back when the track ends: the trace shows 32 allocations
and 32 frees across a FLAC. Nothing leaked.

Once, at the first track after boot, the I2S channel is made and kept: its
DMA ring is 16 x 4000 bytes of internal RAM (`audio_out.c`). That is the drop
from 174 KB to 107 KB in run 1 (61 to 47 KB largest), and it never comes back,
by design.

Why 14 KB: after the I2S ring, the largest free block is 47 KB (the top of the
D/IRAM region). A FLAC's two 16 KB residual arrays are the only requests that
need a block that big, so they go there, leaving 14 KB, and the parser buffer
then takes most of what is left. It recovered at every ENDED (47104 again),
so it did not shrink over the run, but it moved with the format, and the
decoder's memory went wherever there was room at that moment. If a 16 KB block
had not been free when a FLAC opened, malloc would have put the arrays in
PSRAM without a word. That is not a failed allocation but it is a slow one:
with all of the codec's memory in PSRAM (an experiment), t02 (96 kHz 24-bit)
took 9949 ms of decoder time for 10.2 s of audio (97%), against 6039 ms (60%);
the ring never filled. Of the allocations named in the brief: FatFs `FIL`s are
made at mount (VFS), not per open; the SD bounce buffer is a small DMA block
and the 32 KB internal reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`) is
kept for such requests; stdio buffers fall back to PSRAM. None of those could
fail at these levels. The decoder's placement was the real risk.

## The fix (`8c2c1a4`, `main/player.c`)

The player now defines the four `media_lib_*` functions. Blocks of up to
16 KB come from a private heap (`multi_heap`) over a 56 KB array in `.bss`,
set up in `player_start` before the codecs are registered; larger blocks, and
any request the pool cannot serve, go to malloc as before. So:

- a decoder's internal memory is always internal and always the same RAM,
  set aside once at start-up; the general heap never sees it;
- the decoders' speed is as before: the pool is internal RAM, and the blocks
  that were in PSRAM (the FLAC output arrays) still are;
- the pool holds one decoder at a time: a stereo FLAC uses 51024 to 52052 of
  its 57344 bytes (the 1288 not counted are the pool's own bookkeeping and
  the registration's small blocks), an MP3 28536. `stat` prints
  `codec_pool=`, `codec_pool_free=` and `codec_pool_min=`.

The parser's frame buffer is the one exception, left to malloc. In PSRAM it
cost 4% to 17% more decoder time (t01 1694 against 1626 ms, t02 6695 against
6039, t10 3379 against 2879, Hey Mama 20845 against 18272 ms per minute): the
parser scans it byte by byte for the next frame header. In the pool it would
make the pool up to 16 KB bigger for every format. So it still comes from the
general heap and is the one thing there that changes with the track: by the
largest frame of the stream, up to 16 KB.

Measured after the fix, against the figures above: the largest free block is
38912 between tracks and with every MP3 and small-frame FLAC, 29696 with t02
and 24576 with the 13.7 KB-frame files (it was 14336 with any stereo FLAC).
The least-ever free is 23039, against 22419. In run 2 it was 25491 from
9.5 min on and fell to 23039 at 47.8 min, right after a fast-forward and
rewind in Hey Mama: a seek resets the decoder (`dec_seek`), and the parser
logs its frame size again and allocates its buffer anew. That is the only
drop seen, so it is likely but not proven to be the cause. The cost: the pool is reserved
always, so free internal RAM between tracks and during an MP3 is about 56 KB
less than before (50 KB between tracks, 46 KB with an MP3, against 107 and
74). With a FLAC the free total is about 5 KB less than before (the pool's
unused 4-5 KB and its bookkeeping), and the largest block is 10 to 24 KB
bigger.

## Regression after the change

Every file in `/test/` and both `/music/` tracks played to their end in one
boot on `8c2c1a4` (console `play`, one after another):

| File | Result | decoder ms / wall ms |
|---|---|---|
| t01_44k_16_st.flac | ENDED 441000 / 441000 | 1632 / 10077 |
| t02_96k_24_st.flac | ENDED 960000 / 960000 | 6053 / 10166 |
| t03_48k_16_mono.flac | ENDED 480000 / 480000 | 773 / 10049 |
| t04_44k_cbr192_st.mp3 | ENDED 441000 (expected 0: MP3) | 1621 / 10071 |
| t05_48k_vbr_mono.mp3 | ENDED 480000 | 686 / 10046 |
| t06_tone_30s.flac | ENDED 1323000 / 1323000 | 3709 / 30067 |
| t07_empty.flac | ERROR decode | |
| t08_not_audio.flac | ERROR decode | |
| t09_truncated.flac | ENDED 258048 / 441000 (cut short on purpose, as before) | 1006 / 5932 |
| t10_loud_quiet_44k_16_st.flac | ENDED 882000 / 882000 | 2852 / 20094 |
| 16 - Kanye West - Hey Mama.flac | ENDED 13453440 / 13453440 | 84681 / 305270 |
| Good Time.mp3 | ENDED 9069039 | 28479 / 205717 |

`underruns=0` throughout. Decoder time matches stage 7 (t02 6039 before,
6053 after; t10 2879 / 2852).

## Seen, not fixed

- **LCD I2C timeouts.** `i2c.master: I2C transaction timeout detected` and
  `lcd: LCD transfer failed (ESP_ERR_INVALID_RESPONSE); will re-initialise on
  the next update` came 39 times in run 1 and 29 times in run 2, spread evenly
  over the hour, with probe timeouts while the driver re-initialised. The LCD
  driver recovers by design (stage 2). Not memory, not the player; it looks
  like the breadboard I2C wiring or pull-ups. Not investigated. Whether the
  screen ever showed wrong characters needs a person to look.
- **The parser buffer** still comes from the general heap (above).
- **stdio's 4 KB buffer** per open file is still allocated and freed per
  track, as it is for every file the other tasks open.
- **Audio** was not listened to: frame counts and underruns are the evidence.
