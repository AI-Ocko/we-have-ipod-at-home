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
//   play <path> [start_ms]   player_play. Also logs `play id=<n>`: the play
//                            number the track's events will carry. The
//                            track is not put in the play queue, and nothing
//                            follows it when it ends.
//   pause | resume           player_pause / player_resume. The UI is not
//                            told, so this pause does not save the time
//                            reached as one made with `key D` does.
//   stop                     ui_stop: the UI task stops the player, keeps the
//                            track as the one D starts again, and does not
//                            go on to the next track of the queue
//   vol <n>                  player_set_volume (any integer; the player clamps)
//                            Not saved: only a volume set with the keys is.
//   seek <signed ms>         player_seek_rel
//   key <char>               a press and a release of that key
//   long <char>              a press, a long press and a release
//   hold <char> <repeats>    a key held down: see below
//   stat                     logs `stat state=... pos_ms=... vol=... fill=...
//                            low=...` and more (see player_status.h), ending
//                            with the UI task's numbers (ui.h): `ui_stack_free=
//                            <bytes> ui_passes=<n> ui_lcd=<n>`
//   stall <ms>               test aid: the decoder task does nothing for that
//                            long (at most 30000), to force an underrun
//   screen                   logs what the LCD was last told to show, each
//                            row in quotes: `screen "<16 chars>" "<16 chars>"`.
//                            The UI's user-defined characters (ui.h) are
//                            printed as stand-ins: `>` play (also the key
//                            test's triangle), `=` pause, `#` `+` `-` a
//                            full, a half and an empty cell of the volume
//                            bar, and in the last column of Now Playing's
//                            bottom row `L` the loop-all mark and `1` the
//                            loop-one mark. Anything else that is not
//                            printable ASCII is shown as `?`. (A list's
//                            cursor is a real `>`: it is followed at once
//                            by the item, the play symbol by a space. The
//                            shuffle mark is a real `S`.)
//   scan                     start a scan of the card (library_task_rescan).
//                            `ok` means it has started; its progress and
//                            result are logged under the tag `library`.
//   lib ...                  what is in the library index; see below.
//   state                    what is saved for the next power-on; see below.
//   state clear              erase all of it: the settings and the position
//                            in flash, the queue file on the card. What the
//                            running player holds is not changed, and is
//                            saved again when it next changes.
//   queue [from [count]]     what the play queue holds; see below.
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
// THE `state` COMMAND
//
// Prints two lines, read back from flash and from the card at that moment
// (state_task.h):
//
//   state flash=<ok|unusable> volume=<n> shuffle=<0|1> loop=<0|1|2>
//     position=<n> track=<n> elapsed_ms=<n> stack_free=<bytes>
//                                                     (all on one line)
//   state queue_file=present bytes=<n> count=<n> index=<8 hex digits>
//     loaded_index=<8 hex digits> match=<yes|no> usable=<yes|no>
//     unfinished=<yes|no>                             (all on one line)
//
// A value that is not saved is printed as `-`. `position` is the place in
// the queue, `track` the track number that was there, `elapsed_ms` the time
// into it; `stack_free` is how much of the state task's stack has never
// been used. `count` and `index` are what the queue file's header says: how
// many tracks, and the checksum of the library index they belong to;
// `loaded_index` is the checksum of the index now loaded (`-` if none), and
// `match` whether the two are the same. `usable` is the real test: whether
// the file is whole and would be restored with the loaded index. `unfinished`
// says that the file of an interrupted save (queue.bin.tmp) is there too.
// With no queue file the second line is
//
//   state queue_file=absent loaded_index=<...> unfinished=<yes|no>
//
// Both commands wait for the state task, which may be in the middle of a
// save: up to a second or so.
//
// THE `queue` COMMAND
//
// `queue [from [count]]` prints the play queue as the UI task holds it at
// that moment (ui_queue_report), in exactly these forms:
//
//   queue count=<n> position=<n> shuffle=<0|1> loop=<0|1|2> index=<8 hex>
//   queue <place> track=<#> path="<path>"        one line per track listed
//   queue listed=<n> from=<place> of=<count>[ capped]
//
// `count` is how many tracks the queue has and `position` the current
// track's place in the play order, counting from 0 (0 for an empty queue).
// `loop` is 0 off, 1 all, 2 one. `index` is the checksum of the library
// index the queue's track numbers belong to (compare `loaded_index` of
// `state`).
//
// The tracks are listed in play order, which is the order they will play in
// (shuffled, if shuffle is on), from place <from>; with no <from>, from the
// current track, so that the first line is the track on Now Playing and
// the ones after it are what follows. <count> is how many to list: 10 if
// left out, at most 50 (`capped` says the limit of 50 is what ended the
// listing), so a longer queue is read in pieces. <#> is the track's number
// in the index, as in the `lib` lines, and `path` is in the same form as
// theirs; it is `?` if the index now loaded is not the queue's.
//
// The command waits for the UI task, which answers at the end of its next
// pass: `err queue: the UI task did not answer` after a second without one.
//
// The path of `play` is everything after the command word, spaces included,
// with spaces at either end removed. If the last word of the line is made of
// digits only and something comes before it, that word is start_ms and not
// part of the path. So a path whose last word is a number (for example
// "/sdcard/live at leeds 1970") must be followed by a start position:
// "play /sdcard/live at leeds 1970 0".
//
// <char> is one of 123A456B789C*0#D (a to d are accepted for A to D).
//
// THE `hold` COMMAND
//
// `hold <char> <repeats>` posts to the key queue, in this order, what the
// keypad posts while a key is held: one KEY_PRESS, one KEY_LONG, <repeats>
// KEY_REPEAT (0 to 2000) and one KEY_RELEASE, all for that key: <repeats> + 3
// events.
//
// They are posted back to back, with no delay between them. A key event
// carries no time, so the order is all the UI task can see of a held key;
// the real spacing (600 ms to the long press, then a repeat every 150 ms,
// every 50 ms once the key has been down 2 s) would only make 2000 repeats
// take 100 s. Back to back is also the hardest case for the UI task: events
// arrive as fast as it can take them.
//
// No event is dropped silently. The queue holds 16, so each event waits for
// room, up to 1 s. If the UI task leaves the queue full for that long the
// command stops posting repeats, still tries to post the release (waiting
// 1 s once more), and says what happened. It always logs
//
//   hold key=<char> repeats=<n> posted=<events posted> dropped=<not posted>
//
// and then answers `ok ...` if dropped is 0, otherwise
// `err ...: key queue full: events were dropped`. The other refusals post
// nothing: `no such key`, `no repeat count`, `expected one key and a repeat
// count`, `expected a repeat count, 0 to 2000`.
//
// While the command runs the console reads no other line; in practice the UI
// task takes each event as it is posted (it has the higher priority).

// Start the console task. Key events go to `key_events` (items are
// key_event_t), the queue the keypad posts to. Call once.
esp_err_t debug_console_start(QueueHandle_t key_events);

// Tell the console what the LCD has just been told to show, for the `screen`
// command. The UI task calls this after each lcd_show; the console keeps its
// own copy, so it never looks at the UI task's. Takes about a microsecond,
// never blocks, and may be called before debug_console_start.
void debug_console_note_screen(const screen_t *shown);
