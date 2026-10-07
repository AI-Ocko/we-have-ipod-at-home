#include "playlist_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "library_task.h"
#include "storage.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Design notes
//
// ONE TASK, ONE REQUEST AT A TIME
//
// The task sleeps on its request queue and serves what arrives, one request
// at a time, in the order queued. It runs at priority 2 on core 0: below the
// UI, the console and the library task, so none of them waits for it, and
// away from the audio tasks on core 1. Asleep it costs nothing; nothing is
// polled.
//
// THE REQUEST QUEUE
//
// Requests differ in size: most are a file name and a number, but a create
// or an append can carry 1,000 track numbers (2 KB). A FreeRTOS queue has
// one item size, so it would copy 2 KB for every "move up". The queue is a
// no-split ring buffer instead (esp_ringbuf): each request takes its own
// size, written in place (xRingbufferSendAcquire, then ...SendComplete), so
// a request costs one copy of what it carries. The task reads a request
// where it lies and gives its room back when it has served it. The ring
// buffer keeps items in the order their room was acquired, and the task
// takes them in that order, so requests are served in the order made, from
// any number of callers. Acquire never waits (timeout 0): no room is a
// refusal, number 0.
//
// Request numbers come from an atomic counter, skipping 0. Two callers'
// numbers need not be in queue order between them (the UI and the console
// can each take a number in the other's acquire-to-complete window); within
// one caller they are. Nothing compares numbers for order: a result is
// matched to its request by equality.
//
// EVENTS AND RESULTS
//
// Each request ends with one playlist_event_t, posted after its result (if
// any) is in place, so a client that sees the event finds the result.
//
//   - The UI's events go to the queue given at start-up, which the UI adds
//     to its queue set. The task waits for room there, as long as it takes:
//     the UI empties its queues on every pass and never waits for this task
//     (it only copies results under the lock, which this task never holds
//     while it posts), so the wait ends, and an event is never dropped.
//   - The console's events go to a queue of this module's own, read only by
//     playlist_task_wait, which skips events of earlier requests the
//     console gave up on. If the console has left that queue full (it gave
//     up on that many), the oldest event is dropped to make room: the
//     console only ever wants its newest. So the console cannot hold the
//     task up, and never takes the UI's events.
//
// Big results (a listing, a load, a console read) live in buffers this
// module owns, one per client and kind (s_slot), plus one spare per kind.
// The task fills the spare, with no lock, for as long as the card takes:
// no client can see the spare. Then, under s_lock, it swaps the spare with
// the client's slot and writes the request's number into the slot; the old
// slot becomes the next spare. A client copies its slot under s_lock, if
// the slot holds the request it asks about. So the task holds the lock for
// a pointer swap, and a client for one copy of what is used (for a full
// catalog of 200 names, 17 KB of PSRAM: well under the 4 ms the UI's queue
// copies take for 8 KB tracks, see state_task.c). s_lock is a mutex, not a
// critical section: a copy that long must not hold interrupts off.
//
// If a client is slow and a newer result of the same kind for it has come,
// its slot holds the newer one and the copy for the older request answers
// false. That event is to be ignored: the newer request is the client's
// own, its event comes later in the client's queue, and it copies.
//
// THE INDEX
//
// Track numbers are only good within one index (library_task.h). Every
// request takes the index with library_task_get when it starts and puts it
// back when it is done, before its event is posted: at most the time of a
// read or an edit of a 1,000-entry file, well under a second, which a scan
// asked for meanwhile waits out. A create or an append whose checksum is
// not that index's is refused before anything is opened; a load resolves
// against it and says so.
//
// With no index every request answers library busy, at once, even those
// that need none (a listing, a remove, a move). No index means a scan is
// running (or none has worked since start-up), and a scan holds the card:
// it runs at priority 3 and goes from one card operation to the next, so a
// request here would wait for the card until the scan ended (measured: a
// listing asked for during a scan of 1,028 tracks took 6.07 s). Holding the
// index for the whole request is also what keeps a scan from starting
// half-way through one.
//
// MEMORY
//
// Everything is in PSRAM and allocated once, at start-up:
//
//   work area (playlist_work_t)              16,384
//   catalogs, 3 (2 clients and a spare)      3 x 17,220 = 51,660
//   loads, 3                                 3 x  2,012 =  6,036
//   console reads, 3                         3 x  4,108 = 12,324
//   three paths                              3 x    288 =    864
//   request queue (ring buffer)              8,192
//   the console's event queue                32 x 8 = 256
//   the task's stack                         6,144
//
// about 101 KB in all (the exact total is logged at start-up). The ring
// buffer's and the console queue's control blocks (about 200 bytes) and the
// mutex are internal, as are the task's TCB (about 350 bytes) and, while a
// file is open, newlib's FILE (about 100 bytes each); FatFs's FIL, with its
// 4 KB sector buffer, is in PSRAM (CONFIG_FATFS_ALLOC_PREFER_EXTRAM), and so
// is its long-file-name buffer.
//
// The stack is in PSRAM too (xTaskCreatePinnedToCoreWithCaps; allowed by
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM, see sdkconfig.defaults). That
// is safe only for a task whose stack is never touched while the flash
// cache is off: this task never writes flash (no NVS, no OTA: the SD card
// is SDMMC, not SPI flash), so the cache is never disabled on its behalf,
// and while another task writes flash this one is not running (its context
// was saved before the cache went off; interrupts taken meanwhile run on
// the interrupt stack and the stack of the task that was running). The
// other ESP32 caveat, ROM code built without the PSRAM cache workaround,
// applies to chip revisions before 3, and this build requires revision 3
// (CONFIG_ESP32_REV_MIN_3).
//
// Streams are unbuffered (setvbuf _IONBF straight after fopen): a stdio
// buffer would be 4 KB of internal RAM, the scarcest thing there is while
// a track plays (state_task.c, MEMORY DURING A SAVE), and playlist_read and
// playlist_edit already read and write in blocks of at least 4 KB through
// the work area.
//
// SAVING, AND A POWER CUT AT EACH STEP
//
// As for the index (library_task.c) and the queue file (state_task.c), and
// for the same reason (FAT cannot rename onto an existing file), an edit of
// <file> takes three steps:
//
//   1. write the whole new playlist to <file>.tmp with playlist_edit, and
//      close it (every result checked, fclose included)
//   2. delete <file>
//   3. rename <file>.tmp to <file>
//
// A create is 1 and 3 (there is no <file>). Unlike the index, a playlist
// has no checksum, so whether a .tmp is whole cannot be told from its
// bytes. It can be told from its length: FatFs writes a file's length into
// its folder entry only when the file is closed (f_sync; CONFIG_FATFS_
// IMMEDIATE_FSYNC is off), after the data and the cluster chain are on the
// card, and in one sector write. So a .tmp that the power interrupted in
// step 1 has length 0 in its folder entry, whatever was written; a .tmp
// with a length is whole.
//
// The tidying, done when the folder is listed (before anyone can open a
// playlist), for each <file>.tmp whose <file> is a playlist's name:
//
//   <file> exists      delete the .tmp: <file> is the old playlist, whole
//                      (the cut was during or after step 1)
//   no <file>, the     rename it to <file>: it is whole, and <file> was
//   .tmp has a length  deleted by step 2 (or this was a create)
//   no <file>, length  delete it: a create the power interrupted in step
//   0                  1; there was no playlist before, and there is none
//
// What the next listing finds, by where the power went:
//
//   during 1   the .tmp has length 0 (or, if the cut came as it was
//              closed, it is whole); <file> is the old playlist. Deleted:
//              the old playlist stands, the edit is lost.
//   after 1    both whole. The .tmp is deleted: the old playlist stands.
//   during 2,  <file> gone or going (FatFs writes the folder entry before
//   after 2    it frees the clusters; a cut between leaves lost clusters,
//              not a damaged file); the .tmp is whole and is renamed: the
//              new playlist stands.
//   during 3   a rename writes the new name before it removes the old one,
//              so for a moment both names point at the same data. The
//              tidying sees <file> and deletes the .tmp, which frees the
//              data they share: <file> then reads back correctly until
//              those clusters are written over by another file, after
//              which it holds that file's bytes (entries show as missing,
//              or the file is a bad one) and the two share clusters until
//              one is deleted. The window is two sector writes long, the
//              same window the index and the queue file have; there it
//              ends in a rescan or an empty queue because they have
//              checksums. Closing it would need FatFs below the VFS (to see
//              that two entries share a first cluster), which is not done.
//   after 3    the normal case: only <file>.
//
// Every case but the "during 3" window leaves the old playlist or the new
// one, whole, and no .tmp.
//
// An edit that leaves nothing (the only entry removed) would give a whole
// .tmp of length 0, which the tidying could not tell from an interrupted
// create. So such an edit does not replace the file: once the empty .tmp is
// written and closed, <file> itself is truncated (one fopen "wb", which in
// FatFs is one folder-entry write: the length and first cluster set to 0,
// then the clusters freed), and the .tmp is deleted. A cut before the
// truncation leaves the old playlist and the .tmp (deleted by the tidying);
// after it, the empty one. A create of an empty playlist goes the usual
// way: its .tmp of length 0 interrupted before step 3 is deleted, which
// leaves no playlist, the state before the create.
//
// Failures that are not power cuts end the same way. A step 1 that fails
// deletes the .tmp (nothing else was touched). A step 2 that fails deletes
// the .tmp (the old playlist stands). A step 3 that fails leaves a whole
// .tmp and no <file>: it is not deleted, and the next listing renames it,
// so the new playlist is not lost; the request answers card error and the
// UI reloads, which lists first. (A create and a rename tidy the folder
// before they look for the name, so neither can write over such a .tmp or
// take the name it is to come back under.) Rename and delete delete a leftover .tmp
// of their playlist first (the playlist exists, so the tidying would delete
// it anyway): renamed or deleted from under its .tmp, a whole .tmp would
// otherwise come back under the old name at the next listing.
//
// A rename of a playlist is one FAT rename, with the window described for
// step 3: a cut there leaves both names (two playlists, sharing data).
//
// NO CARD, A SCAN, A CARD ERROR
//
// The card is mounted once, at start-up, and stays: with no card mounted a
// request answers card error at once. Without an index (a scan is running)
// a request answers library busy at once (see THE INDEX). A read or write
// that fails answers card error, with the files as the notes above leave
// them. Nothing is retried here: the UI decides what to do. Nothing aborts.

#define TASK_STACK_BYTES 6144
#define TASK_PRIORITY 2
#define TASK_CORE 0

#define DIR_PATH STORAGE_MOUNT_POINT "/playlists"
#define TMP_ENDING ".tmp"
// The folder, '/', a file name, ".tmp", the terminator; rounded up.
#define PATH_BYTES 288
_Static_assert(sizeof(DIR_PATH) + PLAYLIST_FILE_MAX + sizeof(TMP_ENDING) <=
                   PATH_BYTES,
               "a path must fit");

#define CONSOLE_EVENT_QUEUE_LENGTH 32
#define NAME_BYTES_MAX 64 // a typed name, trimmed: longer is too long anyway

// Request flags: what the request call already knew was wrong.
#define FLAG_BAD_FILE (1u << 0)  // the file name is too long to be one
#define FLAG_LONG_NAME (1u << 1) // the name is far too long
#define FLAG_TOO_MANY (1u << 2)  // more than PLAYLIST_ENTRIES_MAX tracks
#define FLAG_NO_TRACKS (1u << 3) // a count with no tracks

static const char *TAG = "playlist";

// A request as it lies in the ring buffer: this header, the file name and
// the name (each terminated), padding to an even length, the tracks.
typedef struct {
  uint32_t number;
  uint32_t index; // library checksum of the tracks
  uint32_t arg;   // the entry, or the offset of a read
  uint16_t count; // tracks
  uint16_t file_len;
  uint16_t name_len;
  uint8_t kind;
  uint8_t client;
  uint8_t flags;
  uint8_t unused; // so that `text`, and the tracks after it, are 2-aligned
  char text[];    // file, '\0', name, '\0'
} request_t;

_Static_assert(offsetof(request_t, text) % sizeof(uint16_t) == 0,
               "the tracks after the text must be aligned");

static inline const char *request_file(const request_t *r) { return r->text; }

static inline const char *request_name(const request_t *r) {
  return r->text + r->file_len + 1;
}

static inline size_t request_text_bytes(size_t file_len, size_t name_len) {
  return (file_len + 1 + name_len + 1 + 1) & ~(size_t)1;
}

static inline const uint16_t *request_tracks(const request_t *r) {
  return (const uint16_t *)(const void *)(r->text +
                                          request_text_bytes(r->file_len,
                                                             r->name_len));
}

// --- Memory ------------------------------------------------------------------

enum { RESULT_CATALOG, RESULT_LOADED, RESULT_BYTES, RESULT_KINDS };

typedef struct {
  playlist_work_t work;
  playlist_catalog_t catalog[PLAYLIST_CLIENTS + 1];
  playlist_loaded_t loaded[PLAYLIST_CLIENTS + 1];
  playlist_bytes_t bytes[PLAYLIST_CLIENTS + 1];
  char path[PATH_BYTES];     // the playlist
  char tmp_path[PATH_BYTES]; // its .tmp
  char new_path[PATH_BYTES]; // a rename's new name; a tidied .tmp
} memory_t;

static memory_t *s_mem;
static TaskHandle_t s_task;
static QueueHandle_t s_ui_events;
static QueueHandle_t s_console_events;
static StaticQueue_t s_console_queue_block;
static RingbufHandle_t s_ring;
static StaticRingbuffer_t s_ring_block;
static atomic_uint s_next_number;

// A client's result of one kind, and the request it answers (0: none).
typedef struct {
  void *result;
  uint32_t request;
} slot_t;

// s_lock guards s_slot. The task holds it for a swap, a client for a copy.
static SemaphoreHandle_t s_lock;
static slot_t s_slot[RESULT_KINDS][PLAYLIST_CLIENTS];
static void *s_spare[RESULT_KINDS]; // the task's alone

// Internal RAM, sampled with the files of an edit open, for the log.
static uint32_t s_int_free, s_int_largest;
static bool s_sampled;

// --- Names ---------------------------------------------------------------------

static inline unsigned char fold(unsigned char c) {
  return (unsigned char)(c - 'A') < 26u ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool ends_with_folded(const char *s, size_t len, const char *end) {
  size_t n = strlen(end);
  if (len < n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (fold((unsigned char)s[len - n + i]) != (unsigned char)end[i]) {
      return false;
    }
  }
  return true;
}

// The length of a playlist ending (.m3u or .m3u8, any case), or 0.
static size_t ending_length(const char *file, size_t len) {
  if (ends_with_folded(file, len, ".m3u8")) {
    return 5;
  }
  return ends_with_folded(file, len, ".m3u") ? 4 : 0;
}

// Whether `file` can be a playlist in the folder: a name, not a path, with
// a playlist ending, not hidden.
static bool is_playlist_file(const char *file) {
  size_t len = strlen(file);
  if (len == 0 || len > PLAYLIST_FILE_MAX || file[0] == '.' ||
      strchr(file, '/') != NULL || strchr(file, '\\') != NULL) {
    return false;
  }
  size_t end = ending_length(file, len);
  return end > 0 && end < len;
}

// Whether playlist file `file` has the name `name`, as names are compared:
// ignoring ASCII case and the ending.
static bool has_name(const char *file, const char *name) {
  size_t len = strlen(file);
  size_t n = len - ending_length(file, len);
  if (strlen(name) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (fold((unsigned char)file[i]) != fold((unsigned char)name[i])) {
      return false;
    }
  }
  return true;
}

static void path_of(char *out, const char *file, const char *suffix) {
  snprintf(out, PATH_BYTES, "%s/%s%s", DIR_PATH, file, suffix);
}

const char *playlist_task_request_name(playlist_request_t kind) {
  switch (kind) {
  case PLAYLIST_REQUEST_LIST:
    return "list";
  case PLAYLIST_REQUEST_LOAD:
    return "load";
  case PLAYLIST_REQUEST_CREATE:
    return "create";
  case PLAYLIST_REQUEST_APPEND:
    return "append";
  case PLAYLIST_REQUEST_REMOVE:
    return "remove";
  case PLAYLIST_REQUEST_MOVE_UP:
    return "move_up";
  case PLAYLIST_REQUEST_MOVE_DOWN:
    return "move_down";
  case PLAYLIST_REQUEST_RENAME:
    return "rename";
  case PLAYLIST_REQUEST_DELETE:
    return "delete";
  case PLAYLIST_REQUEST_READ:
    return "read";
  }
  return "?";
}

const char *playlist_task_outcome_name(playlist_outcome_t outcome) {
  switch (outcome) {
  case PLAYLIST_TASK_DONE:
    return "done";
  case PLAYLIST_TASK_NAME_IN_USE:
    return "name in use";
  case PLAYLIST_TASK_BAD_NAME:
    return "bad name";
  case PLAYLIST_TASK_FULL:
    return "playlist full";
  case PLAYLIST_TASK_NO_ENTRY:
    return "no such entry";
  case PLAYLIST_TASK_BAD_FILE:
    return "bad file";
  case PLAYLIST_TASK_CARD_ERROR:
    return "card error";
  case PLAYLIST_TASK_LIBRARY_BUSY:
    return "library busy";
  case PLAYLIST_TASK_STALE_INDEX:
    return "stale index";
  }
  return "?";
}

// --- Requests (any task) -------------------------------------------------------

static uint32_t next_number(void) {
  uint32_t n;
  do {
    n = (uint32_t)atomic_fetch_add(&s_next_number, 1u) + 1u;
  } while (n == 0);
  return n;
}

static uint32_t submit(playlist_client_t client, playlist_request_t kind,
                       const char *file, const char *name,
                       const uint16_t *tracks, uint32_t count, uint32_t index,
                       uint32_t arg) {
  if (s_ring == NULL || (unsigned)client >= PLAYLIST_CLIENTS) {
    return 0;
  }
  uint8_t flags = 0;
  size_t file_len = file != NULL ? strlen(file) : 0;
  if (file_len > PLAYLIST_FILE_MAX) {
    flags |= FLAG_BAD_FILE;
    file_len = 0;
  }
  // The name trimmed as playlist_name_check trims it, so that what is
  // carried is bounded.
  const char *n = name != NULL ? name : "";
  while (*n == ' ') {
    n++;
  }
  size_t name_len = strlen(n);
  while (name_len > 0 && n[name_len - 1] == ' ') {
    name_len--;
  }
  if (name_len > NAME_BYTES_MAX) {
    flags |= FLAG_LONG_NAME;
    name_len = 0;
  }
  if (count > PLAYLIST_ENTRIES_MAX) {
    flags |= FLAG_TOO_MANY;
    count = 0;
  } else if (count > 0 && tracks == NULL) {
    flags |= FLAG_NO_TRACKS;
    count = 0;
  }

  size_t text = request_text_bytes(file_len, name_len);
  size_t size = offsetof(request_t, text) + text + count * sizeof(uint16_t);
  void *item = NULL;
  if (xRingbufferSendAcquire(s_ring, &item, size, 0) != pdTRUE ||
      item == NULL) {
    return 0;
  }
  request_t *r = item;
  r->index = index;
  r->arg = arg;
  r->count = (uint16_t)count;
  r->file_len = (uint16_t)file_len;
  r->name_len = (uint16_t)name_len;
  r->kind = (uint8_t)kind;
  r->client = (uint8_t)client;
  r->flags = flags;
  r->unused = 0;
  if (file_len > 0) {
    memcpy(r->text, file, file_len);
  }
  r->text[file_len] = '\0';
  if (name_len > 0) {
    memcpy(r->text + file_len + 1, n, name_len);
  }
  r->text[file_len + 1 + name_len] = '\0';
  if (count > 0) {
    memcpy((void *)request_tracks(r), tracks, count * sizeof(uint16_t));
  }
  uint32_t number = next_number();
  r->number = number;
  xRingbufferSendComplete(s_ring, item);
  return number;
}

uint32_t playlist_task_list(playlist_client_t client) {
  return submit(client, PLAYLIST_REQUEST_LIST, NULL, NULL, NULL, 0, 0, 0);
}

uint32_t playlist_task_load(playlist_client_t client, const char *file) {
  return submit(client, PLAYLIST_REQUEST_LOAD, file, NULL, NULL, 0, 0, 0);
}

uint32_t playlist_task_create(playlist_client_t client, const char *name,
                              const uint16_t *tracks, uint32_t count,
                              uint32_t index) {
  return submit(client, PLAYLIST_REQUEST_CREATE, NULL, name, tracks, count,
                index, 0);
}

uint32_t playlist_task_append(playlist_client_t client, const char *file,
                              const uint16_t *tracks, uint32_t count,
                              uint32_t index) {
  return submit(client, PLAYLIST_REQUEST_APPEND, file, NULL, tracks, count,
                index, 0);
}

uint32_t playlist_task_remove(playlist_client_t client, const char *file,
                              uint32_t entry) {
  return submit(client, PLAYLIST_REQUEST_REMOVE, file, NULL, NULL, 0, 0, entry);
}

uint32_t playlist_task_move_up(playlist_client_t client, const char *file,
                               uint32_t entry) {
  return submit(client, PLAYLIST_REQUEST_MOVE_UP, file, NULL, NULL, 0, 0,
                entry);
}

uint32_t playlist_task_move_down(playlist_client_t client, const char *file,
                                 uint32_t entry) {
  return submit(client, PLAYLIST_REQUEST_MOVE_DOWN, file, NULL, NULL, 0, 0,
                entry);
}

uint32_t playlist_task_rename(playlist_client_t client, const char *file,
                              const char *name) {
  return submit(client, PLAYLIST_REQUEST_RENAME, file, name, NULL, 0, 0, 0);
}

uint32_t playlist_task_delete(playlist_client_t client, const char *file) {
  return submit(client, PLAYLIST_REQUEST_DELETE, file, NULL, NULL, 0, 0, 0);
}

uint32_t playlist_task_read(playlist_client_t client, const char *file,
                            uint32_t offset) {
  return submit(client, PLAYLIST_REQUEST_READ, file, NULL, NULL, 0, 0, offset);
}

// --- Results (any task) --------------------------------------------------------

// Take the lock and find `client`'s slot of `kind` if it answers `request`.
// NULL (and the lock not held) if it does not.
static const void *slot_begin(int kind, playlist_client_t client,
                              uint32_t request) {
  if (s_lock == NULL || (unsigned)client >= PLAYLIST_CLIENTS || request == 0) {
    return NULL;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const slot_t *slot = &s_slot[kind][client];
  if (slot->request != request) {
    xSemaphoreGive(s_lock);
    return NULL;
  }
  return slot->result;
}

static void slot_end(void) { xSemaphoreGive(s_lock); }

bool playlist_task_copy_catalog(playlist_client_t client, uint32_t request,
                                playlist_catalog_t *out) {
  const playlist_catalog_t *c = slot_begin(RESULT_CATALOG, client, request);
  if (c == NULL) {
    return false;
  }
  out->count = c->count;
  out->truncated = c->truncated;
  out->pool_used = c->pool_used;
  memcpy(out->offset, c->offset, c->count * sizeof(c->offset[0]));
  memcpy(out->pool, c->pool, c->pool_used);
  slot_end();
  return true;
}

bool playlist_task_copy_load(playlist_client_t client, uint32_t request,
                             playlist_loaded_t *out) {
  const playlist_loaded_t *l = slot_begin(RESULT_LOADED, client, request);
  if (l == NULL) {
    return false;
  }
  out->index = l->index;
  out->count = l->count;
  out->truncated = l->truncated;
  memcpy(out->tracks, l->tracks, l->count * sizeof(l->tracks[0]));
  slot_end();
  return true;
}

bool playlist_task_copy_bytes(playlist_client_t client, uint32_t request,
                              playlist_bytes_t *out) {
  const playlist_bytes_t *b = slot_begin(RESULT_BYTES, client, request);
  if (b == NULL) {
    return false;
  }
  out->size = b->size;
  out->offset = b->offset;
  out->length = b->length;
  memcpy(out->bytes, b->bytes, b->length);
  slot_end();
  return true;
}

bool playlist_task_wait(uint32_t request, playlist_event_t *out,
                        uint32_t timeout_ms) {
  if (s_console_events == NULL || request == 0) {
    return false;
  }
  TickType_t start = xTaskGetTickCount();
  TickType_t limit = pdMS_TO_TICKS(timeout_ms);
  for (;;) {
    TickType_t waited = xTaskGetTickCount() - start;
    if (waited > limit) {
      return false;
    }
    playlist_event_t event;
    if (xQueueReceive(s_console_events, &event, limit - waited) != pdTRUE) {
      return false;
    }
    if (event.request == request) {
      *out = event;
      return true;
    }
    // An event of a request the console gave up waiting for.
  }
}

// --- The task's side: results and events ---------------------------------------

// The spare of `kind` now holds the result of `r`: make it the client's.
static void publish(int kind, const request_t *r) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  slot_t *slot = &s_slot[kind][r->client];
  void *old = slot->result;
  slot->result = s_spare[kind];
  slot->request = r->number;
  s_spare[kind] = old;
  xSemaphoreGive(s_lock);
}

static void post(const request_t *r, playlist_outcome_t outcome,
                 playlist_name_t name) {
  const playlist_event_t event = {
      .request = r->number,
      .kind = r->kind,
      .outcome = (uint8_t)outcome,
      .name = (uint8_t)name,
  };
  if (r->client == PLAYLIST_CLIENT_UI) {
    if (s_ui_events != NULL) {
      xQueueSend(s_ui_events, &event, portMAX_DELAY);
    }
    return;
  }
  // The console's queue: see EVENTS AND RESULTS.
  if (xQueueSend(s_console_events, &event, 0) != pdTRUE) {
    playlist_event_t oldest;
    xQueueReceive(s_console_events, &oldest, 0);
    xQueueSend(s_console_events, &event, 0);
  }
}

static void sample_internal_ram(void) {
  s_int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT);
  s_int_largest = (uint32_t)heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_sampled = true;
}

// --- Listing and tidying -------------------------------------------------------

// One leftover .tmp, `name`: see SAVING for the three cases. Uses
// s_mem->tmp_path and s_mem->new_path.
static void tidy(const char *name) {
  size_t len = strlen(name) - (sizeof(TMP_ENDING) - 1);
  char *tmp = s_mem->tmp_path;
  char *base = s_mem->new_path;
  snprintf(tmp, PATH_BYTES, "%s/%s", DIR_PATH, name);
  snprintf(base, PATH_BYTES, "%s/%.*s", DIR_PATH, (int)len, name);
  struct stat st;
  if (stat(base, &st) == 0) {
    ESP_LOGW(TAG, "discarding an unfinished save: %s", name);
    unlink(tmp);
    return;
  }
  if (errno != ENOENT || stat(tmp, &st) != 0) {
    ESP_LOGW(TAG, "cannot tidy %s (%s)", name, strerror(errno));
    return;
  }
  if (st.st_size > 0) {
    ESP_LOGW(TAG, "finishing a save that was interrupted: %s", name);
    if (rename(tmp, base) != 0) {
      ESP_LOGW(TAG, "could not rename %s (%s)", name, strerror(errno));
    }
  } else {
    ESP_LOGW(TAG, "discarding an unfinished new playlist: %s", name);
    unlink(tmp);
  }
}

// Read the folder into `c`. Leftover .tmp names of playlists go into the
// work area (packed, terminated) if `tmps` is set; how many is returned in
// `*n_tmp`. False on a card error. No folder is an empty catalog.
static bool read_folder(playlist_catalog_t *c, bool tmps, uint32_t *n_tmp) {
  playlist_catalog_clear(c);
  *n_tmp = 0;
  DIR *dir = opendir(DIR_PATH);
  if (dir == NULL) {
    return errno == ENOENT;
  }
  char *names = (char *)s_mem->work.bytes;
  size_t used = 0;
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *e = readdir(dir);
    if (e == NULL) {
      ok = (errno == 0);
      break;
    }
    if (e->d_type == DT_DIR) {
      continue;
    }
    size_t len = strlen(e->d_name);
    if (ends_with_folded(e->d_name, len, TMP_ENDING)) {
      size_t base = len - (sizeof(TMP_ENDING) - 1);
      if (tmps && e->d_name[0] != '.' && ending_length(e->d_name, base) > 0 &&
          used + len + 1 <= sizeof(s_mem->work.bytes)) {
        memcpy(names + used, e->d_name, len + 1);
        used += len + 1;
        (*n_tmp)++;
      }
      continue;
    }
    playlist_catalog_add(c, e->d_name);
  }
  closedir(dir);
  return ok;
}

// Tidy the folder's leftovers (SAVING), then read it into `c`. False on a
// card error.
static bool tidy_and_read(playlist_catalog_t *c) {
  uint32_t n_tmp;
  if (!read_folder(c, true, &n_tmp)) {
    return false;
  }
  if (n_tmp == 0) {
    return true;
  }
  // Tidied after the folder is closed, then read again: a rename makes a
  // playlist the first reading did not have.
  const char *name = (const char *)s_mem->work.bytes;
  for (uint32_t i = 0; i < n_tmp; i++) {
    tidy(name);
    name += strlen(name) + 1;
  }
  return read_folder(c, false, &n_tmp);
}

static playlist_outcome_t do_list(const request_t *r) {
  playlist_catalog_t *c = s_spare[RESULT_CATALOG];
  if (!tidy_and_read(c)) {
    return PLAYLIST_TASK_CARD_ERROR;
  }
  playlist_catalog_sort(c);
  publish(RESULT_CATALOG, r);
  return PLAYLIST_TASK_DONE;
}

// Whether a playlist in the folder has `name`. NAME_IN_USE, DONE (not in
// use) or CARD_ERROR. Looks at every file, so a catalog cut short at
// PLAYLIST_CATALOG_MAX does not hide one. The folder is tidied first: a
// whole .tmp whose playlist step 2 had deleted (a card error in step 3) is
// that playlist, and must neither be written over by a create of the same
// name nor be left unable to come back because a rename took its name.
static playlist_outcome_t check_in_use(const char *name) {
  if (!tidy_and_read(s_spare[RESULT_CATALOG])) {
    return PLAYLIST_TASK_CARD_ERROR;
  }
  DIR *dir = opendir(DIR_PATH);
  if (dir == NULL) {
    return errno == ENOENT ? PLAYLIST_TASK_DONE : PLAYLIST_TASK_CARD_ERROR;
  }
  playlist_outcome_t outcome = PLAYLIST_TASK_DONE;
  for (;;) {
    errno = 0;
    struct dirent *e = readdir(dir);
    if (e == NULL) {
      if (errno != 0) {
        outcome = PLAYLIST_TASK_CARD_ERROR;
      }
      break;
    }
    if (e->d_type != DT_DIR && e->d_name[0] != '.' &&
        ending_length(e->d_name, strlen(e->d_name)) > 0 &&
        has_name(e->d_name, name)) {
      outcome = PLAYLIST_TASK_NAME_IN_USE;
      break;
    }
  }
  closedir(dir);
  return outcome;
}

// --- Loading and reading ---------------------------------------------------------

static playlist_outcome_t do_load(const request_t *r, const library_t *lib) {
  path_of(s_mem->path, request_file(r), "");
  FILE *f = fopen(s_mem->path, "rb");
  if (f == NULL) {
    return errno == ENOENT ? PLAYLIST_TASK_NO_ENTRY : PLAYLIST_TASK_CARD_ERROR;
  }
  setvbuf(f, NULL, _IONBF, 0); // see MEMORY
  playlist_loaded_t *l = s_spare[RESULT_LOADED];
  playlist_result_t result =
      playlist_read(f, lib, l->tracks, PLAYLIST_ENTRIES_MAX, &l->count,
                    &s_mem->work);
  l->index = library_checksum(lib);
  l->truncated = (result == PLAYLIST_TRUNCATED);
  fclose(f);
  if (result == PLAYLIST_OK || result == PLAYLIST_TRUNCATED) {
    publish(RESULT_LOADED, r);
    return PLAYLIST_TASK_DONE;
  }
  return result == PLAYLIST_BAD_FILE ? PLAYLIST_TASK_BAD_FILE
                                     : PLAYLIST_TASK_CARD_ERROR;
}

static playlist_outcome_t do_read(const request_t *r) {
  path_of(s_mem->path, request_file(r), "");
  FILE *f = fopen(s_mem->path, "rb");
  if (f == NULL) {
    return errno == ENOENT ? PLAYLIST_TASK_NO_ENTRY : PLAYLIST_TASK_CARD_ERROR;
  }
  setvbuf(f, NULL, _IONBF, 0);
  playlist_bytes_t *b = s_spare[RESULT_BYTES];
  struct stat st;
  bool ok = fstat(fileno(f), &st) == 0;
  b->size = ok ? (uint32_t)st.st_size : 0;
  b->offset = r->arg;
  b->length = 0;
  if (ok && r->arg < b->size) {
    ok = fseek(f, (long)r->arg, SEEK_SET) == 0;
    if (ok) {
      size_t want = b->size - r->arg;
      if (want > PLAYLIST_READ_BYTES) {
        want = PLAYLIST_READ_BYTES;
      }
      b->length = (uint32_t)fread(b->bytes, 1, want, f);
      ok = (b->length == want);
    }
  }
  fclose(f);
  if (!ok) {
    return PLAYLIST_TASK_CARD_ERROR;
  }
  publish(RESULT_BYTES, r);
  return PLAYLIST_TASK_DONE;
}

// --- Saving ----------------------------------------------------------------------

static playlist_outcome_t edit_outcome(playlist_result_t result) {
  switch (result) {
  case PLAYLIST_FULL:
    return PLAYLIST_TASK_FULL;
  case PLAYLIST_NO_ENTRY:
    return PLAYLIST_TASK_NO_ENTRY;
  case PLAYLIST_BAD_FILE:
    return PLAYLIST_TASK_BAD_FILE;
  default:
    return PLAYLIST_TASK_CARD_ERROR;
  }
}

// Write s_mem->path with `edit` applied (steps 1 to 3 of SAVING), from the
// file as it is, or, for a create, from nothing.
static playlist_outcome_t save(bool create, const library_t *lib,
                               const playlist_edit_t *edit) {
  const char *base = s_mem->path;
  const char *tmp = s_mem->tmp_path;
  FILE *in = NULL;
  if (!create) {
    in = fopen(base, "rb");
    if (in == NULL) {
      return errno == ENOENT ? PLAYLIST_TASK_NO_ENTRY
                             : PLAYLIST_TASK_CARD_ERROR;
    }
    setvbuf(in, NULL, _IONBF, 0); // see MEMORY
  }
  FILE *out = fopen(tmp, "wb");
  if (out == NULL && create && errno == ENOENT) {
    // The first playlist: the folder is made now.
    mkdir(DIR_PATH, 0775);
    out = fopen(tmp, "wb");
  }
  if (out == NULL) {
    ESP_LOGW(TAG, "could not create %s (%s)", tmp, strerror(errno));
    if (in != NULL) {
      fclose(in);
    }
    return PLAYLIST_TASK_CARD_ERROR;
  }
  setvbuf(out, NULL, _IONBF, 0);
  sample_internal_ram();

  playlist_result_t result = playlist_edit(in, out, lib, edit, &s_mem->work);
  long size = ftell(out);
  if (in != NULL) {
    fclose(in);
  }
  // The close writes the file's length and its last data to the card, so
  // it can fail when the writes did not.
  bool closed = (fclose(out) == 0);
  if (result != PLAYLIST_OK || !closed || size < 0) {
    if (result == PLAYLIST_OK || result == PLAYLIST_IO_ERROR) {
      ESP_LOGW(TAG, "could not write %s (%s)", tmp, strerror(errno));
    }
    unlink(tmp); // no half-written .tmp is left for the tidying
    return result == PLAYLIST_OK ? PLAYLIST_TASK_CARD_ERROR
                                 : edit_outcome(result);
  }

  if (create) {
    if (rename(tmp, base) != 0) {
      ESP_LOGW(TAG, "could not rename %s (%s)", tmp, strerror(errno));
      unlink(tmp);
      return PLAYLIST_TASK_CARD_ERROR;
    }
    return PLAYLIST_TASK_DONE;
  }
  if (size == 0) {
    // Nothing left: truncate the file itself (see SAVING, an edit that
    // leaves nothing).
    FILE *f = fopen(base, "wb");
    bool emptied = f != NULL && fclose(f) == 0;
    unlink(tmp);
    return emptied ? PLAYLIST_TASK_DONE : PLAYLIST_TASK_CARD_ERROR;
  }
  if (unlink(base) != 0 && errno != ENOENT) {
    ESP_LOGW(TAG, "could not remove %s (%s)", base, strerror(errno));
    unlink(tmp); // the old playlist stands
    return PLAYLIST_TASK_CARD_ERROR;
  }
  if (rename(tmp, base) != 0) {
    // The .tmp is whole and the old file gone: the next listing finishes
    // the job (SAVING).
    ESP_LOGW(TAG, "could not rename %s (%s)", tmp, strerror(errno));
    return PLAYLIST_TASK_CARD_ERROR;
  }
  return PLAYLIST_TASK_DONE;
}

static playlist_outcome_t do_create(const request_t *r, const library_t *lib,
                                    playlist_name_t *name_result) {
  char name[PLAYLIST_NAME_MAX + 1];
  *name_result = playlist_name_check(request_name(r), name);
  if (*name_result != PLAYLIST_NAME_OK) {
    return PLAYLIST_TASK_BAD_NAME;
  }
  if (r->flags & FLAG_TOO_MANY) {
    return PLAYLIST_TASK_FULL;
  }
  playlist_outcome_t in_use = check_in_use(name);
  if (in_use != PLAYLIST_TASK_DONE) {
    return in_use;
  }
  if (r->count > 0 && library_checksum(lib) != r->index) {
    return PLAYLIST_TASK_STALE_INDEX;
  }
  char file[PLAYLIST_NAME_MAX + sizeof(".m3u")];
  playlist_file_for(name, file, sizeof(file));
  path_of(s_mem->path, file, "");
  path_of(s_mem->tmp_path, file, TMP_ENDING);
  const playlist_edit_t edit = {
      .kind = PLAYLIST_EDIT_APPEND,
      .tracks = request_tracks(r),
      .count = r->count,
  };
  return save(true, lib, &edit);
}

static playlist_outcome_t do_edit(const request_t *r, const library_t *lib) {
  playlist_edit_t edit = {.entry = r->arg};
  switch (r->kind) {
  case PLAYLIST_REQUEST_APPEND:
    if (r->flags & FLAG_TOO_MANY) {
      return PLAYLIST_TASK_FULL;
    }
    if (library_checksum(lib) != r->index) {
      return PLAYLIST_TASK_STALE_INDEX;
    }
    edit.kind = PLAYLIST_EDIT_APPEND;
    edit.tracks = request_tracks(r);
    edit.count = r->count;
    break;
  case PLAYLIST_REQUEST_REMOVE:
    edit.kind = PLAYLIST_EDIT_REMOVE;
    break;
  case PLAYLIST_REQUEST_MOVE_UP:
    edit.kind = PLAYLIST_EDIT_MOVE_UP;
    break;
  default:
    edit.kind = PLAYLIST_EDIT_MOVE_DOWN;
    break;
  }
  path_of(s_mem->path, request_file(r), "");
  path_of(s_mem->tmp_path, request_file(r), TMP_ENDING);
  return save(false, lib, &edit);
}

// The playlist exists (CARD_ERROR or NO_ENTRY if it cannot be seen), and
// any .tmp it has is gone (see SAVING: rename and delete).
static playlist_outcome_t prepare_rename_or_delete(const request_t *r) {
  path_of(s_mem->path, request_file(r), "");
  path_of(s_mem->tmp_path, request_file(r), TMP_ENDING);
  struct stat st;
  if (stat(s_mem->path, &st) != 0) {
    return errno == ENOENT ? PLAYLIST_TASK_NO_ENTRY : PLAYLIST_TASK_CARD_ERROR;
  }
  if (unlink(s_mem->tmp_path) != 0 && errno != ENOENT) {
    return PLAYLIST_TASK_CARD_ERROR;
  }
  return PLAYLIST_TASK_DONE;
}

static playlist_outcome_t do_rename(const request_t *r,
                                    playlist_name_t *name_result) {
  char name[PLAYLIST_NAME_MAX + 1];
  *name_result = playlist_name_check(request_name(r), name);
  if (*name_result != PLAYLIST_NAME_OK) {
    return PLAYLIST_TASK_BAD_NAME;
  }
  const char *file = request_file(r);
  playlist_outcome_t outcome = prepare_rename_or_delete(r);
  if (outcome != PLAYLIST_TASK_DONE || has_name(file, name)) {
    return outcome; // a name that is its own: done, nothing changed
  }
  outcome = check_in_use(name);
  if (outcome != PLAYLIST_TASK_DONE) {
    return outcome;
  }
  // The new name with the file's own ending.
  size_t len = strlen(file);
  const char *ending = file + len - ending_length(file, len);
  snprintf(s_mem->new_path, PATH_BYTES, "%s/%s%s", DIR_PATH, name, ending);
  if (rename(s_mem->path, s_mem->new_path) != 0) {
    ESP_LOGW(TAG, "could not rename %s (%s)", file, strerror(errno));
    return errno == ENOENT   ? PLAYLIST_TASK_NO_ENTRY
           : errno == EEXIST ? PLAYLIST_TASK_NAME_IN_USE
                             : PLAYLIST_TASK_CARD_ERROR;
  }
  return PLAYLIST_TASK_DONE;
}

static playlist_outcome_t do_delete(const request_t *r) {
  playlist_outcome_t outcome = prepare_rename_or_delete(r);
  if (outcome != PLAYLIST_TASK_DONE) {
    return outcome;
  }
  if (unlink(s_mem->path) != 0) {
    ESP_LOGW(TAG, "could not delete %s (%s)", request_file(r),
             strerror(errno));
    return errno == ENOENT ? PLAYLIST_TASK_NO_ENTRY : PLAYLIST_TASK_CARD_ERROR;
  }
  return PLAYLIST_TASK_DONE;
}

// --- The task ----------------------------------------------------------------------

static playlist_outcome_t serve(const request_t *r,
                                playlist_name_t *name_result) {
  if (r->flags & (FLAG_BAD_FILE | FLAG_NO_TRACKS)) {
    return PLAYLIST_TASK_NO_ENTRY;
  }
  if (r->flags & FLAG_LONG_NAME) {
    *name_result = PLAYLIST_NAME_TOO_LONG;
    return PLAYLIST_TASK_BAD_NAME;
  }
  bool has_file = r->kind != PLAYLIST_REQUEST_LIST &&
                  r->kind != PLAYLIST_REQUEST_CREATE;
  if (has_file && !is_playlist_file(request_file(r))) {
    return PLAYLIST_TASK_NO_ENTRY;
  }
  if (!storage_is_mounted()) {
    return PLAYLIST_TASK_CARD_ERROR;
  }
  // Held for the whole request: see THE INDEX.
  const library_t *lib = library_task_get();
  if (lib == NULL) {
    return PLAYLIST_TASK_LIBRARY_BUSY;
  }
  playlist_outcome_t outcome = PLAYLIST_TASK_NO_ENTRY;
  switch (r->kind) {
  case PLAYLIST_REQUEST_LIST:
    outcome = do_list(r);
    break;
  case PLAYLIST_REQUEST_LOAD:
    outcome = do_load(r, lib);
    break;
  case PLAYLIST_REQUEST_CREATE:
    outcome = do_create(r, lib, name_result);
    break;
  case PLAYLIST_REQUEST_APPEND:
  case PLAYLIST_REQUEST_REMOVE:
  case PLAYLIST_REQUEST_MOVE_UP:
  case PLAYLIST_REQUEST_MOVE_DOWN:
    outcome = do_edit(r, lib);
    break;
  case PLAYLIST_REQUEST_RENAME:
    outcome = do_rename(r, name_result);
    break;
  case PLAYLIST_REQUEST_DELETE:
    outcome = do_delete(r);
    break;
  case PLAYLIST_REQUEST_READ:
    outcome = do_read(r);
    break;
  }
  library_task_put();
  return outcome;
}

static void playlist_task(void *arg) {
  (void)arg;
  for (;;) {
    size_t size;
    request_t *r = xRingbufferReceive(s_ring, &size, portMAX_DELAY);
    if (r == NULL) {
      continue;
    }
    int64_t start = esp_timer_get_time();
    s_sampled = false;
    playlist_name_t name_result = PLAYLIST_NAME_OK;
    playlist_outcome_t outcome = serve(r, &name_result);
    uint32_t us = (uint32_t)(esp_timer_get_time() - start);

    const char *shown = r->kind == PLAYLIST_REQUEST_CREATE ? request_name(r)
                                                           : request_file(r);
    if (s_sampled) {
      ESP_LOGI(TAG,
               "#%" PRIu32 " %s \"%s\": %s in %" PRIu32 " us (internal RAM "
               "with the files open: %" PRIu32 " free, largest %" PRIu32
               "; least ever %u; stack free %u)",
               r->number, playlist_task_request_name(r->kind), shown,
               playlist_task_outcome_name(outcome), us, s_int_free,
               s_int_largest,
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT),
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else {
      ESP_LOGI(TAG, "#%" PRIu32 " %s \"%s\": %s in %" PRIu32 " us", r->number,
               playlist_task_request_name(r->kind), shown,
               playlist_task_outcome_name(outcome), us);
    }

    // The event is made from the request before its room is given back.
    request_t done;
    memcpy(&done, r, offsetof(request_t, text));
    vRingbufferReturnItem(s_ring, r);
    post(&done, outcome, name_result);
  }
}

esp_err_t playlist_task_start(QueueHandle_t ui_events) {
  s_ui_events = ui_events;
  s_mem = heap_caps_calloc(1, sizeof(memory_t),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *ring = heap_caps_malloc(PLAYLIST_REQUEST_QUEUE_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *events =
      heap_caps_malloc(CONSOLE_EVENT_QUEUE_LENGTH * sizeof(playlist_event_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  RingbufHandle_t rb = NULL;
  QueueHandle_t console = NULL;
  if (s_mem != NULL && ring != NULL && events != NULL && lock != NULL) {
    rb = xRingbufferCreateStatic(PLAYLIST_REQUEST_QUEUE_BYTES,
                                 RINGBUF_TYPE_NOSPLIT, ring, &s_ring_block);
    console = xQueueCreateStatic(CONSOLE_EVENT_QUEUE_LENGTH,
                                 sizeof(playlist_event_t), events,
                                 &s_console_queue_block);
  }
  // The largest request: a file name, a name and PLAYLIST_ENTRIES_MAX tracks.
  const size_t largest = offsetof(request_t, text) +
                         request_text_bytes(PLAYLIST_FILE_MAX, NAME_BYTES_MAX) +
                         PLAYLIST_ENTRIES_MAX * sizeof(uint16_t);
  bool ok = rb != NULL && console != NULL &&
            xRingbufferGetMaxItemSize(rb) >= largest;
  if (ok) {
    for (int k = 0; k < RESULT_KINDS; k++) {
      for (int c = 0; c < PLAYLIST_CLIENTS; c++) {
        s_slot[k][c].request = 0;
      }
    }
    for (int c = 0; c < PLAYLIST_CLIENTS; c++) {
      s_slot[RESULT_CATALOG][c].result = &s_mem->catalog[c];
      s_slot[RESULT_LOADED][c].result = &s_mem->loaded[c];
      s_slot[RESULT_BYTES][c].result = &s_mem->bytes[c];
    }
    s_spare[RESULT_CATALOG] = &s_mem->catalog[PLAYLIST_CLIENTS];
    s_spare[RESULT_LOADED] = &s_mem->loaded[PLAYLIST_CLIENTS];
    s_spare[RESULT_BYTES] = &s_mem->bytes[PLAYLIST_CLIENTS];
    s_lock = lock;
    s_console_events = console;
    s_ring = rb;
    // The stack in PSRAM: see MEMORY for why that is safe here.
    ok = xTaskCreatePinnedToCoreWithCaps(
             playlist_task, "playlist", TASK_STACK_BYTES, NULL, TASK_PRIORITY,
             &s_task, TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
         pdPASS;
  }
  if (!ok) {
    ESP_LOGE(TAG,
             "no memory for the playlist task (%u bytes of PSRAM wanted, %u "
             "free): running without playlists",
             (unsigned)(sizeof(memory_t) + PLAYLIST_REQUEST_QUEUE_BYTES +
                        TASK_STACK_BYTES),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // Nothing has used these yet. (A control block made static over
    // storage that is now freed is never touched again: s_ring and
    // s_console_events stay NULL.)
    s_ring = NULL;
    s_console_events = NULL;
    s_lock = NULL;
    s_task = NULL;
    if (lock != NULL) {
      vSemaphoreDelete(lock);
    }
    heap_caps_free(events);
    heap_caps_free(ring);
    heap_caps_free(s_mem);
    s_mem = NULL;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG,
           "%u bytes of PSRAM (buffers %u, request queue %u, stack %u); free "
           "now: psram %u, internal %u",
           (unsigned)(sizeof(memory_t) + PLAYLIST_REQUEST_QUEUE_BYTES +
                      CONSOLE_EVENT_QUEUE_LENGTH * sizeof(playlist_event_t) +
                      TASK_STACK_BYTES),
           (unsigned)sizeof(memory_t), (unsigned)PLAYLIST_REQUEST_QUEUE_BYTES,
           (unsigned)TASK_STACK_BYTES,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return ESP_OK;
}
