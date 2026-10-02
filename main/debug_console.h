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
//   scan                     start a scan of the card (library_task_rescan).
//                            `ok` means it has started; its progress and
//                            result are logged under the tag `library`.
//   lib ...                  what is in the library index; see below.
//
// THE `lib` COMMANDS
//
// Each prints one line per item, tag `dbg`, in exactly these forms (text is
// in double quotes and printed as stored; numbers are decimal):
//
//   lib                      lib tracks=<n> albums=<n> artists=<n> bytes=<n>
//   lib artists [from]       artist <#> "<name>" albums=<n> tracks=<n>
//   lib albums <artist#> [from]
//                            album <#> "<name>" artist=<#> tracks=<n>
//   lib tracks <album#> [from]
//   lib songs <from> <count>
//   lib find <path>          track <#> "<title>" artist="<artist>" album=<#>
//                              no=<disc>.<track> ms=<length> path="<path>"
//                            (all on one line)
//
// <#> is the item's number in the index: artists are numbered A to Z, albums
// by artist then name, tracks by artist, album, disc, track number. `lib
// songs` lists tracks in title order, A to Z over the whole library, from
// position <from>; the other lists are in number order. `lib find` takes a
// path as the index stores it, relative to the card and without a leading
// slash ("music/a.flac"; letter case is ignored), and prints that track or
// answers `err ...: not in the library`. `path` in a track line is that same
// form: put "/sdcard/" in front of it to `play` it.
//
// A listing prints at most 50 items; `from` (0 if left out) is where in the
// list to start, so a longer one is read in pieces. Every listing ends with
//
//   lib listed=<n> from=<from> of=<total>[ capped]
//
// where `capped` is there if the limit of 50 is what ended it. With no
// library (none loaded yet, a scan is running, or the last scan failed)
// every `lib` command answers `err lib...: no library (...)`.
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
