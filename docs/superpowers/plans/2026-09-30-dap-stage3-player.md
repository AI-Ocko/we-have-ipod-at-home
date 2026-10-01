# DAP Stage 3 (Player) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the blocking "play one file" loop with a real player: decode and output on their own tasks with a PSRAM buffer between them, driven by commands and reporting events, with volume, pause, MP3, true 24-bit output and seeking. First, find and fix the inherited defect that skips about three seconds of the test track.

**Architecture:** A decoder task reads the file, decodes, converts every format to 32-bit stereo frames and fills a 768 KB stream buffer in PSRAM. An output task drains it into I2S, applying volume and fades. The UI task sends commands (`play`, `pause`, `seek`...) and receives events (`STARTED`, `POSITION`, `ENDED`, `ERROR`). Format conversion, the volume curve and FLAC header parsing are hardware-free and tested on the host. A serial debug console lets agents drive the player and inject key presses, so hardware behaviour can be checked from the log without anyone touching the keypad.

**Tech Stack:** ESP-IDF v6.0.2 (C), `espressif/esp_audio_codec` 2.6.2 simple decoder (FLAC, MP3), FreeRTOS tasks, queues and stream buffers, I2S std driver, Unity host tests under AddressSanitizer, ffmpeg and flac on the host for test audio.

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read section 2 "Power", section 3 including "Performance rules", all of section 4, section 11, and stage 3 of section 14).

**How this plan differs from stages 1 and 2:** the project owner wants agents to do the coding. This plan therefore fixes the interfaces, the required behaviour, the exact test cases and the acceptance checks, and leaves implementation bodies to the implementer. Header contents and log formats given here are binding; function bodies are yours to write, test-first.

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- **Power quirk (spec section 2):** on the computer's USB port, a cold plug-in leaves the SD card dead until the user re-inserts it. Resets and flashing are fine afterwards. If a capture shows `sdmmc_init_ocr ... returned 0x107` / `Failed to mount SD card (ESP_ERR_TIMEOUT)`, retry once; if it fails twice, STOP and report BLOCKED: only the user can re-insert the card.
- Pins are defined only in `main/pins.h`. DAC: BCK=26, LRCK=25, DOUT=22. Never drive GPIO 0 or 12.
- **Output format (spec section 4):** everything is converted to 32-bit stereo frames, left-justified, at the file's own sample rate. I2S: Philips format, 32-bit data in 32-bit slots, no MCLK, `dma_desc_num = 16`, `dma_frame_num = 500`. Sample rates 8000 to 96000 Hz are accepted; anything else is "unsupported".
- **Ring buffer:** 768 KB (786432 bytes) in PSRAM between decoder and output.
- **Decoder input buffer:** 32 KB, allocated `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` (the SDMMC driver cannot read into PSRAM). Never move or overwrite input bytes while the decoder's `raw.len` is non-zero: it keeps pointers into that buffer.
- Register both `esp_audio_dec_register_default()` and `esp_audio_simple_dec_register_default()`, once.
- **Tasks (spec section 3):** output task priority 20, core 1, 4 KB stack. Decoder task priority 10, core 1, 16 KB stack (the FLAC decoder overflows smaller stacks). Keypad task priority 6, core 0. UI task priority 5, core 0, 8 KB. Debug console task priority 4, core 0.
- **Volume:** 21 levels, 0 to 20; level 0 is mute, level 20 is unity, each step is 2 dB. Default 10. Applied in the output task.
- **Fades:** every start and resume fades in over 400 frames; every pause, stop and seek fades out over 400 frames. After a fade-out with nothing more to play: write silence, wait 50 ms, disable the I2S channel.
- **Performance rules (spec section 3):** in the decode and output loops, integer arithmetic only, no floating point, no division per sample, no function call per sample, no heap allocation, no logging, no locks held across blocking calls. Data moves in blocks. Tasks block on queues, stream buffers or timers; no busy-waiting.
- **Failure handling (spec section 11):** nothing that can fail at run time because of the card or a bad file may use `ESP_ERROR_CHECK`, abort or restart. Such failures become an `ERROR` event. `ESP_ERROR_CHECK` is acceptable only for one-time start-up of drivers and tasks.
- `dap_core` contains pure C using only the C standard library. No ESP-IDF or FreeRTOS includes.
- Host tests build with `-Wall -Wextra -Werror` under AddressSanitizer and UBSan, and must not allocate from the heap.
- `MINIMAL_BUILD` stays on; each component lists what it uses in `REQUIRES`/`PRIV_REQUIRES`.
- Match the existing code style: 2-space indent, `static const char *TAG`, `ESP_LOGx` logging, comments that say *why*.
- No compiler warnings from `main/`, `components/dap_hw/` or `components/dap_core/`.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main`. Tasks run in order; they share one board and one build directory.
- **The music track** (amended after Task 3): the original test track `Clay Pigeons.flac` was found to be damaged on the card and its source is gone. From Task 5 on, "the music track" means `/sdcard/music/16 - Kanye West - Hey Mama.flac` (44.1 kHz, 16-bit stereo, about 305 s, largest frame 13736 bytes), which is also the first FLAC the firmware finds at start-up. Its exact frame count comes from its own header. A full real-time play takes over five minutes: run such captures in the background.
- The card's `/test/` folder also has `t10_loud_quiet_44k_16_st.flac` (882000 frames): about 5 s of noise at -20 dBFS, then 15 s of a very quiet tone. Its small frames after large ones exercise the decode-loop fix from Task 3; include it wherever a task plays the FLAC test files, expecting `frames=882000`.
- Agents cannot hear the output, see the LCD or press keys. Anything only a person can judge goes in the hardware check at the end; do not claim it.

### Binding log formats

Checks in this plan grep for these exact strings (tag `player`, level INFO unless noted):

- `STARTED path=<path> rate=<hz> bits=<n> ch=<n> duration_ms=<n>`
- `POSITION ms=<n>`
- `ENDED frames=<played> expected=<total or 0 if unknown>`
- `ERROR reason=<not_found|unsupported|decode|io|no_mem>` (level ERROR)
- Debug console replies (tag `dbg`): `ok <command>` or `err <command>: <why>`

## Review Focus

1. **Audio skipped silently.** The inherited loop drops data without any error the user would notice. Pinned in Task 3: for every FLAC, frames played must equal the header's total, exactly, and that check stays in the firmware (`ENDED frames= expected=`) for every later stage.
2. **The buffer runs dry** (slow card, long seek). Output must go silent, not replay stale data as a buzz, and must recover when data returns. Pinned in Task 6 by I2S `auto_clear` and a test that stalls the decoder.
3. **Commands arriving in any state**: `pause` while paused, `resume` while playing, `stop` while idle, `play` while playing, `seek` while paused or idle, volume out of range. None may crash, hang or leave the state machine stuck. Pinned in Task 6 by a scripted sequence that sends each one in each state.
4. **A file that vanishes or is corrupt**: `play` on a missing path, a zero-length file, a text file renamed `.flac`, a file truncated mid-frame. Each must produce one `ERROR` or a clean `ENDED`, and the player must accept the next `play`. Pinned in Task 6 with files created in Task 1.
5. **Sample-rate and width changes between tracks** (44.1 kHz 16-bit, then 96 kHz 24-bit, then 48 kHz mono). Pinned in Task 6 by playing them back to back.
6. **Full-scale samples and extreme volume.** Conversion and gain must not overflow at the maximum negative sample, and level 0 must be exact silence. Pinned in Task 4 by named test cases.
7. **Seeking to the very start, the very end, and past either.** Pinned in Task 8.
8. **Keys pressed during playback** must not disturb audio. Cannot be automated for sound; pinned in Task 6 by injecting 200 key events through the console during a playback whose frame count must still match, and in the user's hardware check by ear.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `tools/make_test_audio.sh` | Generate the test audio set and host fixtures | 1 |
| `tools/serial_script.py` | Send lines to the board and wait for expected log lines | 1 |
| `test/host/fixtures/` | Tiny audio files for host tests (committed) | 1 |
| `components/dap_core/flac_header.c/.h` | Parse a FLAC file's header | 2 |
| `main/player.c` | (Task 3) fix to the existing blocking loop | 3 |
| `components/dap_core/pcm.c/.h` | Convert samples to 32-bit stereo; volume table; gain ramps | 4 |
| `components/dap_hw/audio_out.c/.h` | I2S output, now fixed at 32-bit stereo | 5 |
| `main/player.c/.h` | Decoder task, output task, commands and events | 6, 7, 8 |
| `main/debug_console.c/.h` | Serial commands for driving the player and injecting keys | 6 |
| `main/app_main.c` | Start-up, temporary player screen and key bindings, card retry | 6, 9 |

---

### Task 1: Test audio, host fixtures and the serial script tool

**Files:**
- Create: `tools/make_test_audio.sh`, `tools/serial_script.py`, `test/host/fixtures/*` (generated, committed)
- Modify: `.gitignore` (ignore `test-audio/`)

**Interfaces:**
- Produces `test-audio/` (not committed; the user copies its contents to the card's `/test/` folder):

| File | Content | Exact length |
|---|---|---|
| `t01_44k_16_st.flac` | 440 Hz left, 660 Hz right, 10 s | 441000 frames |
| `t02_96k_24_st.flac` | same tones, 96 kHz 24-bit, 10 s | 960000 frames |
| `t03_48k_16_mono.flac` | 440 Hz mono, 10 s | 480000 frames |
| `t04_44k_cbr192_st.mp3` | 440/660 Hz, CBR 192 kbps, 10 s | about 441000 |
| `t05_48k_vbr_mono.mp3` | 440 Hz mono, VBR, 10 s | about 480000 |
| `t06_tone_30s.flac` | steady 440 Hz both channels at -12 dBFS, 44.1 kHz 16-bit, 30 s | 1323000 frames |
| `t07_empty.flac` | zero bytes | — |
| `t08_not_audio.flac` | 4 KB of text | — |
| `t09_truncated.flac` | first 60% of the bytes of `t01` | — |

- Produces `test/host/fixtures/` (committed; each under 64 KB): `f_44k_16_st.flac` (0.1 s, 4410 frames), `f_96k_24_st.flac` (9600 frames), `f_48k_16_mono.flac` (4800 frames), `f_with_picture.flac` (as the first, plus an embedded picture block of at least 20 KB), `f_with_id3.flac` (as the first, with a 1 KB ID3v2 tag prepended), `f_not_flac.bin` (64 bytes of text), `f_truncated.flac` (first 20 bytes of the first).
- Produces `tools/serial_script.py <port> [--no-reset] <step>...` where each step is `expect:<text>[:<timeout_s>]` (default 10), `send:<line>` or `sleep:<seconds>`. It echoes everything received to stdout, exits 0 if every `expect` matched in order, 1 on the first timeout (printing which step). With `--no-reset` it does not reset the board on open.

Requirements:
- The script must be deterministic (fixed tones, no dither or random content) and safe to re-run. Use ffmpeg's `sine` source; for the 24-bit file make sure the result really is 24 bits per sample (verify with `metaflac --show-bps`).
- Verify every FLAC's frame count with `metaflac --show-total-samples` and put the numbers in your report. If a count differs from the table, fix the script, not the table.
- `serial_script.py` follows the reset logic of `tools/serial_capture.py` (read it first).

- [ ] **Step 1:** Write `tools/make_test_audio.sh`; run it; verify counts, bit depths and fixture sizes.
- [ ] **Step 2:** Write `tools/serial_script.py`. Check it against the board as it is now: `expect:Playing:15` then `expect:FLAC::5` must exit 0; `expect:no such text:3` must exit 1 and name the step.
- [ ] **Step 3:** Add `test-audio/` to `.gitignore`. Commit the two tools and the fixtures: `Add test audio generator, host fixtures and serial script tool`.

**Then a pause for the user:** copy the contents of `test-audio/` into a folder named `test` on the SD card, put the card back, and (on the computer's USB) re-seat it after plugging in. Later tasks expect `/sdcard/test/t01_44k_16_st.flac` and so on.

---

### Task 2: `flac_header`

**Files:**
- Create: `components/dap_core/flac_header.h`, `components/dap_core/flac_header.c`
- Test: `test/host/test_flac_header.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Consumes: the fixtures from Task 1. Give the tests the fixture directory through a compile definition set in CMake (for example `FIXTURE_DIR`).
- Produces (`flac_header.h`, binding):

```c
#pragma once

#include <stdint.h>
#include <stdio.h>

typedef struct {
  uint32_t sample_rate;     // Hz
  uint8_t channels;         // 1..8
  uint8_t bits_per_sample;  // 4..32
  uint64_t total_frames;    // samples per channel; 0 if the file does not say
  uint32_t max_frame_bytes; // largest encoded frame; 0 if the file does not say
  long audio_start;         // byte offset of the first audio frame
} flac_header_t;

typedef enum {
  FLAC_HEADER_OK,
  FLAC_HEADER_NOT_FLAC,  // no "fLaC" marker where one should be
  FLAC_HEADER_TRUNCATED, // the file ends inside the header
} flac_header_result_t;

// Read a FLAC file's header from the start of `f`. Skips a leading ID3v2 tag
// if there is one. Leaves the file position unspecified.
flac_header_result_t flac_header_read(FILE *f, flac_header_t *out);

// Track length in milliseconds, or 0 if total_frames or sample_rate is 0.
uint32_t flac_header_duration_ms(const flac_header_t *h);
```

Format facts: the file starts with `fLaC`, then metadata blocks, each with a 4-byte header (bit 7 of byte 0 = last block, low 7 bits = type, then a 24-bit big-endian length). The first block is STREAMINFO (type 0, 34 bytes): min block size (16 bits), max block size (16), min frame size (24), max frame size (24), sample rate (20), channels minus 1 (3), bits per sample minus 1 (5), total samples (36), MD5 (128), all big-endian and bit-packed. An ID3v2 tag is `ID3`, two version bytes, one flags byte, then a 28-bit size in four 7-bit bytes; the tag is 10 bytes plus that size.

Performance: read only header bytes. Skip block bodies with `fseek`, never by reading them; a 5 MB embedded picture must cost one seek.

Required test cases (write them first; names are binding):

| Test | Expectation |
|---|---|
| `test_reads_44k_16_stereo` | OK; 44100, 2, 16, 4410 frames; `max_frame_bytes` > 0; `audio_start` > 42 |
| `test_reads_96k_24_stereo` | OK; 96000, 2, 24, 9600 |
| `test_reads_48k_16_mono` | OK; 48000, 1, 16, 4800 |
| `test_audio_start_is_past_a_picture_block` | OK; same stream values as the first; `audio_start` >= 20000 |
| `test_audio_start_points_at_a_frame_sync` | for each valid fixture, the two bytes at `audio_start` are `0xFF` and `0xF8` or `0xF9` |
| `test_skips_a_leading_id3_tag` | OK; same stream values as the first; `audio_start` >= 1034 |
| `test_rejects_a_file_that_is_not_flac` | `FLAC_HEADER_NOT_FLAC` |
| `test_reports_a_truncated_header` | `FLAC_HEADER_TRUNCATED` |
| `test_reports_an_empty_file` | on a zero-length temporary file: `FLAC_HEADER_TRUNCATED` |
| `test_duration` | 441000 frames at 44100 gives 10000 ms; 0 frames gives 0; 0 rate gives 0; 2^36 - 1 frames at 8000 Hz does not overflow |

- [ ] **Step 1:** Header, stub, tests, CMake wiring. Run: tests fail for the right reason.
- [ ] **Step 2:** Implement. Run: all pass, nothing on stderr.
- [ ] **Step 3:** Add to `components/dap_core/CMakeLists.txt`; firmware build succeeds without warnings.
- [ ] **Step 4:** Commit: `Add flac_header: parse a FLAC file's header`.

---

### Task 3: Find and fix the skipped audio

**Files:**
- Modify: `main/player.c` (the existing blocking loop), `main/CMakeLists.txt` if needed
- Create: `docs/superpowers/plans/stage3-skip-investigation.md` (your findings)
- Temporary instrumentation is allowed while investigating and must not be committed.

**Interfaces:**
- Consumes: `flac_header_read` (Task 2); the test files on the card (Task 1).
- Produces: a blocking `player_play_file` that plays every frame of a FLAC file, and logs `ENDED frames=<played> expected=<total>` (tag `player`) at the end, replacing the old `Finished: N blocks decoded` line.

**The defect, as measured:** playing `/sdcard/Clay Pigeons.flac` (44.1 kHz, 16-bit stereo, reported duration 255440 ms), the decoder library logs `E AUD_Dec_Parse: Not found frame in max frame size 10507` about 4 s in, and 2718 blocks are decoded where the length implies about 2751. Roughly 3 seconds of audio are missing. Baseline log: `docs/superpowers/plans/stage1-baseline.log`.

**This is a debugging task. Use the superpowers:systematic-debugging skill. No fix before the root cause is demonstrated.**

One suspect, unverified: in the decode loop, `if (raw.consumed == 0) { break; }` abandons the rest of the 32 KB chunk whenever the decoder returns without consuming input. Espressif's reference loop has no such break. Do not assume this is it. Other candidates include the chunk size against the parser's needs, the `eos` flag, and how `ESP_AUDIO_ERR_BUFF_NOT_ENOUGH` retries interact with consumed input.

Method requirements:
- Build a decode-only path (no I2S writes) so a whole file decodes in seconds, and count PCM frames produced against `flac_header_t.total_frames`.
- Instrument the loop: for each `process` call around the failure, log the file offset, `raw.len` before, `raw.consumed`, `decoded_size` and the return code. Find the first call where the stream goes wrong.
- Change one thing at a time and record each experiment and its result in the investigation document: what you changed, the frame count you got, what that proves.
- Confirm the root cause by showing the count become exact when, and only when, that one thing is corrected.

Acceptance (all with the real, audible playback path, not only decode-only):
- `Clay Pigeons.flac`: `ENDED frames=X expected=X` with X equal to the header's total, and zero `AUD_Dec_Parse` error lines.
- `t01`, `t02`, `t03`, `t06`: frames equal expected (441000, 960000, 480000, 1323000).
- Report, from the decode-only path at `-O2`/240 MHz: decode speed as a multiple of real time for `Clay Pigeons.flac` and for `t02` (96 kHz 24-bit).
- Report how the decoder delivers 24-bit audio for `t02`: bytes per sample and justification (compare `decoded_size` with frames, and print the first few sample values of the 440 Hz tone, whose peak is known). Task 4 needs this.

If the root cause is inside the decoder library and cannot be corrected from our side, STOP and report BLOCKED with the evidence; do not paper over it.

- [ ] **Step 1:** Reproduce: confirm the shortfall and the error line on the current code.
- [ ] **Step 2:** Investigate as above; write the investigation document as you go.
- [ ] **Step 3:** Fix the root cause only. Remove temporary instrumentation. Keep the `ENDED frames= expected=` log.
- [ ] **Step 4:** Run the acceptance checks; put the log lines in your report.
- [ ] **Step 5:** Commit: `Fix skipped audio in the FLAC decode loop` with the root cause in the message body, plus the investigation document.

---

### Task 4: `pcm`

**Files:**
- Create: `components/dap_core/pcm.h`, `components/dap_core/pcm.c`
- Test: `test/host/test_pcm.c`
- Modify: `test/host/CMakeLists.txt`, `components/dap_core/CMakeLists.txt`

**Interfaces:**
- Produces (`pcm.h`, binding):

```c
#pragma once

#include <stddef.h>
#include <stdint.h>

// How the decoder hands over its samples. All are signed, little-endian,
// interleaved by channel.
typedef enum {
  PCM_FMT_S16,        // 2 bytes per sample
  PCM_FMT_S24_PACKED, // 3 bytes per sample
  PCM_FMT_S24_IN_S32, // 4 bytes per sample, value in the low 24 bits
  PCM_FMT_S32,        // 4 bytes per sample, full scale
} pcm_format_t;

size_t pcm_bytes_per_sample(pcm_format_t format);

// Convert to interleaved 32-bit stereo, left-justified (16-bit input is
// shifted up 16 bits, 24-bit input 8 bits). Mono input is copied to both
// channels. `channels` is 1 or 2. Writes at most `out_frames_max` frames and
// returns how many it wrote. Input bytes beyond the last whole frame are
// ignored.
size_t pcm_to_s32_stereo(const uint8_t *in, size_t in_bytes,
                         pcm_format_t format, int channels, int32_t *out,
                         size_t out_frames_max);

#define PCM_VOLUME_MAX 20
#define PCM_GAIN_UNITY 32768

// Gain for a volume level as a Q15 multiplier. Level 20 is unity, each step
// down is 2 dB, level 0 is silence. Levels outside 0..20 are clamped.
int32_t pcm_volume_gain(int level);

// Scale `frames` stereo frames in place. The gain moves in a straight line
// from `gain_from` at the first frame towards `gain_to`, reaching it at the
// frame after the last one, so consecutive blocks join without a step. Equal
// gains give a constant gain. Both channels of a frame get the same gain.
void pcm_apply_gain(int32_t *samples, size_t frames, int32_t gain_from,
                    int32_t gain_to);
```

Volume table (level 0 to 20, binding): `0, 413, 519, 654, 823, 1036, 1305, 1642, 2068, 2603, 3277, 4125, 5193, 6538, 8231, 10362, 13045, 16423, 20675, 26029, 32768`

Performance (this runs for every sample played): no floating point; no division inside any per-sample loop (the ramp advances by a fixed-point step computed once per block); a fast path that returns immediately when both gains are unity; a fast path that writes zeros when both gains are 0; one tight loop per input format in the converter, not a per-sample `switch`. Use a 64-bit intermediate for the multiply so full-scale input cannot overflow.

Required test cases (write them first; names are binding):

| Test | Expectation |
|---|---|
| `test_bytes_per_sample` | 2, 3, 4, 4 |
| `test_s16_stereo` | `{0x0001, 0xFFFF}` gives `{0x00010000, (int32_t)0xFFFF0000}` |
| `test_s16_mono_is_copied_to_both_channels` | one sample `0x1234` gives `{0x12340000, 0x12340000}` |
| `test_s16_extremes` | `-32768` gives `INT32_MIN`; `32767` gives `0x7FFF0000` |
| `test_s24_packed` | bytes `56 34 12` give `0x12345600`; bytes `00 00 80` give `INT32_MIN` |
| `test_s24_in_s32` | `0x00123456` gives `0x12345600`; `(int32_t)0xFF800000` gives `INT32_MIN` |
| `test_s32_passes_through` | values unchanged, including `INT32_MIN` and `INT32_MAX` |
| `test_partial_trailing_frame_is_ignored` | 7 bytes of S16 stereo give 1 frame |
| `test_output_limit_is_respected` | 10 frames in, limit 4: returns 4 and the fifth output frame is untouched |
| `test_zero_input` | 0 bytes returns 0 |
| `test_volume_table` | each of the 21 values equals the table above |
| `test_volume_is_clamped` | level -5 gives 0; level 99 gives 32768 |
| `test_unity_gain_changes_nothing` | including `INT32_MIN` and `INT32_MAX` |
| `test_zero_gain_gives_exact_silence` | every output sample is 0 |
| `test_half_gain` | gain 16384 on `0x40000000` gives `0x20000000`; on `INT32_MIN` gives `-0x40000000` |
| `test_ramp_starts_at_from_and_approaches_to` | 4 frames of `0x40000000`, from 0 to 32768: frame gains are 0, 1/4, 1/2, 3/4 of unity, within 1 part in 4096 |
| `test_ramps_join_without_a_step` | two consecutive 400-frame blocks, 0 to 16384 then 16384 to 32768, on a constant input: the output never decreases and the largest jump between neighbouring frames is no more than twice the smallest non-zero jump |
| `test_both_channels_get_the_same_gain` | within each frame, left equals right for equal inputs |
| `test_full_scale_does_not_overflow_under_ramp` | ramp 32768 to 32768 and 32760 to 32768 on `INT32_MIN`/`INT32_MAX`: no wrap-around (sign never flips) |

- [ ] **Step 1:** Header, stub, tests, CMake wiring. Run: fail for the right reason.
- [ ] **Step 2:** Implement. Run: all pass, nothing on stderr.
- [ ] **Step 3:** Add to the firmware build; no warnings.
- [ ] **Step 4:** Commit: `Add pcm: 32-bit stereo conversion, volume table and gain ramps`.

---

### Task 5: 32-bit output

**Files:**
- Modify: `components/dap_hw/audio_out.h`, `components/dap_hw/audio_out.c`, `main/player.c`

**Interfaces:**
- Consumes: `pcm_to_s32_stereo`, `pcm_format_t` (Task 4); the decoder's 24-bit layout found in Task 3 (`docs/superpowers/plans/stage3-skip-investigation.md`).
- Produces (`audio_out.h`, replacing the current start/write):

```c
// Start output at `sample_rate` (8000..96000). The format is always 32-bit
// stereo. Returns ESP_ERR_NOT_SUPPORTED for a rate outside the range and
// ESP_ERR_INVALID_STATE if output is already running.
esp_err_t audio_out_start(uint32_t sample_rate);

// Queue `frames` stereo frames (2 * frames int32_t values). Blocks until all
// are queued.
esp_err_t audio_out_write(const int32_t *samples, size_t frames);

// Unchanged: audio_out_init(pins), audio_out_stop().
```

Requirements:
- 32-bit data in 32-bit slots, `dma_desc_num = 16`, `dma_frame_num = 500`, and I2S `auto_clear` enabled so an underrun sends silence instead of replaying stale buffers. Update the comment that explains the DMA sizing: a descriptor holds at most 4092 bytes and a frame is now 8 bytes.
- Keep "create the channel once, retune on later starts".
- The blocking player converts each decoded block with `pcm_to_s32_stereo` into a PSRAM buffer allocated once per track and writes that. No per-block allocation.
- `ENDED frames= expected=` must still be exact.

Board checks: the music track, `t01`, `t02` (24-bit, 96 kHz), `t03` (mono, 48 kHz) each end with frames equal to expected; no errors; wall-clock duration of each within 1% of its length. Selecting which file plays is up to you for this task (a temporary constant is fine, do not commit it); the committed firmware still plays the first FLAC it finds.

Sound quality on 32-bit frames can only be judged by ear; it is in the user's hardware check. Do not claim it.

- [ ] **Step 1:** Change `audio_out`; adapt the blocking player; build without warnings.
- [ ] **Step 2:** Run the board checks; record the lines.
- [ ] **Step 3:** Commit: `Output 32-bit stereo frames for every source`.

---

### Task 6: Player tasks, commands, events, debug console

**Files:**
- Rewrite: `main/player.h`, `main/player.c`
- Create: `main/debug_console.h`, `main/debug_console.c`
- Modify: `main/app_main.c`, `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `flac_header` (2), `pcm` (4), `audio_out` (5), `keys`/`screen`/`lcd`/`keypad` (stage 2), `translit` (stage 1).
- Produces (`player.h`, binding):

```c
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

typedef enum {
  PLAYER_EVENT_STARTED,  // a track began: duration_ms, sample_rate, bits, channels
  PLAYER_EVENT_POSITION, // every 500 ms of audio played: position_ms
  PLAYER_EVENT_ENDED,    // the track played to its end
  PLAYER_EVENT_ERROR,    // the track could not be played: error
} player_event_type_t;

typedef enum {
  PLAYER_ERROR_NOT_FOUND,
  PLAYER_ERROR_UNSUPPORTED,
  PLAYER_ERROR_DECODE,
  PLAYER_ERROR_IO,
  PLAYER_ERROR_NO_MEM,
} player_error_t;

typedef struct {
  player_event_type_t type;
  uint32_t position_ms;
  uint32_t duration_ms; // 0 if unknown
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;
  player_error_t error;
} player_event_t;

// Create the decoder and output tasks and the buffer between them. Events are
// posted to `events`; if that queue is full the event is dropped. Call once.
esp_err_t player_start(QueueHandle_t events);

// Each of these queues a command and returns at once. They are safe to call
// in any state and from any task.
esp_err_t player_play(const char *path, uint32_t start_ms); // replaces whatever is playing
esp_err_t player_pause(void);
esp_err_t player_resume(void);
esp_err_t player_stop(void);
esp_err_t player_seek_rel(int32_t delta_ms); // implemented in Task 8; until then, ignored
esp_err_t player_set_volume(int level);      // 0..20, clamped
```

Required behaviour:
- **States:** idle, playing, paused. `play` from any state fades out what is playing (if anything), discards buffered audio, and starts the new track. `pause` while playing fades out and holds; anything else ignores it. `resume` while paused fades in from where it stopped, with no audio lost or repeated; anything else ignores it. `stop` fades out, discards buffered audio and goes idle, from any state. `set_volume` takes effect within 50 ms in any state and moves the gain smoothly (use the ramp) so a volume change never clicks.
- **Buffer:** a FreeRTOS stream buffer with 786432 bytes of storage allocated from PSRAM. The decoder task fills it with 32-bit stereo frames; the output task drains it. The decoder must keep servicing commands while the buffer is full (bounded waits, not an unbounded block).
- **End of track:** `ENDED` is posted when the last buffered frame has been handed to I2S, not when the decoder reaches the end of the file. `ENDED frames= expected=` is logged then.
- **Position:** counted from frames handed to I2S. `POSITION` every 500 ms of audio. Paused time does not count.
- **Errors:** missing file, unsupported format or rate, decode failure, read failure and allocation failure each post one `ERROR` and return the player to idle, ready for the next `play`. A decode failure part-way through a track plays out what was already buffered, then posts `ERROR`.
- **Underrun:** if the buffer runs dry while playing (slow card), output goes silent and continues when data returns. Log one warning per underrun, not one per block.
- **Rate changes:** a new track with a different sample rate reconfigures I2S after the previous track's audio has finished or been discarded.
- **Concurrency:** state shared between the two tasks must be either owned by one task and passed by message, or a single word read and written atomically. Document in a comment at the top of `player.c` which task owns what.
- FLAC only in this task (`.flac`, case-insensitive). Any other extension is `unsupported` until Task 7.

**Debug console** (`debug_console_start(QueueHandle_t key_events)`): a task that reads lines from the serial console and acts on them. Commands: `play <path> [start_ms]`, `pause`, `resume`, `stop`, `vol <n>`, `seek <signed ms>`, `key <char>` (posts a press then a release for that key to the key queue), `long <char>` (press, long, release), `stat` (logs state, position, volume, buffer fill in bytes, and the buffer's lowest fill since the track started). Each replies with the binding `dbg` log line. Paths may contain spaces: everything after `play ` up to an optional trailing number is the path; define and document the rule you choose.

**Temporary screen and keys** (replaced in stage 5): at start-up the first FLAC found plays, as now. Top row: the file's name without folder or extension, through `translit`, cut to 16 characters. Bottom row: `>` while playing or `=` while paused, elapsed and total time as `m:ss/m:ss`, and `V` plus the volume level right-aligned (for example `> 0:12/4:15  V10`). Keys: `D` toggles pause; `A` and `B` raise and lower the volume by one level, on press and on repeat. The UI task owns the LCD and reacts to both key events and player events; it must wait on both without polling (a queue set, or one queue carrying both kinds of message).

**Performance evidence required in your report** (from `stat` and the log, on the real board): buffer fill while playing the music track for 60 s (typical and lowest); the decoder task's and output task's stack high-water marks after a full track; confirmation by reading your own diff that the two loops contain no allocation, logging, division per sample or floating point.

Board checks, all driven by `tools/serial_script.py` and judged from the log:
1. `play /sdcard/test/t01_44k_16_st.flac`: `STARTED ... rate=44100 bits=16 ch=2 duration_ms=10000`, at least 18 `POSITION` lines with strictly increasing `ms`, `ENDED frames=441000 expected=441000`.
2. Back to back without resetting: `t02` (96000, 24, 2, `frames=960000`), `t03` (48000, 16, 1, `frames=480000`), then `t01` again.
3. Pause: play `t06`, at about 5 s `pause`, wait 4 s (no `POSITION` lines in that time), `resume`, play to the end: `ENDED frames=1323000 expected=1323000`.
4. Every command in every state (Review Focus 3): script each of `pause`, `resume`, `stop`, `vol 0`, `vol 20`, `vol 99`, `vol -3`, `seek 1000`, and a second `play`, sent while idle, while playing and while paused. No crash, no hang, and a final `play t01` still ends with the exact frame count.
5. Bad files (Review Focus 4): `play /sdcard/test/nope.flac` gives `ERROR reason=not_found`; `t07_empty.flac` and `t08_not_audio.flac` give one `ERROR` each; `t09_truncated.flac` gives `ENDED` or one `ERROR` after playing part of the file; `play /sdcard/test/t04_44k_cbr192_st.mp3` gives `ERROR reason=unsupported`. After each, `play t01` works.
6. Keys during playback (Review Focus 8): while the music track plays, send 200 `key 5` commands over 30 s, then let the track finish: `ENDED frames=X expected=X` and no underrun warning.
7. Full track: the music track from start to end with nothing else happening: exact frame count, no `AUD_Dec_Parse` errors, no underrun warnings.

- [ ] **Step 1:** Write `player.h` as above and design notes at the top of `player.c` (states, ownership, message flow). Build.
- [ ] **Step 2:** Implement the player tasks. Implement the debug console. Rework `app_main.c`.
- [ ] **Step 3:** Run checks 1 to 7; record the log evidence for each.
- [ ] **Step 4:** Commit: `Player: decoder and output tasks, commands, events, volume and pause`.

---

### Task 7: MP3

**Files:**
- Modify: `main/player.c`

**Interfaces:**
- Consumes: the player from Task 6.
- Produces: `.mp3` files play (case-insensitive extension). `STARTED` reports the stream's rate, 16 bits and channel count; `duration_ms` is estimated from the file size and the bitrate the decoder reports once the first frame is decoded, and is 0 if that is not available. `ENDED frames=<played> expected=0`.

Requirements:
- Choose the decoder type from the extension. A file whose contents do not match its extension gives `ERROR reason=decode` or `unsupported`, not a crash.
- The decode loop is the same loop as FLAC, not a copy: one code path, parameterised by decoder type.
- MP3 frame counts are not exact (encoder padding). Accept a played count within 1.5% of the nominal length.

Board checks: `t04_44k_cbr192_st.mp3` and `t05_48k_vbr_mono.mp3` each give `STARTED` with the right rate and channel count and `ENDED` with frames within 1.5% of 441000 and 480000. Then FLAC, MP3, FLAC back to back without resetting. Report the MP3 decode speed if you can measure it without adding permanent code.

- [ ] **Step 1:** Implement; build.
- [ ] **Step 2:** Run the board checks; record the lines.
- [ ] **Step 3:** Commit: `Player: MP3 playback`.

---

### Task 8: Seeking

**Files:**
- Modify: `main/player.c`

**Interfaces:**
- Consumes: `flac_header_t.audio_start` and `total_frames` (Task 2); the player (Tasks 6 and 7).
- Produces: `player_seek_rel(delta_ms)` and a non-zero `start_ms` in `player_play` work for FLAC and MP3.

Method (spec section 4, "Seeking"; FLAC result measured in stage 1):
- Target byte offset = `audio_start + (file_size - audio_start) * target_ms / duration_ms`. Approximate by design.
- **FLAC:** `esp_audio_simple_dec_reset()`, feed the file's bytes `[0, audio_start)` again so the parser has the stream parameters, then continue from the target offset. (Method B; methods without the header replay fail with `ESP_AUDIO_ERR_NOT_SUPPORT`.)
- **MP3:** `esp_audio_simple_dec_reset()` then continue from the target offset; verify on the board that this works and say so in your report. If it does not, report DONE_WITH_CONCERNS with the evidence and leave MP3 seeking disabled.
- Seeking fades out, discards buffered audio, repositions, and fades in. Position reporting continues from the target. A target before the start clamps to 0; a target at or past the end finishes the track (`ENDED`). Seeking while paused repositions and stays paused. Seeking while idle is ignored. Where the duration is unknown (0), seeking is ignored.
- Header replay must not re-read a large picture block from the card on every seek if that can be avoided; measure the time from the `seek` command to the next `POSITION` line for the music track and for a file with a large embedded picture if one is available, and report it.

Board checks:
1. `play t06` (30 s), at about 3 s `seek 10000`: the next `POSITION` is within 500 ms of 13000 plus the time elapsed; the track ends with `ENDED` and total frames played is between 60% and 72% of 1323000.
2. `seek -5000` at about 2 s clamps to 0 and plays on. `seek 60000` ends the track with `ENDED`.
3. `play /sdcard/test/t06_tone_30s.flac 20000`: first `POSITION` near 20500; frames played about one third of the file.
4. `pause`, `seek 5000`, wait 2 s (no `POSITION`), `resume`: position continues from the new place.
5. MP3: the same as check 1 on `t04` with `seek 4000`.
6. Twenty seeks in a row, 300 ms apart, alternating `+2000` and `-1000`, on the music track: no crash, no decoder error, playback continues, and the track still reaches `ENDED`.

- [ ] **Step 1:** Implement; build.
- [ ] **Step 2:** Run the board checks; record the lines and the seek latency.
- [ ] **Step 3:** Commit: `Player: seeking for FLAC and MP3`.

---

### Task 9: Wait for the card

**Files:**
- Modify: `main/app_main.c`, `components/dap_hw/storage.c` only if its interface needs it

**Interfaces:**
- Produces: at start-up, if the card does not mount, the LCD shows `Insert SD card` and the firmware retries every 2 seconds until it mounts, then carries on as normal. No reset is needed after re-inserting the card.

Requirements:
- The retry must not block the UI task or the keypad; the LCD and keys stay alive while waiting.
- A failed attempt must release whatever the mount attempt acquired, so retries do not leak (check `storage_mount`'s failure path and the IDF mount function's behaviour; say what you found).
- One log line per failed attempt at most, and the IDF's own error lines from each attempt are acceptable.

Board check: the agent cannot remove the card. Verify what can be verified: a normal start still mounts on the first attempt and plays; and, by temporarily forcing the first two attempts to fail in code (not committed), the retry path shows the message, retries at 2 s intervals and then plays. The real test is in the user's hardware check.

- [ ] **Step 1:** Implement; build.
- [ ] **Step 2:** Run the checks; record the lines.
- [ ] **Step 3:** Commit: `Wait for the SD card at start-up, retrying every 2 seconds`.

---

## Stage 3 hardware check (run by the user)

On the phone charger unless noted.

1. **No more skip.** Play the music track from the start. The jump around 0:04 is gone.
2. **Sound quality.** It sounds at least as clean as before stage 3. (Audio now goes to the DAC as 32-bit frames; if there is new hiss, buzz or distortion, say so: the fallback is 16-bit frames for 16-bit files.)
3. **Pause and resume** with `D`, several times, on music and on the steady test tone: no click, pop or buzz, and it resumes where it stopped.
4. **Volume** with `A` and `B`, tapped and held: steps are even, no clicks or zipper noise, level 0 is silent, the number on the LCD follows.
5. **Keys do not disturb the music.** Mash keys while it plays.
6. **The LCD** shows the track name, a running time and the volume.
7. **Card retry.** On the computer's USB port: plug in (the LCD should say `Insert SD card`), then take the card out and click it back in. Music should start within a few seconds, with no reset.
8. **Optional:** copy a 24-bit FLAC and an MP3 of real music to the card; the coordinator can play them from the console for a listen.
