#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "playlist.h"
#include "playlist_catalog.h"
#include <stdbool.h>
#include <stdint.h>

// The playlists in /playlists/ on the card, and the one task that reads and
// writes them. Nothing else touches that folder: the UI (and the console)
// hand this task requests and carry on, and are told how each ended by a
// small event. How it works inside, and what a power cut during a save
// leaves behind, is described at the top of playlist_task.c.
//
// THE RULE FOR CALLERS
//
// - Every request call returns at once, without touching the card: it
//   copies what it is given into the task's request queue and returns the
//   request's number (never 0), or 0 if the queue has no room for it (the
//   UI then says "Busy"; nothing was queued). The caller's own copies may
//   change the moment the call returns.
// - Requests are carried out one at a time, in the order they were queued,
//   whoever made them. None that was queued is lost: each ends with exactly
//   one event (playlist_event_t) carrying its number, its kind and how it
//   ended.
// - Every request names a client. The UI's events go to the queue given to
//   playlist_task_start (the UI adds it to its queue set); the console's go
//   to a queue of this module's own, which playlist_task_wait reads. So the
//   console never takes an event meant for the UI, nor the UI one meant for
//   the console.
// - A listing and a load have results too big for an event (a catalog is
//   about 17 KB, a load 2 KB). Each client has one result of each kind,
//   kept by this module; the event says it is ready and the client copies
//   it out with playlist_task_copy_catalog / playlist_task_copy_load, under
//   a lock held only for that copy. A newer result of the same kind for the
//   same client replaces the older one: if the client was slow and that has
//   happened, the copy answers false and copies nothing. The client then
//   simply ignores that event: the newer request is its own too, so its
//   event is already behind this one in the client's queue, and that one
//   copies.
// - Requests that carry track numbers (create, append) carry the checksum
//   (library_checksum) of the index the numbers belong to. The task holds
//   the index while it serves a request; tracks of another index are
//   PLAYLIST_TASK_STALE_INDEX and nothing is written. A load answers with
//   the checksum of the index it resolved the entries against.
// - With no index (a scan is running, or none has loaded) every request
//   ends PLAYLIST_TASK_LIBRARY_BUSY at once, even one that needs no index:
//   a scan keeps the card busy, and the request would otherwise wait for
//   the scan to end.
// - With no card mounted every request that needs the card ends
//   PLAYLIST_TASK_CARD_ERROR at once; nothing waits for a card, and nothing
//   is retried by itself.
// - Playlists are named by their file name (the name in the folder, ending
//   included, as playlist_catalog_file gives it). A file name that is not a
//   playlist's (no .m3u or .m3u8 ending, a '/' in it, starting with '.') is
//   PLAYLIST_TASK_NO_ENTRY.
//
// All of it may be called before playlist_task_start or after it failed:
// every request is then refused (0), and the copies answer false.

// The longest file name a request can carry, in bytes (the card's own
// limit: a folder listing gives at most 255).
#define PLAYLIST_FILE_MAX 255

// A suitable length for the UI's event queue. Any length works: the task
// waits for room in it rather than drop an event (it never waits for the UI
// otherwise, and the UI never waits for it).
#define PLAYLIST_EVENT_QUEUE_LENGTH 8

// Bytes of the request queue. A request takes about 30 bytes, its file name
// and name, and 2 bytes per track: a burst of over a hundred small edits
// fits, or three of the largest requests (1,000 tracks) and a dozen small
// ones.
#define PLAYLIST_REQUEST_QUEUE_BYTES 8192

// Who a request is for: where its event goes and which results it fills.
typedef enum {
  PLAYLIST_CLIENT_UI,      // events to the queue given to playlist_task_start
  PLAYLIST_CLIENT_CONSOLE, // events read with playlist_task_wait
  PLAYLIST_CLIENTS
} playlist_client_t;

typedef enum {
  PLAYLIST_REQUEST_LIST,      // list the folder (a catalog to copy)
  PLAYLIST_REQUEST_LOAD,      // read a playlist (a load to copy)
  PLAYLIST_REQUEST_CREATE,    // a new playlist, with tracks or none
  PLAYLIST_REQUEST_APPEND,    // tracks added at the end
  PLAYLIST_REQUEST_REMOVE,    // one entry removed
  PLAYLIST_REQUEST_MOVE_UP,   // one entry swapped with the one before
  PLAYLIST_REQUEST_MOVE_DOWN, // one entry swapped with the one after
  PLAYLIST_REQUEST_RENAME,
  PLAYLIST_REQUEST_DELETE,
  PLAYLIST_REQUEST_READ, // raw bytes of a file, for the console's `pl cat`
} playlist_request_t;

typedef enum {
  PLAYLIST_TASK_DONE,
  PLAYLIST_TASK_NAME_IN_USE, // create, rename: another playlist in the
                             // folder has that name (ignoring case and the
                             // ending)
  PLAYLIST_TASK_BAD_NAME,    // create, rename: the name breaks a rule; the
                             // event's `name` says which
  PLAYLIST_TASK_FULL,        // create, append: the playlist would have more
                             // than PLAYLIST_ENTRIES_MAX entries; nothing
                             // was written
  PLAYLIST_TASK_NO_ENTRY,    // the entry cannot be removed or moved that way
                             // (past either end), a track number is not in
                             // the index, or there is no such playlist
  PLAYLIST_TASK_BAD_FILE,    // the file has a line longer than
                             // PLAYLIST_LINE_MAX: it cannot be loaded, and
                             // an append to it is refused
  PLAYLIST_TASK_CARD_ERROR,  // no card, or reading or writing it failed;
                             // the playlist is as it was (see the power-cut
                             // notes in playlist_task.c), so reload it
  PLAYLIST_TASK_LIBRARY_BUSY, // no index right now (a scan is running)
  PLAYLIST_TASK_STALE_INDEX,  // create, append: the tracks are numbers of an
                              // index that is not the one loaded
} playlist_outcome_t;

// How a request ended. Small, so that it can go through a FreeRTOS queue in
// a queue set.
typedef struct {
  uint32_t request; // the number the request call returned
  uint8_t kind;     // playlist_request_t
  uint8_t outcome;  // playlist_outcome_t
  uint8_t name;     // playlist_name_t, when outcome is PLAYLIST_TASK_BAD_NAME
} playlist_event_t;

// A finished load.
typedef struct {
  uint32_t index;  // library_checksum of the index `tracks` belong to
  uint32_t count;  // entries read
  bool truncated;  // the file has more than PLAYLIST_ENTRIES_MAX entries;
                   // these are the first PLAYLIST_ENTRIES_MAX
  uint16_t tracks[PLAYLIST_ENTRIES_MAX]; // file order; PLAYLIST_MISSING for
                                         // an entry the index does not have
} playlist_loaded_t;

// Raw bytes of a file, for the console.
#define PLAYLIST_READ_BYTES 4096
typedef struct {
  uint32_t size;   // of the whole file
  uint32_t offset; // where these bytes start
  uint32_t length; // how many there are (0 at or past the end)
  uint8_t bytes[PLAYLIST_READ_BYTES];
} playlist_bytes_t;

// Allocate everything (all of it in PSRAM, the task's stack included: see
// MEMORY in playlist_task.c) and create the task (priority 2, core 0). Call
// once, before any request. `ui_events` is a queue of playlist_event_t that
// gets the events of PLAYLIST_CLIENT_UI's requests; it may be NULL, and
// those events are then dropped. The card need not be mounted yet.
//
// Never fatal: ESP_ERR_NO_MEM (logged) if the memory cannot be had; the
// player then runs without playlists (every request is refused).
esp_err_t playlist_task_start(QueueHandle_t ui_events);

// --- Requests ----------------------------------------------------------------
//
// Each returns the request's number, or 0 if it could not be queued. Each
// costs a copy of what it carries into PSRAM and a few instructions, and
// never blocks. `file` is a playlist's file name; `name` a name as typed
// (spaces at either end are trimmed; the task checks it with
// playlist_name_check).

// The folder's playlists, A to Z (playlist_catalog_t). Leftovers of saves a
// power cut interrupted are tidied first. No folder is an empty catalog.
uint32_t playlist_task_list(playlist_client_t client);

// The entries of a playlist as track numbers of the index now loaded
// (playlist_loaded_t).
uint32_t playlist_task_load(playlist_client_t client, const char *file);

// A new playlist `name` (file playlist_file_for(name)) holding `tracks`, in
// order; `count` may be 0. The folder is made if there is none. More than
// PLAYLIST_ENTRIES_MAX tracks is PLAYLIST_TASK_FULL.
uint32_t playlist_task_create(playlist_client_t client, const char *name,
                              const uint16_t *tracks, uint32_t count,
                              uint32_t index);

// `tracks` added at the end of the playlist.
uint32_t playlist_task_append(playlist_client_t client, const char *file,
                              const uint16_t *tracks, uint32_t count,
                              uint32_t index);

// Entry `entry` (from 0, file order) removed, or moved one place.
uint32_t playlist_task_remove(playlist_client_t client, const char *file,
                              uint32_t entry);
uint32_t playlist_task_move_up(playlist_client_t client, const char *file,
                               uint32_t entry);
uint32_t playlist_task_move_down(playlist_client_t client, const char *file,
                                 uint32_t entry);

// The playlist renamed to `name`. The file keeps its ending (.m3u, .m3u8,
// in whatever case it had), so a computer reads it as before. A name that
// is the playlist's own, ignoring case, is done and changes nothing.
uint32_t playlist_task_rename(playlist_client_t client, const char *file,
                              const char *name);

uint32_t playlist_task_delete(playlist_client_t client, const char *file);

// Up to PLAYLIST_READ_BYTES of the file from `offset` (playlist_bytes_t).
// For the console, which shows a file as it is.
uint32_t playlist_task_read(playlist_client_t client, const char *file,
                            uint32_t offset);

// --- Results -----------------------------------------------------------------
//
// Copy out the result of request `request` of `client`. False, with nothing
// copied, if that result is not the one held: the request was of another
// kind, did not end PLAYLIST_TASK_DONE, or a newer one of the same kind for
// the same client has replaced it (see THE RULE FOR CALLERS). Each copies
// only what is used (a catalog's names, a load's entries) under the lock,
// which the task holds for a few instructions at a time.

bool playlist_task_copy_catalog(playlist_client_t client, uint32_t request,
                                playlist_catalog_t *out);
bool playlist_task_copy_load(playlist_client_t client, uint32_t request,
                             playlist_loaded_t *out);
bool playlist_task_copy_bytes(playlist_client_t client, uint32_t request,
                              playlist_bytes_t *out);

// For the console: wait up to `timeout_ms` for the event of its request
// `request`. Events of its earlier requests that come first (ones it gave
// up waiting for) are thrown away. False if none came in time.
bool playlist_task_wait(uint32_t request, playlist_event_t *out,
                        uint32_t timeout_ms);

// Names for the log and the console.
const char *playlist_task_request_name(playlist_request_t kind);
const char *playlist_task_outcome_name(playlist_outcome_t outcome);
