# Stage 3, Task 5b: the lost final block at end of stream

Date: 2026-10-01. Board: Freenove ESP32-WROVER, ESP-IDF v6.0.2, `-O2`, 240 MHz,
`esp_audio_codec` 2.6.2 (prebuilt `libesp_audio_simple_dec.a`, es_parser
v1.0.1). Starting commit: b502b17. Follows `stage3-skip-investigation.md`.

## Summary

The defect is in the codec library, not in our loop. Its FLAC frame parser
(`esp_flac_dec_parse_frame`) mishandles the last frame of a stream whenever
the two bytes that begin a frame header (0xFF 0xF8 or 0xFF 0xF9) happen to
occur inside that frame, anywhere except its last 16 bytes. It then discards
the frame, logs `Not found frame in max frame size N`, and the track ends one
block short.

`t10` trips it because very quiet audio encodes to long runs of 1 bits, so
nearly every frame contains such a byte pair. Ordinary music trips it by
chance: the odds are about one in eight for a 4 KB final frame.

The fix uses the library's documented way to replace a parser
(`esp_audio_simple_dec_register`): our parser calls the library's and corrects
that one decision. With it `t10` plays 882000 of 882000 frames with no parser
error; switching only that correction off brings the fault back.

## Symptom

```
I AUD_Dec_Parse: FLAC sample_rate 44100 duration:20000 max_frame:13421
E AUD_Dec_Parse: Not found frame in max frame size 13421        (at the very end)
I player: ENDED frames=880640 expected=882000
```

880640 = 215 x 4096. `flac -a` on the computer copy: 216 frames; the last,
number 215, starts at byte 833832, is 229 bytes long and holds 1360 samples.
That frame is the one that never comes out. The file is 834061 bytes = 25 x
32768 + 14861, so the last read returns 14861 bytes and sets `eos`.

## Method

The decode-only harness from Task 3 (temporary, not committed) recorded every
`esp_audio_simple_dec_process` call: file offset, `raw.len` before,
`raw.consumed`, `decoded_size`, return code, `eos`. Small test streams were
generated on the computer with a script and `flac --best`, checked with
`flac -a`, embedded in the temporary firmware and decoded from memory, so no
files had to be copied to the card. One thing varied per experiment.

## Experiments, in order

### 1. Trace the end of `t10` with the current loop

Last calls (offset, len, consumed, decoded, return, eos):

```
call 201  off=819200 len=14861 cons=2656  dec=16384 ret=0 eos=1   <- last read
calls 202..220  off=821856 len=12205 cons=0  dec=16384 ret=0 eos=1   (19 cached frames, no input taken)
call 221  off=821856 len=12205 cons=12204 dec=16384 ret=0 eos=1
calls 222..239  off=834060 len=1 cons=0   dec=16384 ret=0 eos=1   (18 cached frames)
call 240  off=834060 len=1 cons=1   dec=0     ret=0 eos=1   <- parser error logged here
```

Conclusions:
- Our loop does not stop early. The parser leaves the file's last byte
  unconsumed until it has handed out every cached frame, so `raw.len` stays
  above zero and the loop keeps calling. All 215 full frames come out.
- `eos` is set on every call of the last read, including the one that
  matters.
- The last call takes the last byte with `eos` set, returns OK with no data,
  and the library logs its error. At that point the parser holds exactly the
  229 bytes of frame 215. The fault is in what the library does with them.

For comparison, `t01` ends `len=1649 cons=1649 dec=10912`: the last call
consumes the last frame and returns it.

### 2. Does the read chunk size or the final read boundary matter?

Change: chunk 4096 and 512 instead of 32768, on `t10` from the card. Result:
880640 both times, same error. Conclusion: no. Where the reads fall is
irrelevant.

### 3. Size of the last frame, and loud frames before it

Four embedded streams: two blocks of noise (frames of 13.4 KB) then 20 blocks
of the quiet tone (650 bytes), ending in a partial block.

| Stream | What varies | last frame | frames (expected) |
|---|---|---|---|
| v1 | `t10` in miniature | 228 bytes, 1360 samples | 90112 (91472) |
| v2 | longer last block | 638 bytes, 4000 samples | 90112 (94112) |
| v4 | last block as `t01` | 440 bytes, 2728 samples | 90112 (92840) |
| v3 | no noise at all (largest frame 651 bytes) | 228 bytes, 1360 samples | 81920 (83280) |

All four lose the last frame and log the error. Conclusions: it does not
depend on the last frame's size; it does not need large frames earlier in the
stream; and it does not need the parser to be holding several frames (v3 has
no call with `consumed == 0`: its last call is `len=228 cons=228 dec=0`).
What the failing streams share is the quiet tone. `t03` ends in a 293-byte
frame and works.

### 4. What is different about the quiet tone's frames?

On the computer, each file's frames were searched for the byte pair that
starts a frame header (0xFF, then 0xF8 or 0xF9) at any position other than the
frame's start:

| File | such pairs inside frames | in the last frame |
|---|---|---|
| `t10` | 842 | 2 (offsets 64, 95) |
| v1 / v3 | 101 / 100 | 1 (offset 204) |
| v2 | 104 | 4 |
| `t01`, `t03` | 0 | 0 |
| Task 3's loud-then-quiet stream (worked) | 4 | 0 |

The quiet tone's frames are full of 0xFF bytes (36 of the 228 bytes of v1's
last frame), most likely because nearly every sample is predicted exactly and
a zero leftover is coded as a single 1 bit.

Hypothesis: the last frame is lost when it contains a false header start.

### 5. Test the hypothesis with matched pairs

| Stream | last frame | false header start in last frame | frames (expected) |
|---|---|---|---|
| nclean (noise only, seed 1) | 6539 bytes | none | 10192 (10192) |
| nsync (noise only, seed 5) | 6544 bytes | offset 1620 | **8192** (10192), error |
| q160 (quiet tone, 160-sample tail) | 41 bytes | none | 24736 (24736) |
| q220 (quiet tone, 220-sample tail) | 50 bytes | offset 34 | 24796 (24796) |

Conclusions: the noise pair behaves as predicted, and shows this is not about
quiet audio: ordinary loud frames fail too if the pair of bytes is there. But
q220 has a false header start and still works, so the hypothesis as stated is
incomplete.

### 6. Read the library's parser

`esp_flac_dec_parse_frame`, `validate_flac_header` and a CRC-16 helper were
disassembled from `lib/esp32/libesp_audio_simple_dec.a` (object
`es_parse_default.c.obj`). In C terms, for data that starts with a frame
header:

```
first_sync = 0
if len > 18:
    for off in 2 .. len-17:
        if bytes at off look like a header start (0xFF, 0xF8/0xF9):
            ok = crc16_ok(buf, off)              // is buf[0..off) a complete frame?
            if first_sync == 0: first_sync = off // remembered even when ok is false
            if ok and header_valid(buf + off):   // full header parse and CRC-8
                frame_size = off; return OK      // the normal case
    if len < max_frame and not eos:
        frame_size = max_frame; return OK        // wait for more data
    if first_sync != 0:
        skipped_size = first_sync; frame_size = 0; return OK    // <-- the defect
elif len < max_frame and not eos:
    frame_size = max_frame; return OK
if crc16_ok(buf, len):
    frame_size = len; return OK                  // last frame: all that is left
log "Not found frame in max frame size"; skipped_size = len; frame_size = 0; return OK
```

At end of stream, with only the last frame left, no candidate can be a real
next frame. If the scan saw no candidate, the code falls through to the check
that the remainder is a whole frame, and delivers it. If it saw any candidate,
even one it had just rejected, it skips to that candidate instead and never
makes the whole-frame check. On the next call the data starts in the middle of
the frame, nothing verifies, and it logs the error and drops the rest.

This explains every observation, including q220: the scan stops 16 bytes
before the end, q220's false start is at offset 34 of 50, exactly 16 from the
end, so it is never seen.

### 7. A prediction from the disassembly

If that reading is right, moving the false start one byte further from the
end must break q220. q222 is the same stream with two more samples: last frame
51 bytes, false start still at offset 34, now 17 from the end.

| Stream | last frame | false start | 16 from the end? | frames (expected) |
|---|---|---|---|---|
| q220 | 50 bytes | offset 34 | yes: outside the scan | 24796 (24796) |
| q222 | 51 bytes | offset 34 | no: inside the scan | **24576** (24798), error |

Confirmed.

### 8. Correct that one decision, and take the correction away again

A parser of our own was registered for FLAC with
`esp_audio_simple_dec_register`. It calls the library's parser and changes the
answer only when the library says "skip" at end of stream while everything
that is left is one frame with a correct CRC-16. A flag switched the
correction off and on with the wrapper still registered, so nothing else
changed between passes. Decode-only, frames (expected):

| Stream | correction on | correction off | on again |
|---|---|---|---|
| q220 | 24796 (24796) | 24796 | 24796 |
| q222 | 24798 (24798) | 24576, error | 24798 |
| nclean | 10192 (10192) | 10192 | 10192 |
| nsync | 10192 (10192) | 8192, error | 10192 |
| v1 | 91472 (91472) | 90112, error | 91472 |
| v3 | 83280 (83280) | 81920, error | 83280 |
| `t10` (card) | 882000 (882000) | 880640, error | 882000 |

The correction fired once per failing stream, and each time the library's
skip was exactly the offset of the first false header start found on the
computer (34, 1620, 204, 204 and 64). No error line appeared with the
correction on. Decode time is unchanged (`t10`: 3595 ms before, 3605 ms with
the wrapper).

(A first attempt at the "off" pass unregistered the wrapper instead. That
also removes the FLAC decoder itself from the library's registry, so nothing
decoded; the flag replaced it.)

## Root cause

In `esp_flac_dec_parse_frame` (prebuilt library): at end of stream, when a
byte pair that looks like a frame header start lies inside the final frame
(more than 16 bytes before its end), the parser skips to that pair instead of
checking whether the remaining data is a complete frame. The final frame is
discarded and the library logs `Not found frame in max frame size N`.

Evidence: the trace (experiment 1), the matched pairs (5), the disassembled
logic (6), the boundary prediction (7), and the correction switched on, off
and on (8).

Not involved: our decode loop, the `eos` flag, the read chunk size, where the
last read falls, the size of the last frame, large frames earlier in the
stream, or the parser's handing out of cached frames.

## Fix

`main/player.c` only. The library offers a documented way to put a different
parser in front of a built-in decoder (`esp_audio_simple_dec_register`, shown
in its `simple_decoder_cust.c` test). `player_init` registers a FLAC entry
with the library's own decoder functions and this parser:

```c
static esp_es_parse_err_t flac_parse_frame(esp_es_parse_raw_t *in,
                                           esp_es_parse_frame_info_t *info) {
  esp_es_parse_err_t err = esp_flac_dec_parse_frame(in, info);
  if (err == ESP_ES_PARSE_ERR_OK && in->eos && !in->bos &&
      info->frame_size == 0 && info->skipped_size > 0 &&
      flac_frame_crc_ok(in->buffer, in->len)) {
    info->frame_size = in->len;
    info->skipped_size = 0;
  }
  return err;
}
```

Why this is correct and not a guess: the bytes delivered are the file's own,
unchanged, and they are delivered only when their CRC-16 proves they are one
complete FLAC frame, which is the same test the library applies in its own
"last frame" path. Nothing is invented or padded. For any other answer from
the library, the wrapper passes it through untouched. The CRC runs once per
track, over one frame; the per-call cost elsewhere is a few comparisons. The
decode loop itself is unchanged.

Limits of the workaround:
- `esp_flac_dec_parse_frame` is exported by the library but declared in none
  of its headers, so `player.c` declares it. If a later library version
  renames it, the build fails at link time (it does not fail silently).
- It relies on the meaning of `frame_size`, `skipped_size`, `eos` and `bos`
  documented in `esp_es_parse_types.h`.
- When the library is fixed upstream the wrapper becomes a no-op and can be
  removed.

## Acceptance (real, audible path, fixed firmware, volume level 10)

| File | frames played | expected | `AUD_Dec_Parse` error lines |
|---|---|---|---|
| `t10_loud_quiet_44k_16_st.flac` | 882000 | 882000 | 0 |
| `t01_44k_16_st.flac` | 441000 | 441000 | 0 |
| `t02_96k_24_st.flac` | 960000 | 960000 | 0 |
| `t03_48k_16_mono.flac` | 480000 | 480000 | 0 |
| `t06_tone_30s.flac` | 1323000 | 1323000 | 0 |
| `music/16 - Kanye West - Hey Mama.flac` | 13453440 | 13453440 | 0 |

The five test files were played in one boot by temporary code that chose the
files; the music track was played from start to end by the committed
firmware. In the same boot as the test files, the deliberately bad files
behaved as before: `t09_truncated` 258048 of 441000 with the parser error
where the file is cut off (correct: that frame really is incomplete, its CRC
fails, and the wrapper leaves the library's answer alone), `t08_not_audio`
rejected, `t07_empty` zero frames.

The library now prints one extra line at start-up, at INFO level:
`AUD_CODEC_REG: Overwrite codec library for 1128352838` (the number is
"FLAC"). It is the library acknowledging the registration.

## Open questions

- **Report upstream.** This is a defect in `esp_audio_codec` 2.6.2 and affects
  every user of the simple decoder for FLAC. It should be reported to
  Espressif (esp-adf issues). Not done here.
- **The parser's handling of the last input byte.** In every trace the parser
  left some input unconsumed (often exactly one byte) until all cached frames
  had been handed out, which is what keeps our loop calling. Whether that is
  by design was not established. If it ever consumed all input while still
  holding frames, the loop would stop with frames undelivered. Not seen in
  `t10` or in any of the synthetic streams.
- **A last frame of 16 bytes or less.** The library's scan stops 16 bytes
  before the end of the data, so it cannot see a final frame that short as
  separate from the one before; it hands both to the decoder as one. In Task
  3 the decoder was seen to decode several frames from one input, so this
  probably works, but no test stream here ends that way (digital silence at
  the end of a track would).
- **Mid-stream.** The same "skip to the first candidate" branch runs when the
  parser has a full `max_frame` of data and finds no valid next frame. That is
  the right thing to do for damaged data and was not changed.
- How often real tracks hit this was estimated (chance of the byte pair in
  the final frame), not measured over a music collection.
