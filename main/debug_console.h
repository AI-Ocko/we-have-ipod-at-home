#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "screen.h"

// A line-based console on the serial port the log uses, so that the player
// can be driven and keys pressed from a script. Each line is one command and
// is answered with one log line, tag `dbg`: `ok <line>` or
// `err <line>: <why>`. A line ends with LF, CR or CRLF.
//
//   play <path> [start_ms]   player_play
//   pause | resume | stop    player_pause / player_resume / player_stop
//   vol <n>                  player_set_volume (any integer; the player clamps)
//   seek <signed ms>         player_seek_rel
//   key <char>               a press and a release of that key
//   long <char>              a press, a long press and a release
//   stat                     logs `stat state=... pos_ms=... vol=... fill=...
//                            low=...` and more (see player_status.h)
//   stall <ms>               test aid: the decoder task does nothing for that
//                            long (at most 30000), to force an underrun
//   screen                   logs what the LCD was last told to show, each
//                            row in quotes: `screen "<16 chars>" "<16 chars>"`.
//                            A user-defined character (a code below 0x20) or
//                            anything else that is not printable ASCII is
//                            shown as `?`.
//
// The path of `play` is everything after the command word, spaces included,
// with spaces at either end removed. If the last word of the line is made of
// digits only and something comes before it, that word is start_ms and not
// part of the path. So a path whose last word is a number (for example
// "/sdcard/live at leeds 1970") must be followed by a start position:
// "play /sdcard/live at leeds 1970 0".
//
// <char> is one of 123A456B789C*0#D (a to d are accepted for A to D).

// Start the console task. Key events go to `key_events` (items are
// key_event_t), the queue the keypad posts to. Call once.
esp_err_t debug_console_start(QueueHandle_t key_events);

// Tell the console what the LCD has just been told to show, for the `screen`
// command. The UI task calls this after each lcd_show; the console keeps its
// own copy, so it never looks at the UI task's. Takes about a microsecond,
// never blocks, and may be called before debug_console_start.
void debug_console_note_screen(const screen_t *shown);
