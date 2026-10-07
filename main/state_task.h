#pragma once

#include "esp_err.h"
#include "library.h"
#include "play_queue.h"
#include "standby.h"
#include <stdbool.h>
#include <stdint.h>

// What the player remembers across a power-off, and the one task that writes
// it: the settings and the place in the current track go to flash (NVS), the
// queue to a file on the card. The UI task owns all of these while the
// player runs; it hands this task a copy whenever one changes and carries
// on. How the hand-over works, and what a power cut during a save leaves
// behind, is described at the top of state_task.c.
//
// THE RULE FOR CALLERS
//
// - Every state_task_save_* call returns without waiting for anything: it
//   copies what it is given, wakes the task, and never touches flash or the
//   card. The caller's own copy may change the moment the call returns. The
//   most a call costs is given with each.
// - Call a save whenever the thing may have changed; calling with what was
//   handed over last costs a comparison and nothing else (nothing is written,
//   the task is not woken). So the caller need not work out what changed.
// - Changes are not written at once. The task waits until STATE_SETTLE_MS
//   have gone by with no further hand-over (a held volume key, a burst of
//   "Add to queue", is then one save), but never longer than
//   STATE_DEFER_MAX_MS after the first one. The latest of everything handed
//   over is what gets written; a power cut before that loses those last
//   changes and nothing else.
// - All of it may be called before state_task_start, or after it failed:
//   the saves then do nothing, the loaded values are the defaults and there
//   is no queue to restore.
//
// WHO IS BELIEVED AT POWER-ON
//
// Three things are saved in two places, and a power cut can leave them out
// of step, so at power-on the caller puts them together in this order:
//
//   1. state_task_restore_queue: the queue as the file has it. The file
//      also holds shuffle, loop and a position (it is play_queue_save's
//      format), and the restored queue has the file's.
//   2. The position: state_task_loaded_position gives the queue position,
//      the track number that was at that position, and the time into it.
//      If position < the restored queue's count and play[position] is that
//      track number, play_queue_set_position(position).
//   3. Shuffle and loop: flash is the authority. play_queue_set_loop and
//      play_queue_set_shuffle with the values from
//      state_task_loaded_settings, after 2 (changing shuffle moves the
//      position).
//   4. The time: if 2 placed the track, the saved time is used. If it did
//      not, the queue is looked at again now that 3 is done (flash can be
//      one shuffle change ahead of the file): if the queue's position is
//      the saved position and its current track the saved track, the saved
//      time is used.
//   5. If neither placed the saved track (flash and the file are from
//      different moments), it is looked for in the play order as 3 left it,
//      once: if a position was saved, the track number is not the 65535 of
//      a position no queue has (the UI saves that one when the queue is
//      emptied) and is below the index's track count, its first place is
//      made current with play_queue_set_position and the saved time is
//      used. The file's position is only as recent as the last change to
//      what the queue holds, which is why it is the last resort: only if
//      the queue does not have the track does it stand, and the track there
//      starts from its beginning.
//
// The volume and the backlight setting are only in flash.

// The longest queue that is saved, in tracks: the size of this task's copies.
// The UI's queue has this capacity or less.
#define STATE_QUEUE_CAPACITY 8192u

#define STATE_SETTLE_MS 1000    // quiet time after the last change
#define STATE_DEFER_MAX_MS 5000 // the longest a change waits to be written

typedef struct {
  int volume; // 0..PCM_VOLUME_MAX
  bool shuffle;
  play_loop_t loop;
  standby_backlight_t backlight; // how long the backlight stays on
} state_settings_t;

// Where playback was. `position` is the place in the queue's play order,
// `track` the track number that was there, `elapsed_ms` how far into it.
typedef struct {
  uint32_t position;
  uint32_t track;
  uint32_t elapsed_ms;
} state_position_t;

// Open flash, read what was saved, allocate the queue copies and create the
// task (priority 2, core 0). Call once, before the UI task is started and
// before anything else here. It reads flash, which takes about 15 ms
// (measured; 77 ms, once, when the saved-state area cannot be read and has
// to be erased: that is logged, and the defaults are used).
//
// Never fatal. If flash cannot be used, settings and position are not saved
// (ESP_FAIL, logged); if there is no memory for the copies or the task,
// nothing is (ESP_ERR_NO_MEM, logged). The player runs either way.
esp_err_t state_task_start(void);

// --- Settings: volume, shuffle, loop, backlight -----------------------------

// What was in flash at start-up; the default (volume 10, shuffle off, loop
// off, backlight 30 s) for each value that was not there or was out of
// range. Never blocks.
void state_task_loaded_settings(state_settings_t *out);

// Hand over the settings as they now are. Costs a few instructions in a
// critical section. A value that is already in flash is not written again.
void state_task_save_settings(const state_settings_t *settings);

// --- Position --------------------------------------------------------------

// What was in flash at start-up. False if no position was saved. See WHO IS
// BELIEVED: check `track` against the restored queue before using it.
bool state_task_loaded_position(state_position_t *out);

// Hand over the place playback has reached: on a track change, on pause and
// every 30 seconds while playing. The three values are saved as one item, so
// a power cut leaves the old three or the new three, never a mixture. Costs
// a few instructions in a critical section. A position or a track number
// above 65535 is not one a queue can have and is ignored.
void state_task_save_position(const state_position_t *position);

// --- The queue -------------------------------------------------------------

// Once the card is mounted and BEFORE library_task_boot: read the saved
// queue file into memory, so that state_task_restore_queue has it when the
// UI first sees an index. Returns when that is done (some tens of
// milliseconds: the card is read), so call it from a task that has nothing
// else to do. Call once.
void state_task_card_ready(void);

// Put the queue that was found on the card at start-up into `q`. `lib` is
// the index now loaded (the caller holds it: library_task_get). True if the
// file was whole and was saved for exactly this index; `q` is then as it was
// saved, shuffle, loop and position included (see WHO IS BELIEVED). False
// with `q` empty otherwise: no file, a damaged one, one saved for another
// index, or `lib` is NULL.
//
// This is for power-on, and works once: the file is the one found at
// start-up, and it is given up at the first call made with an index, and at
// the first state_task_save_queue. After that the answer is false. (With
// `lib` NULL nothing is given up: ask again when there is an index.)
//
// Reads memory only, never the card: the file's bytes are checked and
// copied (measured: 18 ms for a full queue of 8,192 tracks, 1.7 ms for
// 1,028; once per power-on). It opens a stream over those bytes, which is
// one small allocation, freed before it returns.
bool state_task_restore_queue(play_queue_t *q, const library_t *lib);

// Hand over the queue as it now is, and the checksum (library_checksum) of
// the index its track numbers belong to. Call when what the queue holds has
// changed: its tracks, their order, shuffle, or the index. A move to another
// position alone needs no call (it costs a pass over the queue for nothing):
// the position is saved in flash, by state_task_save_position, and the
// position in the file is only what stands when the queue does not have
// flash's track (see WHO IS BELIEVED).
//
// The call compares `q` with what was handed over last and copies it only
// if the two orders, the count, shuffle or the index differ; only then is
// the file rewritten. A change of position or loop alone is noted (the next
// file written carries it) but writes nothing: flash is where those are
// saved. The copy is complete before the call returns, and the state task
// never writes the file from a copy this call can still change, so what is
// written is always one queue as it was at one call, never a mixture of two.
//
// Costs one pass over the queue to compare it and, if it differs, one to
// copy it; the lock it takes is never held by the state task for more than
// a few instructions. The copies are in PSRAM, so for a full queue of 8,192
// tracks that is 2 to 14 ms (measured while a track plays), and in
// proportion for a shorter one: 0.5 ms for 1,028 tracks. A queue longer
// than STATE_QUEUE_CAPACITY is not saved.
void state_task_save_queue(const play_queue_t *q, uint32_t index_checksum);

// --- For the console -------------------------------------------------------

// What is saved right now, read back from flash and from the card.
typedef struct {
  bool flash; // flash can be used at all
  bool has_volume, has_shuffle, has_loop, has_backlight,
      has_position;                         // each is in flash
  uint8_t volume, shuffle, loop, backlight; // as stored, meaningful if has_*
  state_position_t position;

  bool file;           // /.dap/queue.bin exists
  bool file_unfinished; // so does the file of a save that was interrupted
  uint32_t file_bytes;
  uint32_t file_count; // tracks, and the index checksum it was saved for,
  uint32_t file_index; //   as its header says (0 if it has no whole header)
  bool index;          // an index is loaded
  uint32_t index_checksum;
  bool usable; // the file is whole and would be restored with that index

  uint32_t stack_free; // bytes of the state task's stack never used
} state_report_t;

// These two wait for the state task to do the work (flash and the card are
// read or written), so they are for the console task, one call at a time,
// never for the UI. False if there is no state task.
bool state_task_report(state_report_t *out);

// Erase everything that is saved: the settings and the position in flash,
// the queue file on the card. Changes handed over but not yet written are
// dropped too. What the running player holds is untouched and is saved
// again when it next changes.
bool state_task_clear(void);
