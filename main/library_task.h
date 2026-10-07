#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "library.h"
#include <stdbool.h>
#include <stdint.h>

// The music library on the device: one task that loads the index from the
// card at start-up, scans the card when there is no usable index or when
// asked to, and saves what it finds. How it works inside, and what a power
// cut during a save leaves behind, is described at the top of library_task.c.
//
// The index lives in one block of PSRAM (the arena) that is allocated once
// and reused by every load and every scan. So a rescan destroys the index
// that was there, and anything that reads the index has to follow the rule
// below.
//
// THE RULE FOR READERS
//
//   const library_t *lib = library_task_get();
//   if (lib != NULL) {
//     ... ask the index things (library.h) ...
//     library_task_put();
//   }
//
// - library_task_get never blocks. It returns NULL when there is no index:
//   none has been loaded yet, a scan is running, or the last scan failed.
//   A reader must cope with that every time it asks.
// - Every get that returned an index is followed by one library_task_put,
//   in the same task, soon: within one screen redraw, one key press, one
//   console command. A scan waits for the last put before it touches the
//   arena, so holding on stalls the scan (and it logs a complaint every few
//   seconds while it waits).
// - After the put, the pointer and every string that came out of the index
//   (library_artist_name, library_track's title and artist) are dead. Keep
//   numbers and copies of text, never pointers; and fetch the index afresh
//   next time. Track, album and artist numbers mean something only within
//   one index: after a scan they refer to other things (the status's
//   `generation` says when that has happened), so anything remembered
//   across a put is remembered as a path and looked up again.
// - The index is published only whole: a load that passed its checks, or a
//   scan that has finished building. Between a get and its put it does not
//   change.
//
// Any number of tasks may read at once.

#define LIBRARY_ARENA_BYTES 1572864u // 1.5 MB, the cap on the index

// Where the index is kept on the card.
#define LIBRARY_DIR "/.dap"
#define LIBRARY_INDEX_NAME "library.idx"

// What the library task is doing, for the screen. The latest of these is
// kept in the queue given to library_task_start: a queue of length one that
// is overwritten, so it never fills, never holds the scan up, and whoever
// reads it gets the newest state however many it missed.
typedef struct {
  bool scanning;       // a scan is running: show its progress
  bool full;           // the index is a partial one: a scan stopped because
                       // the index was full, and this is what it made (or
                       // that same index loaded again after the card was
                       // out); false while scanning
  uint32_t tracks;     // found so far, or in the finished index
  uint32_t generation; // counts the indexes published since start-up
} library_status_t;

// Allocate the arena and create the task. `status` is a queue of length one
// holding library_status_t, or NULL. Call once, before anything else here;
// the card need not be mounted yet. If the arena cannot be had this logs it
// and returns ESP_ERR_NO_MEM: the player then runs without a library
// (library_task_get returns NULL for ever, library_task_boot returns at
// once, a rescan is refused).
esp_err_t library_task_start(QueueHandle_t status);

// Each time the card is mounted (at start-up, and again after it was pulled
// out and put back): load the saved index, or scan the card if there is none
// that can be used. Returns once the request is made; the status queue says
// when the new index is published. If the task is still busy with an
// earlier request it waits for it, in steps of 100 ms; so call it from a
// task that has nothing else to do (the one that mounts the card).
void library_task_boot(void);

// Start a scan of the whole card and return at once. Playback is stopped
// first. ESP_ERR_INVALID_STATE if a load or scan is already going on or the
// card is not mounted; ESP_ERR_NO_MEM if there is no arena.
esp_err_t library_task_rescan(void);

// See THE RULE FOR READERS.
const library_t *library_task_get(void);
void library_task_put(void);

// Whether a load or a scan is asked for or running: true from the moment a
// boot or a rescan claims the task until it has finished (published an
// index or given up). Never blocks. With no index and this false, the last
// scan failed (or there is no arena): no index is coming by itself.
bool library_task_busy(void);
