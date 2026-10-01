# DAP Stage 1 (Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the single-file SD/FLAC example into the module layout the DAP needs, with reproducible build settings and a host test project, while the firmware still plays the first FLAC on the card exactly as before; and find out whether FLAC can be seeked.

**Architecture:** Hardware drivers move into a `dap_hw` component (`storage`, `audio_out`), hardware-free logic into a `dap_core` component that also builds on the host with gcc, and the decode loop into `main/player.c`. A serial-capture tool gives an objective "same behaviour" check: the decoder's block count for the test track must match a baseline taken before any change.

**Tech Stack:** ESP-IDF v6.0.2 (C), `espressif/esp_audio_codec` 2.6.2, FreeRTOS, CMake, Unity 2.6 (shipped inside ESP-IDF), gcc with AddressSanitizer for host tests, pyserial 3.5 (in the ESP-IDF Python environment).

**Spec:** `docs/superpowers/specs/2026-09-30-dap-design.md` (read sections 2, 3, 4, 5, 12, 13 and stage 1 of section 14).

## Global Constraints

- ESP-IDF v6.0.2 at `/opt/esp-idf`. Every shell that runs `idf.py` first runs `source /opt/esp-idf/export.sh`.
- The board is at `/dev/ttyUSB0`. Serial access needs the `uucp` group, and `sg` is not installed. Run anything that opens the port as: `echo '<command>' | newgrp uucp`.
- Target board: Freenove ESP32-WROVER CAM. SD card is **1-bit SDMMC only** on fixed pins CLK=14, CMD=15, D0=2. Never configure 4-bit.
- DAC pins: BCK=26, LRCK=25, DIN=22. Never drive GPIO 0 or 12.
- Stage 1 changes **no audio behaviour**: I2S stays at the file's native bit width, Philips format, no MCLK, `dma_desc_num = 8`, `dma_frame_num = 1000`. (The 32-bit output format in the spec is stage 3.)
- After playback the I2S channel must be stopped by writing silence, waiting 50 ms, then `i2s_channel_disable()`. Never leave it enabled and idle.
- The decoder input buffer must be `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`. Never move or overwrite input bytes while the decoder's `raw.len` is non-zero.
- Register both `esp_audio_dec_register_default()` and `esp_audio_simple_dec_register_default()`.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` stays 16384 (the FLAC decoder overflows smaller stacks).
- `dap_core` contains pure C using only the C standard library. No ESP-IDF or FreeRTOS includes.
- GPIO numbers appear only in `main/pins.h`.
- Performance (spec section 3, "Performance rules"): no floating point, division, per-sample function calls, heap allocation or logging inside audio loops; no linear search inside a loop; single-pass, table-driven text handling; no busy-waiting. Prefer the efficient algorithm when two are equally clear.
- `MINIMAL_BUILD` stays on in the top-level `CMakeLists.txt`; each component lists what it uses in `REQUIRES`/`PRIV_REQUIRES`.
- `main/sd_card_example_main.c` is moved with `git mv`, byte-for-byte. Do not edit, tidy or delete its commented-out `--christopher` blocks.
- Match the existing code style: 2-space indent, `static const char *TAG`, `ESP_LOGx` logging, explanatory comments that say *why*.
- Commit messages end with: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Work on branch `main` unless a task says otherwise. Tasks run in order; they share one board and one build directory, so never run two at once.

## Review Focus

Conditions the spec implies that are most likely to bite, and where each is pinned:

1. **Truncated or invalid UTF-8 in a tag** (a lead byte at the very end of the string, a stray continuation byte) must give `?` and must not read past the terminator. Pinned in Task 3 by `test_invalid_utf8_*`, run under AddressSanitizer.
2. **Output buffer too small** (16-character LCD fields, a multi-character replacement such as `...` straddling the end, size 0 and size 1) must truncate and stay NUL-terminated with no overflow. Pinned in Task 3 by `test_truncation_*`.
3. **Common typographic punctuation** (curly apostrophes, dashes, ellipsis) appears in a large share of real song titles and must not show as `?`. Pinned in Task 3 by `test_typographic_punctuation`.
4. **Starting audio a second time** after a stop. The old code created the I2S channel on every start and never released it, so a second track would abort. Pinned in Task 4 by the design of `audio_out_start` (create once, reconfigure afterwards) and exercised on hardware in Task 5, which starts and stops output at least 16 times.
5. **No card, or a card with no FLAC** must log and return, not crash or reboot-loop. Cannot be automated (nobody can pull the card remotely). Pinned in Task 4 by an explicit code-path requirement and listed for the reviewer to read.

## File Structure

| Path | Responsibility | Task |
|---|---|---|
| `tools/serial_capture.py` | Reset the board and print its serial output until a marker line or timeout | 1 |
| `docs/superpowers/plans/stage1-baseline.log` | Serial log of the unmodified firmware playing the test track | 1 |
| `sdkconfig.defaults` | The only tracked build configuration | 2, 4 |
| `components/dap_core/translit.c/.h` | UTF-8 to LCD character conversion | 3 |
| `components/dap_core/CMakeLists.txt` | Registers `dap_core` for the firmware build | 3 |
| `test/host/CMakeLists.txt`, `test/host/test_translit.c` | Host build of `dap_core` plus Unity tests | 3 |
| `reference/` | The original example, unbuilt | 4 |
| `main/pins.h` | Every GPIO number | 4 |
| `components/dap_hw/storage.c/.h` | Mount and unmount the SD card | 4 |
| `components/dap_hw/audio_out.c/.h` | I2S start, write, clean stop | 4 |
| `main/player.c/.h` | Blocking FLAC decode loop (becomes tasks in stage 3) | 4 |
| `main/app_main.c` | Start-up: mount, find first FLAC, play | 4 |
| `README.md` | Short project readme replacing the example's | 4 |
| branch `spike/flac-seek` | Throwaway seek test firmware, never merged | 5 |

`translit` is brought forward from stage 4 so the host test project is proven with a real, fully specified module rather than an empty shell.

---

### Task 1: Serial capture tool and behaviour baseline

**Files:**
- Create: `tools/serial_capture.py`
- Create: `docs/superpowers/plans/stage1-baseline.log`

**Interfaces:**
- Produces: `python3 tools/serial_capture.py <port> <timeout_seconds> [marker]` — resets the board, echoes serial output to stdout, exits 0 when a line containing `marker` is seen, exits 1 on timeout. Later tasks use it for every hardware check.
- Produces: the two baseline lines later tasks must reproduce, `FLAC: <rate> Hz, <n> channels, <bits> bits` and `Finished: <N> blocks decoded`.

- [ ] **Step 1: Write the capture tool**

Create `tools/serial_capture.py`:

```python
#!/usr/bin/env python3
"""Reset the ESP32 and print its serial output until a marker line or timeout.

Usage: serial_capture.py <port> <timeout_seconds> [marker]
Exit status: 0 if the marker was seen (or no marker was given), 1 on timeout.
"""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    port = sys.argv[1]
    timeout = float(sys.argv[2])
    marker = sys.argv[3].encode() if len(sys.argv) > 3 else None

    ser = serial.Serial(port, 115200, timeout=0.2)
    # The board's auto-reset circuit wires RTS to EN and DTR to GPIO0.
    # Keep DTR released so GPIO0 stays high (normal boot), then pulse EN low.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False

    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        data = ser.read(4096)
        if not data:
            continue
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
        if marker:
            seen = (seen + data)[-65536:]
            if marker in seen:
                time.sleep(0.3)  # let the rest of the line arrive
                sys.stdout.write(ser.read(4096).decode("utf-8", "replace"))
                sys.stdout.flush()
                return 0
    return 1 if marker else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Build the unmodified firmware**

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: ends with `Project build complete.`

- [ ] **Step 3: Flash it**

Run: `echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp`
Expected: `Hard resetting via RTS pin...` and `Done`.

- [ ] **Step 4: Capture the baseline**

The track plays in real time, so this takes as long as the song. Run it in the background and wait for it to finish (allow 15 minutes):

```bash
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 900 "blocks decoded" > docs/superpowers/plans/stage1-baseline.log; echo "capture exit: $?"' | newgrp uucp
```

Expected: `capture exit: 0`.

Then run: `grep -oE "(FLAC: .*|Finished: .*|Playing .*)" docs/superpowers/plans/stage1-baseline.log`
Expected: three lines, for example `Playing /sdcard/music/<name>.flac`, `FLAC: 44100 Hz, 2 channels, 16 bits`, `Finished: <N> blocks decoded`.

If the log shows `No .flac found` or `Failed to mount`, STOP and report BLOCKED: the card is missing or has no FLAC. Do not continue.
If the log contains `Decode failed`, STOP and report BLOCKED with the line.

- [ ] **Step 5: Commit**

```bash
git add tools/serial_capture.py docs/superpowers/plans/stage1-baseline.log
git commit -m "Add serial capture tool and playback baseline log

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Reproducible build settings

**Files:**
- Modify: `sdkconfig.defaults` (regenerated)
- Modify: `.gitignore`
- Untrack: `sdkconfig`

**Interfaces:**
- Consumes: `tools/serial_capture.py` and `docs/superpowers/plans/stage1-baseline.log` from Task 1.
- Produces: a `sdkconfig.defaults` from which a deleted `sdkconfig` regenerates with PSRAM, the 16384-byte main stack, 4MB flash, long file names, UTF-8 file-name encoding, `-O2`, 240 MHz and minimum chip revision 3. `sdkconfig` itself is no longer tracked.

Background: the settings that make audio work (PSRAM, enlarged stack) were set by hand in `sdkconfig`, which is a generated file. `idf.py save-defconfig` writes the minimal set of non-default options to `sdkconfig.defaults`.

**Do not remove the `CONFIG_EXAMPLE_*` lines in this task.** The example's Kconfig still exists until Task 4 and defaults to a 4-bit SD bus, which this board cannot use.

- [ ] **Step 1: Generate the minimal defaults**

```bash
source /opt/esp-idf/export.sh
cp sdkconfig sdkconfig.bak-pre-defaults
idf.py save-defconfig
cat sdkconfig.defaults
```

Expected: `sdkconfig.defaults` now lists the non-default options. (`sdkconfig.bak-*` is already git-ignored.)

- [ ] **Step 2: Check the required settings are present**

```bash
for k in CONFIG_SPIRAM=y CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384 CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y CONFIG_FATFS_LFN_HEAP=y CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096 CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_1=y; do
  grep -qx "$k" sdkconfig.defaults && echo "ok   $k" || echo "MISSING $k"
done
```

Expected: six `ok` lines. For any `MISSING` line, append that exact line to `sdkconfig.defaults`.

- [ ] **Step 3: Switch file names to UTF-8**

Append to `sdkconfig.defaults`:

```
# File names on the card are UTF-8. The stock setting (code page 437) mangles
# anything outside basic ASCII.
CONFIG_FATFS_API_ENCODING_UTF_8=y
```

- [ ] **Step 4: Regenerate `sdkconfig` from defaults and compare**

```bash
rm sdkconfig
idf.py reconfigure
diff <(grep -E '^CONFIG_' sdkconfig.bak-pre-defaults | sort) <(grep -E '^CONFIG_' sdkconfig | sort)
```

Expected: the only differences are the file-name encoding, i.e.

```
< CONFIG_FATFS_API_ENCODING_ANSI_OEM=y
> CONFIG_FATFS_API_ENCODING_UTF_8=y
```

If any other line differs, add the old value (the `<` line) to `sdkconfig.defaults`, then repeat this step until the diff shows only the encoding change.

- [ ] **Step 5: Turn on the performance settings**

The project is still on the example's debug-oriented defaults: `-Og`, 160 MHz, and a PSRAM workaround for old chip revisions that this board (revision 3.1) does not need.

First confirm the chip revision. Run:
`echo 'source /opt/esp-idf/export.sh && esptool.py -p /dev/ttyUSB0 chip_id' | newgrp uucp`
Expected: a line like `Chip is ESP32-D0WD-V3 (revision v3.1)` (older tool versions say `Chip type:`). If the revision is below 3.0, STOP and report BLOCKED; do not set the revision option.

Append to `sdkconfig.defaults`:

```
# Performance: optimise for speed, run at full clock speed.
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# This board's chip is revision 3.1. Declaring revision 3 as the minimum drops
# the PSRAM cache workaround needed only by older chips, which slows every
# PSRAM access and uses about 15KB of IRAM.
CONFIG_ESP32_REV_MIN_3=y
```

Regenerate and check each setting took effect:

```bash
cp sdkconfig sdkconfig.bak-pre-perf
rm sdkconfig
idf.py reconfigure
grep -E "^CONFIG_COMPILER_OPTIMIZATION_PERF=y|^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240|^CONFIG_ESP32_REV_MIN_FULL=300|^CONFIG_SPIRAM=y|^CONFIG_SPIRAM_USE_MALLOC=y|^CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384" sdkconfig
grep -c "^CONFIG_SPIRAM_CACHE_WORKAROUND=y" sdkconfig
diff <(grep -E '^CONFIG_' sdkconfig.bak-pre-perf | sort) <(grep -E '^CONFIG_' sdkconfig | sort) | grep -E '^[<>]' | grep -vE "COMPILER_OPTIMIZATION|CPU_FREQ|REV_MIN|SPIRAM_CACHE_|SPIRAM_WORKAROUND"
```

Expected: the first `grep` prints all six lines; the count is `0`; the final command prints nothing. If the final command prints any line, copy it into your report under "unexpected config changes" and continue: the coordinator rules on it.

- [ ] **Step 6: Stop tracking the generated file**

Append to `.gitignore`:

```
# Generated from sdkconfig.defaults
sdkconfig

# Host test build output
test/host/build/
```

Run: `git rm --cached sdkconfig`
Expected: `rm 'sdkconfig'`. The file stays on disk.

- [ ] **Step 7: Build, flash, and verify behaviour is unchanged**

```bash
source /opt/esp-idf/export.sh && idf.py build
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 900 "blocks decoded" > /tmp/stage1-task2.log; echo "capture exit: $?"' | newgrp uucp
diff <(grep -oE "(FLAC: .*|Finished: .*)" docs/superpowers/plans/stage1-baseline.log) <(grep -oE "(FLAC: .*|Finished: .*)" /tmp/stage1-task2.log) && echo SAME
```

(Run the capture in the background and wait; it lasts one full track.)
Expected: `capture exit: 0` and `SAME`. Also run `grep -cE "Decode failed|Guru Meditation|abort\(\)|Backtrace|rev.* is not supported" /tmp/stage1-task2.log` and expect `0`.

Run `grep -iE "cpu freq|chip rev|SPI RAM|spiram" /tmp/stage1-task2.log | head` and include the output in your report.

The first build after changing the optimisation level recompiles everything and takes several minutes.

- [ ] **Step 8: Commit**

```bash
git add sdkconfig.defaults .gitignore
git commit -m "Make build settings reproducible from sdkconfig.defaults

PSRAM, the 16K main stack and 4MB flash were only in the generated
sdkconfig. Capture them in sdkconfig.defaults, switch FAT file names to
UTF-8, and stop tracking sdkconfig.

Also turn on the performance settings: -O2, 240 MHz, and minimum chip
revision 3, which drops the PSRAM cache workaround.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Host test project and `translit`

**Files:**
- Create: `components/dap_core/CMakeLists.txt`
- Create: `components/dap_core/translit.h`
- Create: `components/dap_core/translit.c`
- Create: `test/host/CMakeLists.txt`
- Test: `test/host/test_translit.c`

**Interfaces:**
- Produces:
  - `translit_stats_t translit(const char *utf8, char *out, size_t out_size);`
  - `void translit_title(const char *title, const char *file_name, char *out, size_t out_size);`
  - `typedef struct { size_t chars; size_t unknown; } translit_stats_t;`
- Produces the host test commands used by every later stage:
  `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`

Rules being implemented (spec section 5, "Text conversion for the LCD"):
- ASCII passes through, except `\` becomes `/` and `~` becomes `-`. Control characters become a space.
- Accented Latin letters (U+00C0–U+00FF and U+0100–U+017F) become their base letter. Multi-letter cases: `Æ`→`AE`, `æ`→`ae`, `ß`→`ss`, `Þ`→`Th`, `þ`→`th`, `Œ`→`OE`, `œ`→`oe`, `Ĳ`→`IJ`, `ĳ`→`ij`.
- Typographic punctuation: curly single quotes and prime → `'`; curly double quotes, `«`, `»` → `"`; hyphens and dashes U+2010–U+2015 → `-`; `…` → `...`; `•` → `*`; non-breaking space → space; `¡` → `!`; `¿` → `?`.
- Anything else, and any invalid UTF-8 byte, becomes `?` and counts as unknown.
- `translit_title`: if the title is empty, or more than half of its characters are unknown, use the file name without its extension instead.

**C string warning:** a hex escape swallows every following hex digit, so `"\xC3\xB3d"` is a compile error (`\xB3d`). Always close the string after an escape and open a new one: `"\xC3\xB3" "d"`.

- [ ] **Step 1: Create the host test project**

Create `test/host/CMakeLists.txt`:

```cmake
# Builds the hardware-free dap_core sources with the host compiler and runs
# their Unity tests. Not part of the firmware build.
cmake_minimum_required(VERSION 3.22)
project(dap_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED ENV{IDF_PATH})
  set(IDF_PATH $ENV{IDF_PATH})
else()
  set(IDF_PATH /opt/esp-idf)
endif()
set(UNITY_DIR ${IDF_PATH}/components/unity/unity/src)
if(NOT EXISTS ${UNITY_DIR}/unity.c)
  message(FATAL_ERROR "Unity not found at ${UNITY_DIR}; set IDF_PATH")
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/dap_core)

# Sanitizers turn out-of-bounds reads and writes into test failures.
add_compile_options(-Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})
target_compile_options(unity PRIVATE -w)

add_library(dap_core STATIC
  ${CORE_DIR}/translit.c
)
target_include_directories(dap_core PUBLIC ${CORE_DIR})

enable_testing()

function(dap_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE dap_core unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

dap_test(test_translit)
```

- [ ] **Step 2: Write the header and a stub so the tests can link**

Create `components/dap_core/translit.h`:

```c
#pragma once

#include <stddef.h>

// The LCD1602 shows basic ASCII only. These functions turn UTF-8 tag text into
// something it can display.

typedef struct {
  size_t chars;   // code points read from the input
  size_t unknown; // how many of those had no LCD equivalent and became '?'
} translit_stats_t;

// Convert `utf8` into `out`. At most `out_size` bytes are written, always
// including the terminator when `out_size` is non-zero. The returned counts
// cover the whole input even if the output was truncated. A NULL input gives
// an empty string.
translit_stats_t translit(const char *utf8, char *out, size_t out_size);

// Convert a track title for display. If the title is missing, empty, or more
// than half of it has no LCD equivalent, the file name without its extension
// is converted instead. Either argument may be NULL.
void translit_title(const char *title, const char *file_name, char *out,
                    size_t out_size);
```

Create `components/dap_core/translit.c` as a stub:

```c
#include "translit.h"

translit_stats_t translit(const char *utf8, char *out, size_t out_size) {
  (void)utf8;
  translit_stats_t stats = {0, 0};
  if (out_size > 0) {
    out[0] = '\0';
  }
  return stats;
}

void translit_title(const char *title, const char *file_name, char *out,
                    size_t out_size) {
  (void)title;
  (void)file_name;
  if (out_size > 0) {
    out[0] = '\0';
  }
}
```

- [ ] **Step 3: Write the failing tests**

Create `test/host/test_translit.c`:

```c
#include "translit.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ascii_passes_through(void) {
  char out[32];
  translit_stats_t s = translit("Kid A (2000)", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Kid A (2000)", out);
  TEST_ASSERT_EQUAL_UINT(12, s.chars);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_backslash_and_tilde_are_replaced(void) {
  char out[32];
  translit("AC\\DC ~1", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AC/DC -1", out);
}

static void test_control_characters_become_spaces(void) {
  char out[32];
  translit_stats_t s = translit("A\tB\nC", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A B C", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_latin1_accents_lose_their_marks(void) {
  char out[32];
  translit("Beyonc\xC3\xA9", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Beyonce", out);
  translit("Sigur R\xC3\xB3" "s", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Sigur Ros", out);
  translit("Mot\xC3\xB6" "rhead", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Motorhead", out);
  translit("\xC3\x98" "l", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Ol", out);
  translit("\xC3\x80\xC3\xBF", out, sizeof(out)); // first and last of the range
  TEST_ASSERT_EQUAL_STRING("Ay", out);
}

static void test_multi_letter_replacements(void) {
  char out[32];
  translit_stats_t s = translit("Stra\xC3\x9F" "e", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Strasse", out);
  TEST_ASSERT_EQUAL_UINT(6, s.chars); // counts input characters, not output
  translit("\xC3\x86" "on", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("AEon", out);
  translit("\xC3\xA6\xC3\x9E\xC3\xBE", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("aeThth", out);
  translit("\xC5\x92" "uvre \xC5\x93", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("OEuvre oe", out);
  translit("\xC4\xB2\xC4\xB3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("IJij", out);
}

static void test_latin_extended_a(void) {
  char out[32];
  translit("\xC5\x81\xC3\xB3" "d\xC5\xBA", out, sizeof(out)); // Lodz
  TEST_ASSERT_EQUAL_STRING("Lodz", out);
  translit("Dvo\xC5\x99\xC3\xA1" "k", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Dvorak", out);
  translit("\xC4\x80\xC5\xBF", out, sizeof(out)); // first and last of the range
  TEST_ASSERT_EQUAL_STRING("As", out);
  translit("\xC5\xA0\xC5\xA1\xC5\xBD\xC5\xBE", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("SsZz", out);
  translit("\xC4\x9E\xC4\xB1\xC5\x9F", out, sizeof(out)); // Turkish G, dotless i, s
  TEST_ASSERT_EQUAL_STRING("Gis", out);
}

static void test_typographic_punctuation(void) {
  char out[32];
  translit_stats_t s = translit("Don\xE2\x80\x99" "t Stop", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Don't Stop", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
  translit("\xE2\x80\x9C" "Hi\xE2\x80\x9D", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\"Hi\"", out);
  translit("A\xE2\x80\x93" "B\xE2\x80\x94" "C", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A-B-C", out);
  translit("Wait\xE2\x80\xA6", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Wait...", out);
  translit("\xC2\xAB" "x\xC2\xBB \xC2\xA1\xC2\xBF", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\"x\" !?", out);
  s = translit("a\xC2\xA0" "b\xE2\x80\xA2", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a b*", out);
  TEST_ASSERT_EQUAL_UINT(0, s.unknown);
}

static void test_unsupported_scripts_become_question_marks(void) {
  char out[32];
  translit_stats_t s = translit("\xE4\xBD\xA0\xE5\xA5\xBD", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("??", out);
  TEST_ASSERT_EQUAL_UINT(2, s.chars);
  TEST_ASSERT_EQUAL_UINT(2, s.unknown);
  s = translit("\xD0\x9C\xD0\xB8\xD1\x80", out, sizeof(out)); // Cyrillic
  TEST_ASSERT_EQUAL_STRING("???", out);
  TEST_ASSERT_EQUAL_UINT(3, s.unknown);
  s = translit("\xF0\x9F\x8E\xB5", out, sizeof(out)); // four-byte emoji
  TEST_ASSERT_EQUAL_STRING("?", out);
  TEST_ASSERT_EQUAL_UINT(1, s.chars);
}

static void test_invalid_utf8_stray_continuation(void) {
  char out[32];
  translit_stats_t s = translit("\x80" "abc", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("?abc", out);
  TEST_ASSERT_EQUAL_UINT(1, s.unknown);
}

static void test_invalid_utf8_truncated_at_end(void) {
  // Exactly-sized heap copies, so AddressSanitizer catches any read past the
  // terminator.
  const char two_byte[] = "abc\xC3";
  char *in = malloc(sizeof(two_byte));
  memcpy(in, two_byte, sizeof(two_byte));
  char out[32];
  translit(in, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("abc?", out);
  free(in);

  const char three_byte[] = "ab\xE4\xBD";
  in = malloc(sizeof(three_byte));
  memcpy(in, three_byte, sizeof(three_byte));
  translit_stats_t s = translit(in, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ab??", out);
  TEST_ASSERT_EQUAL_UINT(2, s.unknown);
  free(in);
}

static void test_truncation_keeps_terminator(void) {
  char out[5];
  translit_stats_t s = translit("Radiohead", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Radi", out);
  TEST_ASSERT_EQUAL_UINT(9, s.chars); // whole input is still counted
}

static void test_truncation_inside_multi_letter_replacement(void) {
  char out[3];
  translit("\xE2\x80\xA6", out, sizeof(out)); // "..." into room for two
  TEST_ASSERT_EQUAL_STRING("..", out);
}

static void test_truncation_size_one_and_zero(void) {
  char one[1] = {'x'};
  translit("abc", one, sizeof(one));
  TEST_ASSERT_EQUAL_CHAR('\0', one[0]);

  char canary[2] = {'x', 'y'};
  translit("abc", canary, 0);
  TEST_ASSERT_EQUAL_CHAR('x', canary[0]);
  TEST_ASSERT_EQUAL_CHAR('y', canary[1]);
}

static void test_null_input_gives_empty_string(void) {
  char out[8] = "junk";
  translit_stats_t s = translit(NULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT(0, s.chars);
}

static void test_title_is_used_when_displayable(void) {
  char out[32];
  translit_title("Beyonc\xC3\xA9", "01 track.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Beyonce", out);
}

static void test_title_mostly_unknown_falls_back_to_file_name(void) {
  char out[32];
  translit_title("\xE4\xBD\xA0\xE5\xA5\xBD", "nihao.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("nihao", out);
}

static void test_title_exactly_half_unknown_is_kept(void) {
  char out[32];
  translit_title("A\xE4\xBD\xA0", "fallback.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A?", out);
}

static void test_title_missing_falls_back_to_file_name(void) {
  char out[32];
  translit_title("", "07 - Song.mp3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("07 - Song", out);
  translit_title(NULL, "07 - Song.mp3", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("07 - Song", out);
}

static void test_title_fallback_extension_handling(void) {
  char out[32];
  translit_title("", "README", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("README", out);
  translit_title("", "a.b.flac", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a.b", out);
  translit_title("", ".hidden", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(".hidden", out);
  translit_title(NULL, NULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_passes_through);
  RUN_TEST(test_backslash_and_tilde_are_replaced);
  RUN_TEST(test_control_characters_become_spaces);
  RUN_TEST(test_latin1_accents_lose_their_marks);
  RUN_TEST(test_multi_letter_replacements);
  RUN_TEST(test_latin_extended_a);
  RUN_TEST(test_typographic_punctuation);
  RUN_TEST(test_unsupported_scripts_become_question_marks);
  RUN_TEST(test_invalid_utf8_stray_continuation);
  RUN_TEST(test_invalid_utf8_truncated_at_end);
  RUN_TEST(test_truncation_keeps_terminator);
  RUN_TEST(test_truncation_inside_multi_letter_replacement);
  RUN_TEST(test_truncation_size_one_and_zero);
  RUN_TEST(test_null_input_gives_empty_string);
  RUN_TEST(test_title_is_used_when_displayable);
  RUN_TEST(test_title_mostly_unknown_falls_back_to_file_name);
  RUN_TEST(test_title_exactly_half_unknown_is_kept);
  RUN_TEST(test_title_missing_falls_back_to_file_name);
  RUN_TEST(test_title_fallback_extension_handling);
  return UNITY_END();
}
```

- [ ] **Step 4: Run the tests and confirm they fail**

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: builds, then `test_translit` FAILS, with Unity reporting failures such as `Expected 'Kid A (2000)' Was ''`. A handful of tests that expect an empty string pass against the stub; that is fine.

- [ ] **Step 5: Implement `translit`**

Replace `components/dap_core/translit.c` with:

```c
#include "translit.h"

#include <stdint.h>
#include <string.h>

// Base letters for U+00C0..U+00FF, one per code point. Entries whose real
// replacement is two letters (AE, Th, ss, ae, th) are handled before this
// table is consulted; the letter here is only a filler.
static const char LATIN1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

// Base letters for U+0100..U+017F. IJ/ij and OE/oe are handled separately.
static const char LATIN_EXT_A[] =
    "AaAaAa"       // 0100
    "CcCcCcCc"     // 0106
    "DdDd"         // 010E
    "EeEeEeEeEe"   // 0112
    "GgGgGgGg"     // 011C
    "HhHh"         // 0124
    "IiIiIiIiIi"   // 0128
    "Jj"           // 0132 (IJ ligatures, filler)
    "Jj"           // 0134
    "Kkk"          // 0136
    "LlLlLlLlLl"   // 0139
    "NnNnNn"       // 0143
    "n"            // 0149
    "Nn"           // 014A
    "OoOoOo"       // 014C
    "Oo"           // 0152 (OE ligatures, filler)
    "RrRrRr"       // 0154
    "SsSsSsSs"     // 015A
    "TtTtTt"       // 0162
    "UuUuUuUuUuUu" // 0168
    "Ww"           // 0174
    "Yy"           // 0176
    "Y"            // 0178
    "ZzZzZz"       // 0179
    "s";           // 017F

_Static_assert(sizeof(LATIN1) == 64 + 1, "LATIN1 must cover U+00C0..U+00FF");
_Static_assert(sizeof(LATIN_EXT_A) == 128 + 1,
               "LATIN_EXT_A must cover U+0100..U+017F");

#define INVALID_CODE_POINT 0xFFFFFFFFu

// Decode one code point and advance *pp past it. A malformed sequence consumes
// a single byte and yields INVALID_CODE_POINT. Never reads past the string's
// terminator: a NUL is not a continuation byte, so it ends the sequence.
static uint32_t next_code_point(const unsigned char **pp) {
  const unsigned char *p = *pp;
  uint32_t cp = p[0];
  int extra;

  if (cp < 0x80) {
    *pp = p + 1;
    return cp;
  }
  if ((cp & 0xE0) == 0xC0) {
    extra = 1;
    cp &= 0x1F;
  } else if ((cp & 0xF0) == 0xE0) {
    extra = 2;
    cp &= 0x0F;
  } else if ((cp & 0xF8) == 0xF0) {
    extra = 3;
    cp &= 0x07;
  } else {
    *pp = p + 1;
    return INVALID_CODE_POINT;
  }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      *pp = p + 1;
      return INVALID_CODE_POINT;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *pp = p + extra + 1;
  return cp;
}

// Return the LCD replacement for a code point, or NULL if there is none.
// `one` is caller-provided scratch for single-character results.
static const char *replacement_for(uint32_t cp, char one[2]) {
  one[1] = '\0';

  if (cp < 0x80) {
    // The HD44780's character ROM shows a yen sign for '\' and an arrow for
    // '~', so swap them for look-alikes.
    if (cp == '\\') {
      return "/";
    }
    if (cp == '~') {
      return "-";
    }
    if (cp < 0x20 || cp == 0x7F) {
      return " ";
    }
    one[0] = (char)cp;
    return one;
  }

  switch (cp) {
  case 0x00A0: // no-break space
    return " ";
  case 0x00A1: // inverted exclamation mark
    return "!";
  case 0x00BF: // inverted question mark
    return "?";
  case 0x00AB: // guillemets
  case 0x00BB:
    return "\"";
  case 0x00C6:
    return "AE";
  case 0x00DE:
    return "Th";
  case 0x00DF:
    return "ss";
  case 0x00E6:
    return "ae";
  case 0x00FE:
    return "th";
  case 0x0132:
    return "IJ";
  case 0x0133:
    return "ij";
  case 0x0152:
    return "OE";
  case 0x0153:
    return "oe";
  case 0x2018: // curly single quotes, low quote, prime
  case 0x2019:
  case 0x201A:
  case 0x2032:
    return "'";
  case 0x201C: // curly double quotes, low quote, double prime
  case 0x201D:
  case 0x201E:
  case 0x2033:
    return "\"";
  case 0x2010: // hyphens and dashes
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
    return "-";
  case 0x2022: // bullet
    return "*";
  case 0x2026: // ellipsis
    return "...";
  default:
    break;
  }

  if (cp >= 0x00C0 && cp <= 0x00FF) {
    one[0] = LATIN1[cp - 0x00C0];
    return one;
  }
  if (cp >= 0x0100 && cp <= 0x017F) {
    one[0] = LATIN_EXT_A[cp - 0x0100];
    return one;
  }
  return NULL;
}

translit_stats_t translit(const char *utf8, char *out, size_t out_size) {
  translit_stats_t stats = {0, 0};
  size_t used = 0;

  if (out_size > 0) {
    out[0] = '\0';
  }
  if (!utf8) {
    return stats;
  }

  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    char one[2];
    const char *rep = replacement_for(next_code_point(&p), one);
    stats.chars++;
    if (!rep) {
      rep = "?";
      stats.unknown++;
    }
    // Keep reading after the output fills, so the counts describe the whole
    // input; translit_title's fallback rule depends on that.
    for (; *rep; rep++) {
      if (out_size > 0 && used + 1 < out_size) {
        out[used++] = *rep;
      }
    }
  }
  if (out_size > 0) {
    out[used] = '\0';
  }
  return stats;
}

void translit_title(const char *title, const char *file_name, char *out,
                    size_t out_size) {
  if (title && title[0]) {
    translit_stats_t stats = translit(title, out, out_size);
    if (stats.unknown * 2 <= stats.chars) {
      return;
    }
  }

  // Fall back to the file name without its extension. A leading dot is part
  // of the name, not an extension.
  char stem[256];
  const char *name = file_name ? file_name : "";
  size_t len = strlen(name);
  const char *dot = strrchr(name, '.');
  if (dot && dot != name) {
    len = (size_t)(dot - name);
  }
  if (len >= sizeof(stem)) {
    len = sizeof(stem) - 1;
  }
  memcpy(stem, name, len);
  stem[len] = '\0';
  translit(stem, out, out_size);
}
```

- [ ] **Step 6: Run the tests and confirm they pass**

Run: `cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`. Run `test/host/build/test_translit` directly and expect `19 Tests 0 Failures 0 Ignored` with no sanitizer output.

- [ ] **Step 7: Register `dap_core` for the firmware build**

Create `components/dap_core/CMakeLists.txt`:

```cmake
# Hardware-free logic. Pure C: no ESP-IDF or FreeRTOS includes, so the same
# sources also build with the host compiler under test/host.
idf_component_register(SRCS "translit.c"
                       INCLUDE_DIRS ".")
```

In `main/CMakeLists.txt`, change the `REQUIRES` line so the component is compiled for the target:

```cmake
                       REQUIRES fatfs sd_card esp_driver_i2s dap_core
```

Run: `source /opt/esp-idf/export.sh && idf.py build`
Expected: `Project build complete.` and the build output mentions `translit.c`.

- [ ] **Step 8: Commit**

```bash
git add components/dap_core test/host main/CMakeLists.txt
git commit -m "Add host test project and LCD text conversion

dap_core holds hardware-free logic that builds for the ESP32 and for the
host. translit converts UTF-8 tag text to what the LCD1602 can show.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Split the example into modules

**Files:**
- Move: `main/sd_card_example_main.c` → `reference/sd_card_example_main.c`
- Move: `main/Kconfig.projbuild` → `reference/Kconfig.projbuild`
- Move: `components/sd_card/` → `reference/sd_card/`
- Move: `pytest_sdmmc_card_example.py`, `sdkconfig.ci`, `README.md` → `reference/`
- Create: `main/pins.h`, `main/player.h`, `main/player.c`, `main/app_main.c`
- Create: `components/dap_hw/CMakeLists.txt`, `components/dap_hw/storage.h`, `components/dap_hw/storage.c`, `components/dap_hw/audio_out.h`, `components/dap_hw/audio_out.c`
- Create: `README.md`
- Modify: `CMakeLists.txt`, `main/CMakeLists.txt`, `sdkconfig.defaults`

**Interfaces:**
- Consumes: `tools/serial_capture.py`, the baseline log, `dap_core` from Task 3.
- Produces (`storage.h`):
  - `#define STORAGE_MOUNT_POINT "/sdcard"`
  - `esp_err_t storage_mount(void);` — idempotent; returns the mount error, never aborts
  - `void storage_unmount(void);`
  - `bool storage_is_mounted(void);`
- Produces (`audio_out.h`):
  - `typedef struct { int bck; int lrck; int dout; } audio_out_pins_t;`
  - `void audio_out_init(const audio_out_pins_t *pins);`
  - `esp_err_t audio_out_start(uint32_t sample_rate, int channels, int bits);`
  - `esp_err_t audio_out_write(const void *data, size_t len);`
  - `void audio_out_stop(void);`
- Produces (`player.h`):
  - `void player_init(void);`
  - `esp_err_t player_play_file(const char *path);` — blocking, FLAC only
- Produces (`pins.h`): `PIN_I2S_BCK`, `PIN_I2S_LRCK`, `PIN_I2S_DOUT`, `PIN_LCD_SDA`, `PIN_LCD_SCL`, `PIN_KEYPAD_ROW_1..4`, `PIN_KEYPAD_COL_1..4`.

Intentional removals from the old `app_main` (they were example residue, not player behaviour): writing and reading back `nihao.txt` on every boot, the directory listing, the pin-debug helper, and the unused `play_tone`. All remain readable in `reference/`.

The log lines `FLAC: ... Hz, ... channels, ... bits` and `Finished: ... blocks decoded` must keep their exact wording; the behaviour check compares them with the baseline.

On the ESP32 the SDMMC slot's pins are fixed in silicon (`SOC_SDMMC_USE_IOMUX`), so `storage` takes no pin arguments.

- [ ] **Step 1: Move the example out of the build**

```bash
mkdir -p reference
git mv main/sd_card_example_main.c reference/sd_card_example_main.c
git mv main/Kconfig.projbuild reference/Kconfig.projbuild
git mv components/sd_card reference/sd_card
git mv pytest_sdmmc_card_example.py sdkconfig.ci README.md reference/
```

- [ ] **Step 2: Create `main/pins.h`**

```c
#pragma once

// Every GPIO the player uses. No other file may contain a pin number.
//
// SD card (1-bit SDMMC) is not listed as a setting because the ESP32 fixes
// those pins in silicon: CLK = 14, CMD = 15, D0 = 2. The camera connector
// shares what would be D1-D3, so 4-bit mode is impossible on this board.
//
// Never use GPIO 0 or 12: they are boot strapping pins, and 12 held high at
// reset stops this module from booting.
//
// Spare, reserved for future hardware: 5, 18, 19, 23 (the VSPI group).

// PCM5102A DAC, I2S. The DAC's SCK pin is tied to GND; it makes its own clock.
#define PIN_I2S_BCK 26  // DAC "BCK"
#define PIN_I2S_LRCK 25 // DAC "LCK"
#define PIN_I2S_DOUT 22 // DAC "DIN"

// LCD1602 behind a PCF8574 I2C backpack. SCL is not the board's default
// (22), because 22 is the DAC data line.
#define PIN_LCD_SDA 21
#define PIN_LCD_SCL 27

// 4x4 keypad. Rows are driven by the ESP32.
#define PIN_KEYPAD_ROW_1 32
#define PIN_KEYPAD_ROW_2 33
#define PIN_KEYPAD_ROW_3 4
#define PIN_KEYPAD_ROW_4 13

// Columns are read. These four pins are input-only and have no internal
// pull-up, so each needs an external 10k resistor to 3.3V.
#define PIN_KEYPAD_COL_1 34
#define PIN_KEYPAD_COL_2 35
#define PIN_KEYPAD_COL_3 36
#define PIN_KEYPAD_COL_4 39
```

- [ ] **Step 3: Create the `dap_hw` component**

`components/dap_hw/CMakeLists.txt`:

```cmake
# Drivers for the player's hardware. Depends on ESP-IDF; not built on the host.
idf_component_register(SRCS "storage.c" "audio_out.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_i2s)
```

`components/dap_hw/storage.h`:

```c
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define STORAGE_MOUNT_POINT "/sdcard"

// Mount the SD card's FAT filesystem at STORAGE_MOUNT_POINT over 1-bit SDMMC.
// Safe to call when already mounted. Returns the driver's error if there is
// no card or it cannot be read; never formats the card.
esp_err_t storage_mount(void);

// Unmount the card. Does nothing if it is not mounted.
void storage_unmount(void);

bool storage_is_mounted(void);
```

`components/dap_hw/storage.c`:

```c
#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdio.h>

static const char *TAG = "storage";

static sdmmc_card_t *s_card;

esp_err_t storage_mount(void) {
  if (s_card) {
    return ESP_OK;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false, // never wipe a card full of music
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
  };

  // Default speed (20MHz). The pins are fixed by the chip, see pins.h.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.unaligned_multi_block_rw_max_chunk_size = 8;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  // 1-bit only: this board's camera connector shares the other data lines.
  slot_config.width = 1;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ESP_LOGI(TAG, "Mounting SD card");
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host,
                                          &slot_config, &mount_config, &s_card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card (%s)", esp_err_to_name(ret));
    s_card = NULL;
    return ret;
  }

  sdmmc_card_print_info(stdout, s_card);
  return ESP_OK;
}

void storage_unmount(void) {
  if (!s_card) {
    return;
  }
  esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_card);
  s_card = NULL;
}

bool storage_is_mounted(void) { return s_card != NULL; }
```

`components/dap_hw/audio_out.h`:

```c
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// I2S output to the PCM5102A DAC.

typedef struct {
  int bck;  // bit clock
  int lrck; // left/right (word select) clock
  int dout; // data, to the DAC's DIN
} audio_out_pins_t;

// Record which pins to use. Call once before anything else.
void audio_out_init(const audio_out_pins_t *pins);

// Configure the link for a stream and start it. `bits` is 16, 24 or 32;
// `channels` is 1 or 2. Returns ESP_ERR_INVALID_STATE if output is already
// running; call audio_out_stop() first.
esp_err_t audio_out_start(uint32_t sample_rate, int channels, int bits);

// Send PCM to the DAC. Blocks until all of it has been queued.
esp_err_t audio_out_write(const void *data, size_t len);

// Play out what is queued, then shut the link down. Does nothing if output is
// not running.
void audio_out_stop(void);
```

`components/dap_hw/audio_out.c`:

```c
#include "audio_out.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static i2s_chan_handle_t s_tx;
static audio_out_pins_t s_pins;
static bool s_have_pins;
static bool s_running;

void audio_out_init(const audio_out_pins_t *pins) {
  s_pins = *pins;
  s_have_pins = true;
}

esp_err_t audio_out_start(uint32_t sample_rate, int channels, int bits) {
  if (!s_have_pins || s_running) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_data_bit_width_t width = (bits == 24)   ? I2S_DATA_BIT_WIDTH_24BIT
                               : (bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT
                                              : I2S_DATA_BIT_WIDTH_16BIT;
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      width, (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);

  esp_err_t err;
  if (!s_tx) {
    // The channel is created once and kept. Its pins stay driven between
    // tracks, which keeps the DAC's inputs from floating.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // The default (6 x 240 frames) is only ~32ms of audio — about as long as
    // one SD card read, so the buffer runs dry between reads and every gap
    // clicks. 8 x 1000 frames gives ~180ms to coast on. 1000 is near the
    // per-descriptor ceiling: a descriptor holds at most 4092 bytes, and a
    // 16-bit stereo frame is 4 bytes.
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1000;
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
      s_tx = NULL;
      return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED, // PCM5102A makes its own clock
                .bclk = (gpio_num_t)s_pins.bck,
                .ws = (gpio_num_t)s_pins.lrck,
                .dout = (gpio_num_t)s_pins.dout,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {0},
            },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
      i2s_del_channel(s_tx);
      s_tx = NULL;
      return err;
    }
  } else {
    // Already created by an earlier start: retune it for this stream.
    err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
    if (err != ESP_OK) {
      return err;
    }
    err = i2s_channel_reconfig_std_slot(s_tx, &slot_cfg);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = i2s_channel_enable(s_tx);
  if (err != ESP_OK) {
    return err;
  }
  s_running = true;
  return ESP_OK;
}

esp_err_t audio_out_write(const void *data, size_t len) {
  if (!s_running) {
    return ESP_ERR_INVALID_STATE;
  }
  size_t written;
  return i2s_channel_write(s_tx, data, len, &written, portMAX_DELAY);
}

// Stop cleanly. Leaving the channel enabled with nothing left to send makes
// the DMA replay stale buffers, which is audible as a continuous buzz.
void audio_out_stop(void) {
  if (!s_running) {
    return;
  }
  static const int16_t silence[256 * 2] = {0};
  size_t written;
  for (int i = 0; i < 4; i++) {
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // let the last buffers drain
  i2s_channel_disable(s_tx);
  s_running = false;
}
```

- [ ] **Step 4: Create the player**

`main/player.h`:

```c
#pragma once

#include "esp_err.h"

// Register the audio decoders. Call once at start-up.
void player_init(void);

// Decode a FLAC file and play it through audio_out. Blocks until the track
// ends or fails. Must run on a task with at least 16KB of stack: the FLAC
// decoder overflows smaller ones.
esp_err_t player_play_file(const char *path);
```

`main/player.c`:

```c
#include "player.h"

#include "audio_out.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define FLAC_IN_CHUNK 32768       // must exceed the largest FLAC frame (~10KB here)
#define FLAC_OUT_INIT (32 * 1024) // starting PCM buffer; grown if a block needs more

static const char *TAG = "player";

void player_init(void) {
  esp_audio_dec_register_default();        // the codecs themselves
  esp_audio_simple_dec_register_default(); // the container parsers on top
}

// Read the file in chunks, hand them to the decoder, push the PCM it returns
// straight at the DAC. The I2S link is set up lazily, once the decoder has
// told us the file's real sample rate and channel count.
esp_err_t player_play_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot open %s", path);
    return ESP_ERR_NOT_FOUND;
  }

  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
      .dec_cfg = NULL,
      .cfg_size = 0,
      .use_frame_dec = false,
  };
  esp_audio_simple_dec_handle_t dec = NULL;
  if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open the FLAC decoder");
    fclose(f);
    return ESP_FAIL;
  }

  // Everything the cleanup block touches must be declared before the first
  // goto, or it reads an uninitialised value.
  esp_err_t result = ESP_FAIL;
  bool started = false;
  bool eof = false;
  uint32_t frames_out = 0;

  // The input buffer receives data straight from the SD card, so it must be
  // internal DMA-capable RAM: the ESP32's SDMMC DMA cannot reach PSRAM.
  // The decoded PCM never touches DMA, so that one can live in PSRAM.
  uint8_t *in =
      heap_caps_malloc(FLAC_IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  uint32_t out_len = FLAC_OUT_INIT;
  uint8_t *out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!in || !out) {
    ESP_LOGE(TAG, "Out of memory for decode buffers");
    result = ESP_ERR_NO_MEM;
    goto done;
  }

  // Follow Espressif's reference loop: read a chunk, feed it until fully
  // consumed, then read the next. The decoder caches partial frames itself,
  // and it keeps pointers into this buffer between calls — so nothing may
  // move or overwrite the buffer while raw.len is still non-zero.
  while (!eof) {
    size_t n = fread(in, 1, FLAC_IN_CHUNK, f);
    if (n == 0) {
      break;
    }
    eof = (n < FLAC_IN_CHUNK);
    esp_audio_simple_dec_raw_t raw = {
        .buffer = in,
        .len = (uint32_t)n,
        .eos = eof, // FLAC needs this to flush its last frame
    };

    while (raw.len > 0) {
      esp_audio_simple_dec_out_t frame = {.buffer = out, .len = out_len};
      esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &frame);

      if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        // A FLAC block was bigger than our buffer; grow and retry.
        heap_caps_free(out);
        out_len = frame.needed_size;
        out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
          ESP_LOGE(TAG, "Out of memory growing to %" PRIu32, out_len);
          result = ESP_ERR_NO_MEM;
          goto done;
        }
        continue;
      }
      if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decode failed: %d at offset %ld (unconsumed=%u)",
                 (int)err, ftell(f), (unsigned)raw.len);
        goto done;
      }

      if (frame.decoded_size > 0) {
        if (!started) {
          esp_audio_simple_dec_info_t info;
          esp_audio_simple_dec_get_info(dec, &info);
          ESP_LOGI(TAG, "FLAC: %" PRIu32 " Hz, %d channels, %d bits",
                   info.sample_rate, info.channel, info.bits_per_sample);
          esp_err_t start_err = audio_out_start(info.sample_rate, info.channel,
                                                info.bits_per_sample);
          if (start_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start audio output (%s)",
                     esp_err_to_name(start_err));
            result = start_err;
            goto done;
          }
          started = true;
        }
        esp_err_t write_err = audio_out_write(out, frame.decoded_size);
        if (write_err != ESP_OK) {
          ESP_LOGE(TAG, "Audio write failed (%s)", esp_err_to_name(write_err));
          result = write_err;
          goto done;
        }
        frames_out++;
      }

      // Guard against spinning forever if the decoder stops making progress.
      if (raw.consumed == 0) {
        break;
      }
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
    }
  }
  ESP_LOGI(TAG, "Finished: %" PRIu32 " blocks decoded", frames_out);
  result = ESP_OK;

done:
  if (started) {
    audio_out_stop();
  }
  esp_audio_simple_dec_close(dec);
  heap_caps_free(in);
  heap_caps_free(out);
  fclose(f);
  return result;
}
```

- [ ] **Step 5: Create `main/app_main.c`**

```c
#include "audio_out.h"
#include "esp_log.h"
#include "pins.h"
#include "player.h"
#include "storage.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "dap";

// Find the first .flac in one directory.
static bool find_flac_in(const char *dir_path, char *out, size_t out_len) {
  DIR *dir = opendir(dir_path);
  if (!dir) {
    return false;
  }
  struct dirent *ent;
  bool found = false;
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (dot && strcasecmp(dot, ".flac") == 0) {
      snprintf(out, out_len, "%s/%s", dir_path, ent->d_name);
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Look in the music folder first, then the top level of the card.
static bool find_flac(char *out, size_t out_len) {
  return find_flac_in(STORAGE_MOUNT_POINT "/music", out, out_len) ||
         find_flac_in(STORAGE_MOUNT_POINT, out, out_len);
}

void app_main(void) {
  if (storage_mount() != ESP_OK) {
    ESP_LOGE(TAG, "No usable SD card; nothing to play");
    return;
  }

  const audio_out_pins_t pins = {
      .bck = PIN_I2S_BCK,
      .lrck = PIN_I2S_LRCK,
      .dout = PIN_I2S_DOUT,
  };
  audio_out_init(&pins);
  player_init();

  // Play the first FLAC we find. The card stays mounted afterwards.
  char track[320];
  if (!find_flac(track, sizeof(track))) {
    ESP_LOGW(TAG, "No .flac found in " STORAGE_MOUNT_POINT "/music");
    return;
  }
  ESP_LOGI(TAG, "Playing %s", track);
  player_play_file(track);
}
```

Both early returns (no card, no FLAC) leave the main task to exit normally. Neither may call `abort`, `ESP_ERROR_CHECK` or restart the chip.

- [ ] **Step 6: Update the build files**

Replace `main/CMakeLists.txt` with:

```cmake
idf_component_register(SRCS "app_main.c" "player.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_hw dap_core)
```

(`espressif/esp_audio_codec` is pulled in by `main/idf_component.yml`, which stays as it is.)

In the top-level `CMakeLists.txt`, change `project(sd_card)` to `project(dap)`.

In `sdkconfig.defaults`, delete every line that starts with `CONFIG_EXAMPLE_`. Their Kconfig file is gone.

- [ ] **Step 7: Write the new `README.md`**

````markdown
# ESP32 Digital Audio Player

A standalone music player for the Freenove ESP32-WROVER CAM board: FLAC and
MP3 from an SD card, a PCM5102A I2S DAC, a 16x2 LCD and a 4x4 keypad.

- Design: `docs/superpowers/specs/2026-09-30-dap-design.md`
- Stage plans: `docs/superpowers/plans/`
- Pin assignments: `main/pins.h`
- The ESP-IDF example this grew from: `reference/`

## Build and flash

```bash
source /opt/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

`sdkconfig` is generated from `sdkconfig.defaults`; change settings there.

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
````

- [ ] **Step 8: Clean build and check the configuration**

The project name and component set changed, so rebuild from scratch:

```bash
source /opt/esp-idf/export.sh
cp sdkconfig sdkconfig.bak-pre-restructure
idf.py fullclean
rm sdkconfig
idf.py build
diff <(grep -E '^CONFIG_' sdkconfig.bak-pre-restructure | sort) <(grep -E '^CONFIG_' sdkconfig | sort)
```

Expected: `Project build complete.` with no compiler warnings from `main/`, `components/dap_hw/` or `components/dap_core/`. The diff shows only `<` lines beginning `CONFIG_EXAMPLE_`. Any other difference: add the old value to `sdkconfig.defaults`, `rm sdkconfig`, and rebuild.

Run: `grep -rnE "GPIO_NUM_[0-9]+|\b(gpio|pin)\w* *= *[0-9]+" main components --include=*.c --include=*.h | grep -v "main/pins.h"`
Expected: no output (pin numbers live only in `pins.h`).

Run: `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 9: Flash and verify behaviour matches the baseline**

```bash
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 900 "blocks decoded" > /tmp/stage1-task4.log; echo "capture exit: $?"' | newgrp uucp
diff <(grep -oE "(Playing .*|FLAC: .*|Finished: .*)" docs/superpowers/plans/stage1-baseline.log) <(grep -oE "(Playing .*|FLAC: .*|Finished: .*)" /tmp/stage1-task4.log) && echo SAME
grep -cE "Decode failed|Guru Meditation|abort\(\)|Backtrace" /tmp/stage1-task4.log
```

(Run the capture in the background and wait; it lasts one full track.)
Expected: `capture exit: 0`, `SAME`, and a count of `0`.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "Split the SD/FLAC example into storage, audio_out and player

The original example moves to reference/ unchanged. Behaviour is the
same: mount the card and play the first FLAC found. Example-only steps
(test file write, directory listing, pin debug) are not carried over.

audio_out now creates its I2S channel once and retunes it on later
starts, so a second track no longer fails on a channel that already
exists.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: FLAC seek feasibility test

**Files:**
- Create (branch `spike/flac-seek` only): `main/spike_flac_seek.c`, `main/spike_flac_seek.h`
- Modify (branch only): `main/app_main.c`, `main/CMakeLists.txt`
- Modify (branch `main`): `docs/superpowers/specs/2026-09-30-dap-design.md` section 4 "Seeking"
- Create (branch `main`): `docs/superpowers/plans/stage1-flac-seek.log`

**Interfaces:**
- Consumes: `storage`, `audio_out`, `player_init()` from Task 4; `tools/serial_capture.py`.
- Produces: a recorded answer in the spec to "can FLAC playback start from the middle of a file, and how?". Stage 3 and stage 8 are planned from it. The spike code is throwaway and is never merged.

The question: the decoder library documents that, in the mode we use (its own parser, `use_frame_dec = false`), playback after `esp_audio_simple_dec_reset()` "must start from the beginning of the audio stream". This test finds out what actually happens, and whether replaying the file's header first makes a mid-file jump work.

Four methods, each tried at 50% and 90% of the way through the file:

| Method | Decoder handle | Header replayed before the jump |
|---|---|---|
| A | reset | no |
| B | reset | yes |
| C | closed and reopened | no |
| D | closed and reopened | yes |

A method **passes** at a position if, after the jump: `errors == 0`, `blocks == 60`, `bytes_to_first_pcm < 262144`, and the reported sample rate, channel count and bit depth equal those seen at the start of the file.

- [ ] **Step 1: Create the branch**

```bash
git checkout -b spike/flac-seek
```

- [ ] **Step 2: Write the spike**

`main/spike_flac_seek.h`:

```c
#pragma once

// Throwaway feasibility test. Plays the start of a FLAC, jumps to the middle
// by several methods, and logs one SEEK_RESULT line per attempt.
void spike_flac_seek_run(const char *path);
```

`main/spike_flac_seek.c`:

```c
#include "spike_flac_seek.h"

#include "audio_out.h"
#include "esp_audio_simple_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define IN_CHUNK 32768
#define OUT_INIT (64 * 1024)
#define WARMUP_BLOCKS 20 // played from the start before each jump
#define SEEK_BLOCKS 60   // played after each jump
#define MAX_ERRORS 20

static const char *TAG = "spike";

typedef struct {
  int blocks;
  int errors;
  int first_err;
  long bytes_to_first_pcm; // input consumed before the first PCM came out
  uint32_t rate;
  int channels;
  int bits;
} result_t;

static uint8_t *s_in;
static uint8_t *s_out;
static uint32_t s_out_len;

// Byte offset of the first audio frame: the "fLaC" marker plus every metadata
// block. Returns -1 if this is not a plain FLAC file.
static long flac_audio_start(FILE *f) {
  uint8_t h[4];
  fseek(f, 0, SEEK_SET);
  if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4) != 0) {
    return -1;
  }
  for (;;) {
    if (fread(h, 1, 4, f) != 4) {
      return -1;
    }
    long len = ((long)h[1] << 16) | ((long)h[2] << 8) | (long)h[3];
    if (fseek(f, len, SEEK_CUR) != 0) {
      return -1;
    }
    if (h[0] & 0x80) { // last-metadata-block flag
      return ftell(f);
    }
  }
}

static esp_audio_simple_dec_handle_t open_decoder(void) {
  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
      .use_frame_dec = false,
  };
  esp_audio_simple_dec_handle_t dec = NULL;
  if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "decoder open failed");
    return NULL;
  }
  return dec;
}

// Feed the decoder from the file's current position and play what comes out.
// Stops after `byte_limit` input bytes (if >= 0), after `max_blocks` PCM
// blocks (if > 0), at end of file, or after MAX_ERRORS decode errors.
static void feed(esp_audio_simple_dec_handle_t dec, FILE *f, long byte_limit,
                 int max_blocks, result_t *r) {
  bool playing = false;
  long fed = 0;
  bool stop = false;

  while (!stop) {
    size_t want = IN_CHUNK;
    if (byte_limit >= 0) {
      if (fed >= byte_limit) {
        break;
      }
      if ((long)want > byte_limit - fed) {
        want = (size_t)(byte_limit - fed);
      }
    }
    size_t n = fread(s_in, 1, want, f);
    if (n == 0) {
      break;
    }
    esp_audio_simple_dec_raw_t raw = {
        .buffer = s_in,
        .len = (uint32_t)n,
        .eos = false,
    };

    while (raw.len > 0) {
      esp_audio_simple_dec_out_t frame = {.buffer = s_out, .len = s_out_len};
      esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &frame);

      if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        heap_caps_free(s_out);
        s_out_len = frame.needed_size;
        s_out = heap_caps_malloc(s_out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_out) {
          ESP_LOGE(TAG, "out of memory");
          stop = true;
          break;
        }
        continue;
      }
      if (err != ESP_AUDIO_ERR_OK) {
        if (r->errors == 0) {
          r->first_err = (int)err;
        }
        r->errors++;
        if (r->errors >= MAX_ERRORS) {
          stop = true;
        }
        break; // drop the rest of this chunk and see whether it recovers
      }

      if (frame.decoded_size > 0) {
        if (r->blocks == 0) {
          esp_audio_simple_dec_info_t info;
          esp_audio_simple_dec_get_info(dec, &info);
          r->rate = info.sample_rate;
          r->channels = info.channel;
          r->bits = info.bits_per_sample;
          r->bytes_to_first_pcm = fed + (long)(raw.buffer - s_in) + raw.consumed;
        }
        if (!playing) {
          if (audio_out_start(r->rate, r->channels, r->bits) == ESP_OK) {
            playing = true;
          }
        }
        if (playing) {
          audio_out_write(s_out, frame.decoded_size);
        }
        r->blocks++;
        if (max_blocks > 0 && r->blocks >= max_blocks) {
          stop = true;
          break;
        }
      }

      if (raw.consumed == 0) {
        break;
      }
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
    }
    fed += (long)n;
  }

  if (playing) {
    audio_out_stop();
  }
}

static void run_method(const char *name, const char *path, bool reopen,
                       bool replay_header, int percent) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "cannot open %s", path);
    return;
  }
  long audio_start = flac_audio_start(f);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (audio_start < 0) {
    ESP_LOGE(TAG, "not a plain FLAC file");
    fclose(f);
    return;
  }

  esp_audio_simple_dec_handle_t dec = open_decoder();
  if (!dec) {
    fclose(f);
    return;
  }

  // 1. Play from the start so the decoder is in a normal mid-stream state.
  result_t warm = {0};
  fseek(f, 0, SEEK_SET);
  feed(dec, f, -1, WARMUP_BLOCKS, &warm);

  // 2. Put the decoder back to its initial state.
  int reset_err = 0;
  if (reopen) {
    esp_audio_simple_dec_close(dec);
    dec = open_decoder();
    if (!dec) {
      fclose(f);
      return;
    }
  } else {
    reset_err = (int)esp_audio_simple_dec_reset(dec);
  }

  // 3. Optionally replay the header so the parser has the stream parameters.
  result_t r = {0};
  if (replay_header) {
    fseek(f, 0, SEEK_SET);
    feed(dec, f, audio_start, 0, &r);
  }
  int header_blocks = r.blocks;
  int header_errors = r.errors;

  // 4. Jump and play.
  long target = audio_start + (long)(((int64_t)(size - audio_start) * percent) / 100);
  fseek(f, target, SEEK_SET);
  r = (result_t){0};
  feed(dec, f, -1, SEEK_BLOCKS, &r);

  bool pass = r.errors == 0 && r.blocks == SEEK_BLOCKS &&
              r.bytes_to_first_pcm < 262144 && r.rate == warm.rate &&
              r.channels == warm.channels && r.bits == warm.bits;
  ESP_LOGI(TAG,
           "SEEK_RESULT method=%s percent=%d pass=%d blocks=%d errors=%d "
           "first_err=%d bytes_to_first_pcm=%ld rate=%" PRIu32
           " ch=%d bits=%d reset_err=%d header_blocks=%d header_errors=%d "
           "warm_rate=%" PRIu32 " warm_ch=%d warm_bits=%d",
           name, percent, pass, r.blocks, r.errors, r.first_err,
           r.bytes_to_first_pcm, r.rate, r.channels, r.bits, reset_err,
           header_blocks, header_errors, warm.rate, warm.channels, warm.bits);

  esp_audio_simple_dec_close(dec);
  fclose(f);
}

void spike_flac_seek_run(const char *path) {
  s_in = heap_caps_malloc(IN_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  s_out_len = OUT_INIT;
  s_out = heap_caps_malloc(s_out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_in || !s_out) {
    ESP_LOGE(TAG, "out of memory");
    return;
  }

  static const int percents[] = {50, 90};
  for (int i = 0; i < 2; i++) {
    run_method("A", path, false, false, percents[i]);
    run_method("B", path, false, true, percents[i]);
    run_method("C", path, true, false, percents[i]);
    run_method("D", path, true, true, percents[i]);
  }
  ESP_LOGI(TAG, "SEEK_TEST_DONE");
}
```

In `main/app_main.c`, add `#include "spike_flac_seek.h"` and replace the final `player_play_file(track);` with `spike_flac_seek_run(track);`.

In `main/CMakeLists.txt`, add `"spike_flac_seek.c"` to `SRCS`.

- [ ] **Step 3: Build, flash, capture**

```bash
source /opt/esp-idf/export.sh && idf.py build
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 300 "SEEK_TEST_DONE" > /tmp/stage1-flac-seek.log; echo "capture exit: $?"' | newgrp uucp
grep -oE "SEEK_RESULT .*" /tmp/stage1-flac-seek.log
grep -cE "Guru Meditation|abort\(\)|Backtrace" /tmp/stage1-flac-seek.log
```

Expected: `capture exit: 0`, eight `SEEK_RESULT` lines (methods A–D at 50 and 90), and a crash count of `0`. Every line's `warm_rate`, `warm_ch`, `warm_bits` must match the baseline's `FLAC:` line; if they do not, the spike itself is broken and must be fixed before its results mean anything.

A method that crashes the firmware is a result, not a blocker: record it as "crashes", remove that method from `spike_flac_seek_run`, and rerun for the rest.

- [ ] **Step 4: Commit the spike on its branch**

```bash
git add main/spike_flac_seek.c main/spike_flac_seek.h main/app_main.c main/CMakeLists.txt
git commit -m "Spike: FLAC mid-file seek feasibility test (throwaway, do not merge)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Record the result on `main`**

```bash
git checkout main
cp /tmp/stage1-flac-seek.log docs/superpowers/plans/stage1-flac-seek.log
```

In `docs/superpowers/specs/2026-09-30-dap-design.md`, replace the body of the `### Seeking` subsection of section 4 (everything from the line after the heading up to, but not including, `## 5. Tags`) with a section of this shape, filled in from the eight `SEEK_RESULT` lines:

```markdown
`SEEK_REL` and a non-zero `start_ms` seek by byte offset: the target position
is `audio_start + (file_size - audio_start) * target_ms / duration_ms`, so the
landing point is approximate for variable-bitrate audio. Position reporting
restarts from `target_ms`.

**FLAC (tested 2026-09-30, stage 1, branch `spike/flac-seek`, log in
`docs/superpowers/plans/stage1-flac-seek.log`):**

| Method | 50% | 90% |
|---|---|---|
| A: reset, jump | <pass / fail: errors=N first_err=E blocks=N> | <...> |
| B: reset, replay header, jump | <...> | <...> |
| C: reopen, jump | <...> | <...> |
| D: reopen, replay header, jump | <...> | <...> |

**Decision:** <one of the three paragraphs below, verbatim, with the method letter filled in>

**MP3:** not yet tested; the MP3 parser resynchronises on frame headers, so a
byte-offset jump after `esp_audio_simple_dec_reset()` is expected to work.
Stage 3 verifies it when MP3 playback is added.
```

Choose the decision paragraph by this rule, using the first method in the order A, B, C, D that passes at **both** positions:

- A method passed: `FLAC seeks using method <X> (<its description from the table>). Stage 3 implements SEEK_REL and start_ms for FLAC this way; stage 8 adds fast-forward, rewind and mid-track resume.`
- No method passed at both positions: `FLAC cannot be seeked with the decoder's built-in parser. FLAC tracks ignore SEEK_REL and always start at 0; fast-forward, rewind and mid-track resume apply to MP3 only. A possible later route is the decoder's frame mode (use_frame_dec = true) with our own FLAC frame finder.`

Every `<...>` in the template must be replaced with measured values. No angle-bracket text may remain in the spec.

- [ ] **Step 6: Restore the main firmware on the board**

The board is still running the spike. Put the real firmware back:

```bash
source /opt/esp-idf/export.sh && idf.py build
echo 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 flash' | newgrp uucp
echo 'source /opt/esp-idf/export.sh && python3 tools/serial_capture.py /dev/ttyUSB0 30 "FLAC: "; echo "capture exit: $?"' | newgrp uucp
```

Expected: `capture exit: 0` (the track has started playing from the main firmware).

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/specs/2026-09-30-dap-design.md docs/superpowers/plans/stage1-flac-seek.log
git commit -m "Record FLAC seek feasibility result in the spec

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Stage 1 hardware check (run by the user)

Agents can read the serial log but cannot hear the output. After Task 5:

1. With the main firmware on the board, reset it and listen to the track. It should sound exactly as it did before stage 1: no clicks, no buzz after the track ends.
2. If a FLAC seek method passed, the coordinator flashes the `spike/flac-seek` build once more. Each attempt plays about two seconds from the start of the track, then about five seconds from the middle. Listen at the jumps for the passing method: a clean cut is a pass; a loud burst of noise at the jump is a fail, and the spec's decision changes to "cannot be seeked" for that method.

Stage 2 planning starts once both are confirmed.
