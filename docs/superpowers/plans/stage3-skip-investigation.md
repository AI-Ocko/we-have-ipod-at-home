# Stage 3, Task 3: the skipped audio in the FLAC decode loop

Date: 2026-10-01. Board: Freenove ESP32-WROVER, ESP-IDF v6.0.2, `-O2`, 240 MHz,
`esp_audio_codec` 2.6.2. Starting commit: d79c4f7.

## Summary

Two separate things were shortening `/sdcard/Clay Pigeons.flac`. Only one of
them is a firmware defect.

1. **The loop's stall guard (firmware defect, fixed).** `player_play_file` had
   `if (raw.consumed == 0) { break; }`. The decoder legitimately returns
   `consumed == 0` together with a decoded block whenever its parser already
   holds a complete frame. The guard took that for "no progress" and abandoned
   the rest of the 32 KB input chunk. On this track that discarded 71540 bytes
   of the file in four places and cost 98304 frames (24 blocks, 2.23 s).
2. **The copy of the file on the card is damaged (not fixable in firmware).**
   Bytes 491520 to 507391 of the file (31 sectors) do not hold FLAC data; they
   hold a run of counting 32-bit numbers. Three FLAC frames (numbers 42, 43
   and 44; 12288 frames, 0.28 s) cannot be decoded by anything. This, and only
   this, produces the library's `Not found frame in max frame size 10507`
   line.

After the fix the player delivers 11252616 frames of the 11264904 the header
promises. 11252616 is exactly the number of samples in the frames of the file
that are intact (counted independently, see experiment 3). The acceptance
target "frames equal to the header's total and no parser error" cannot be met
with this copy of the file; it needs a fresh copy of the track on the card.

## Symptom

Baseline (`stage1-baseline.log`) and reproduced on d79c4f7 with the real,
audible path (experiment 0):

```
I (1653) AUD_Dec_Parse: FLAC sample_rate 44100 duration:255440 max_frame:10507
I (1773) player: FLAC: 44100 Hz, 2 channels, 16 bits
E (5713) AUD_Dec_Parse: Not found frame in max frame size 10507
I (254723) player: Finished: 2718 blocks decoded
```

The header says 11264904 frames (255.44 s, 2751 FLAC frames of 4096). Playing
took 252.95 s.

## Method

A temporary decode-only harness (not committed) ran a copy of the loop without
the I2S writes, counted PCM bytes, timed it with `esp_timer`, and recorded
every `esp_audio_simple_dec_process` call in RAM: file offset of `raw.buffer`,
`raw.len` before, `raw.consumed`, `decoded_size`, return code, `needed_size`.
The records were printed after the run, so the logging did not disturb the
loop. Switches selected: the guard (old / none / corrected), the chunk size,
and how `eos` is set. One switch changed per experiment.

The harness also contained an independent FLAC frame walker that does not use
the decoder library: it accepts a frame only if its header parses, the
header's CRC-8 is right and the CRC-16 over the whole frame is right. This
gives the true frame boundaries and the true number of decodable samples.

## Experiments, in order

### 0. Reproduce on unmodified firmware (real path)

Change: none. Result: `Finished: 2718 blocks decoded`, one parser error at
5713 ms. Conclusion: the fault is deterministic and present at d79c4f7.

### 1. Decode-only copy of the old loop, counting frames

Change: no I2S writes; count bytes. Result:

```
frames=11154312 expected=11264904 blocks=2718 calls=3386 zero_consumed=4 breaks=4 dropped_bytes=71540 grows=3
E AUD_Dec_Parse: Not found frame in max frame size 10507   (one line)
```

Conclusions:
- The decode-only path reproduces the fault exactly (same 2718, same single
  error), so timing and I2S are not involved.
- The real shortfall is 110592 frames (27 blocks of 4096, 2.51 s), not "about
  33 blocks". The old log counted calls that returned data, and after the
  output buffer had grown some calls returned 3 or 5 blocks at once (49152 and
  81920 bytes), so "2718 blocks" under-counted what was played. 2.51 s agrees
  with the 252.95 s the real path took.
- The `consumed == 0` break fired four times and threw away 71540 input bytes.

### 2. Full call trace of the old loop

First call that is not ordinary (offset, len, consumed, decoded, return,
needed):

```
call 55  off=479660  len=11860 cons=8106  dec=16384 ret=0
call 56  off=487766  len=3754  cons=3754  dec=0     ret=0
call 57  off=491520  len=32768 cons=0     dec=0     ret=-8 (BUFF_NOT_ENOUGH) needed=49152   <- parser error logged here
call 58  off=491520  len=32768 cons=23706 dec=16384 ret=0
call 59  off=515226  len=9062  cons=8611  dec=16384 ret=0
```

At call 58 the decoder swallowed 23706 bytes (about three frames) and gave
one block. The loop did nothing unusual before this point: every byte of the
file up to here had been handed over in order. So the first fault is not
caused by the loop.

The first break is 20 MB later:

```
call 3169 off=20628701 len=15139 cons=6378 dec=16384 ret=0
call 3170 off=20635079 len=8761  cons=0    dec=16384 ret=0   <- old loop breaks, drops 8761 bytes
call 3171 off=20643840 len=32768 cons=10302 dec=49152 ret=0
call 3172 off=20654142 len=22466 cons=0    dec=16384 ret=0   <- breaks, drops 22466 bytes
call 3173 off=20676608 len=32768 cons=0    dec=0     ret=-8 needed=65536
call 3174 off=20676608 len=32768 cons=9130 dec=16384 ret=0
call 3175 off=20685738 len=23638 cons=0    dec=16384 ret=0   <- breaks, drops 23638 bytes
call 3176 off=20709376 len=32768 cons=0    dec=0     ret=-8 needed=81920
...
call 3179 off=20725469 len=16675 cons=0    dec=16384 ret=0   <- breaks, drops 16675 bytes
call 3180 off=20742144 len=32768 cons=16119 dec=81920 ret=0
```

Conclusion: two different events. They are examined separately below.

### 3. What is in the file at offset 491520? (frame walker and hex dump)

Change: none to the loop; walk the file's frames and dump bytes 479232 to
528383 over serial. Result:

```
HOLE: frame number 42 at off=484736 has no valid end within 20000 bytes
RESYNC: frame number 45 at off=508118 (bad span 484736..508118, 23382 bytes)
SCAN frames=2748 samples=11252616 expected=11264904 min_frame=16 max_frame=10489 hdr_max=10489 holes=1
```

The dump, analysed on the computer: valid frame headers at 484736 (frame 42),
508118 (45), 515226 (46), 523837 (47). There is no header for frames 43 or 44
anywhere, and frame 42's CRC-16 is wrong. The bytes themselves:

```
491504  4a905eef6b0256bed137accb8f4724e8   <- FLAC data (looks random)
491520  70d2100b 71d2100b 72d2100b 73d2100b 74d2100b ...   <- 32-bit little-endian counter
...                                                        0x0B10D270 up to 0x0B10E1EF
507392  dbc7ca0d58141a76...                 <- looks random again
```

3968 consecutive counting words = 15872 bytes = 31 sectors, starting exactly
at file offset 491520 (0x78000). The same bytes come back through three
different read paths (32 KB reads into the DMA buffer, unaligned reads into a
48 KB window, 32-byte reads through the FAT sector cache) and on every boot,
and the baseline log from stage 1 already shows the same error at the same
time. So this is what the card holds, not a read fault.

Conclusion: the parser error and the loss of frames 42 to 44 (12288 frames)
are caused by damage to the file on the card. The library's message is
accurate: after frame 42's header there really is no next frame within the
maximum frame size. No change to the loop can recover those frames. The file
contains 2748 intact frames holding 11252616 samples; that is the most any
player can produce from it.

### 4. Old guard on the four test files

Change: file only. Result: `t01` 441000/441000, `t02` 960000/960000, `t03`
480000/480000, `t06` 1323000/1323000, `zero_consumed=0` in all four.
Conclusion: these files never make the decoder return `consumed == 0`, so they
cannot show the defect either way. All their frames are close to the file's
maximum frame size.

### 5. Remove the guard (Espressif's reference loop), nothing else

Change: `if (raw.consumed == 0) break;` removed. Result:

```
Clay Pigeons: frames=11252616 expected=11264904 calls=3418 zero_consumed=24 breaks=0 grows=1   (one parser error line, as before)
t01, t02, t03, t06: exact, unchanged
```

11252616 = 11264904 - 12288: every frame the file really contains. The 24
calls with `consumed == 0` each returned one block. Matching the trace against
the true frame offsets from experiment 3 shows why: at call 3169 the bytes
handed over so far reach into frame 2544 while only frames up to 2542 have
come out, so a complete frame (2543) is already inside the parser, and call
3170 returns it without needing input. This happens from offset 20635079 on,
where the track fades and frames shrink from about 6.5 KB to 5 KB and less
(half the file's 10489-byte maximum, which is how much the parser gathers
before it looks for the next frame header).

Conclusion: `consumed == 0` with `decoded_size > 0` is normal behaviour, not a
stall. The guard's `break` is what loses the audio after the damaged region.

### 6. Controlled confirmation on an intact stream

Because the only file that triggers the guard is also damaged, a clean stream
with the triggering property was made on the computer (1 s of loud noise, then
5 s of a quiet tone; 264600 frames; frame sizes 8205 bytes falling to about
1207; verified by `flac -a`) and embedded in the temporary firmware, decoded
from memory in chunks. One switch per run:

| Run | Guard | Chunk | `eos` | frames (expected 264600) | zero-consumed calls |
|---|---|---|---|---|---|
| S1 | old | 32768 | on short read | **57344** | 2 (both break) |
| S2 | none | 32768 | on short read | **264600** | 44 |
| S3 | old | 4096 | on short read | 106496 | 12 |
| S4 | none | 4096 | on short read | 264600 | 44 |
| S5 | old | 512 | on short read | 225280 | 27 |
| S6 | none | 512 | on short read | 264600 | 44 |
| S7 | none | 32768 | never | 241664 | 40 |
| S8 | corrected (`consumed == 0 && decoded_size == 0`) | 32768 | on short read | **264600** | 44, no break |
| S9 | old again | 32768 | on short read | **57344** | 2 (both break) |

Conclusions:
- With the old guard the count is short; without it, or with the corrected
  condition, it is exact; putting the old guard back brings the shortfall
  back (S1, S2, S8, S9). No parser error line appears in any of these runs.
- The chunk size is not the cause. It only changes how much each break throws
  away (S3, S5), and the count is exact at every chunk size once the guard no
  longer fires (S4, S6).
- The `eos` flag is needed and was already right: without it the tail of the
  stream stays in the parser (S7).
- `ESP_AUDIO_ERR_BUFF_NOT_ENOUGH` retries are not a cause: the output buffer
  never had to grow in any run where the guard did not fire. It grew only
  after breaks (twice each in S3 and S5, twice in experiment 1), where the
  cut stream makes the decoder ask for more room, and once at the damaged
  region of Clay Pigeons.

### 7. Corrected guard on Clay Pigeons and on the bad files

Change: guard condition `consumed == 0 && decoded_size == 0`. Result: Clay
Pigeons 11252616 (as experiment 5), `breaks=0`. The empty, not-audio and
truncated test files (`t07`, `t08`, `t09`) end without the guard firing and
without spinning, with either no guard or the corrected one (`t08` ends with
the library's "not supported" return; `t09` gives 258048 frames and a parser
error where the file is cut off). Conclusion: the state the guard exists
for (return OK, nothing consumed, nothing decoded) was never seen. The
corrected guard costs nothing and still protects against it.

## Root cause

`main/player.c`, decode loop: `if (raw.consumed == 0) { break; }`, commented
"Guard against spinning forever if the decoder stops making progress".

The simple decoder's FLAC parser gathers input until it holds about one
maximum-size frame, then looks for the next frame header. When the frames at
that point of the track are much smaller than the file's maximum (quiet
passages, fades), what it holds contains more than one complete frame. It
returns them one per call, and the calls after the first take no new input:
`consumed == 0`, `decoded_size > 0`, return OK. The guard treated that as a
stall and left the inner loop, so the unconsumed remainder of the 32 KB chunk
(`raw.len` bytes) was never given to the decoder. The parser then saw a jump
in the stream, resynchronised, and frames were lost.

Evidence: experiment 2 (each break discards `raw.len` bytes), experiment 5
(removing only the guard recovers every intact frame), experiment 6 (exact
with the guard removed or corrected, short again when it is restored, at
three chunk sizes).

The cause is in our loop, not in the prebuilt library.

## Fix

The guard keeps its purpose but uses the right test for "no progress": no
input consumed and no output produced.

```c
if (raw.consumed == 0 && frame.decoded_size == 0) {
  break;
}
```

Nothing else in the loop changed. Also in the commit, as the task requires:
the end-of-track log is now `ENDED frames=<played> expected=<total>`, with
`expected` from `flac_header_read` and `played` counted in PCM frames (bytes
written to the output divided by the frame size, one addition per block and
one division at the end).

## Acceptance (real, audible path, fixed firmware)

| File | frames played | expected | parser error lines |
|---|---|---|---|
| `Clay Pigeons.flac` | **11252616** | 11264904 | **1** |
| `t01_44k_16_st.flac` | 441000 | 441000 | 0 |
| `t03_48k_16_mono.flac` | 480000 | 480000 | 0 |
| `t06_tone_30s.flac` | 1323000 | 1323000 | 0 |
| `t02_96k_24_st.flac` | did not play | 960000 | 0 |

```
I (1784) player: FLAC: 44100 Hz, 2 channels, 16 bits
E (5724) AUD_Dec_Parse: Not found frame in max frame size 10507
I (256954) player: ENDED frames=11252616 expected=11264904
```

Clay Pigeons does **not** meet the stated target (frames equal to expected,
no parser error). It plays every frame the damaged file contains: the 12288
missing frames and the one error line are frames 42 to 44, destroyed on the
card. Playing now takes 255.17 s (it took 252.95 s before the fix), which is
11252616 frames at 44100 Hz.

`t01`, `t03` and `t06` were played in one boot by temporary code that chose
the files; Clay Pigeons was played by the committed firmware, which found it
as the first FLAC on the card.

`t02` cannot be played by the current output code at all: `audio_out_start`
asks the I2S driver for 24-bit slots and the driver refuses.

```
I player: FLAC: 96000 Hz, 2 channels, 24 bits
W i2s_common: dma frame num is out of dma buffer size, limited to 511
E i2s_std: i2s_std_set_clock(80): The 'mclk_multiple' should be the multiple of 3 while using 24-bit data width
E i2s_std: i2s_channel_init_std_mode(340): initialize channel failed while setting clock
E player: Cannot start audio output (ESP_ERR_INVALID_ARG)
```

Decode-only, `t02` gives 960000 of 960000 frames (experiments 4 and 5). When
`t02` is tried after another track in the same boot, the failed
reconfiguration also leaves the I2S channel unusable for the tracks that
follow until reset. Both belong to the output rework (32-bit slots for every
format), not to this task, and were left alone.

## Decode speed (decode-only, `-O2`, 240 MHz, 32 KB chunks)

| File | audio | wall time incl. SD reads | inside `process()` only | speed incl. reads | decoder alone |
|---|---|---|---|---|---|
| `Clay Pigeons.flac` (44.1 kHz 16-bit stereo) | 255.44 s | 63.4 s | 52.8 s | 4.0 x real time | 4.8 x |
| `t02` (96 kHz 24-bit stereo) | 10.00 s | 6.29 s | 5.26 s | 1.6 x real time | 1.9 x |
| `t01` (44.1 kHz 16-bit stereo tone) | 10.00 s | 1.89 s | 1.75 s | 5.3 x | 5.7 x |
| `t03` (48 kHz 16-bit mono tone) | 10.00 s | 0.81 s | 0.71 s | 12.3 x | 14.0 x |
| `t06` (44.1 kHz 16-bit stereo tone) | 30.00 s | 4.88 s | 4.61 s | 6.2 x | 6.5 x |

Repeat runs agreed to within 1%. The 96 kHz 24-bit margin is small: the
decoder alone uses about 53% of a core, and reading takes it to 63%, before
any conversion to 32-bit frames or volume scaling.

## How the decoder delivers 24-bit audio

Packed, 3 bytes per sample, little-endian, signed, interleaved left then
right: 6 bytes per stereo frame, with no padding byte and no justification
question (all 24 bits are the sample).

- `esp_audio_simple_dec_get_info` reports `bits_per_sample = 24`, `channel = 2`.
- `t02`: 5760000 bytes for 960000 frames = 6 bytes per frame.
- First bytes of the first block:
  `000000 000000 d17400 3aaf00 a6e800 785d01 785d01 b50b02 4cd101 f5b802`
  read as 3-byte little-endian: left 0, 29905, 59558, 89464, 119116;
  right 0, 44858, 89464, 134069, 178421.
- The reference decoder (`flac -d` of the same file on the computer) gives
  exactly the same values. The tone's peak in the file is 1037837 (0.99 x
  2^20, about 0.124 of 24-bit full scale), which only fits this reading: taken
  as 16-bit or as 32-bit words the values are not a sine at all.

To make a left-justified 32-bit sample: `(int32_t)((b0 << 8) | (b1 << 16) |
(b2 << 24))`.

## Open questions

- **Where the 31 sectors of counting numbers came from.** They look like a
  test pattern written straight to the card (each word one more than the
  last, starting 0x0B10D270), or stale contents of a block the card
  substituted. Not investigated further. Whether other files on the card are
  affected is unknown; the four FLAC test files decode exactly, so they are
  intact. The card is an old 960 MB SDSC and may be worth checking or
  replacing.
- **`Clay Pigeons.flac` needs replacing on the card** before it can serve as
  the full-count acceptance file. The original is not on the computer.
- No file in the test set exercises the fixed path on the board: `t01` to
  `t06` never return `consumed == 0`. A test file with a loud and a quiet
  section (like the temporary one in experiment 6) would. Until one exists,
  the intact part of `Clay Pigeons.flac` after 20.6 MB is the only on-card
  check.
- The guard's remaining case (OK, nothing consumed, nothing decoded) has never
  been observed. If it ever fires it still drops the rest of the chunk
  silently; the later player task that reports `ERROR` events should decide
  what to do there.
- After the damaged region the library asks for a bigger output buffer
  (`needed_size` 49152) and then returns a single block. Why it asks for three
  blocks there is not understood; it has no effect on intact data.
