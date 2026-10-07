#include "library_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libscan.h"
#include "player.h"
#include "player_status.h"
#include "storage.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Design notes
//
// ONE TASK, ONE ARENA
//
// The task sleeps on a notification until it is asked for something, and is
// asked twice in a normal life: at start-up (load the index, or scan if that
// fails) and when the user wants a rescan. Asleep it costs nothing: there is
// no timer and nothing is polled, so playback never notices it. It runs at
// priority 3 on core 0, below the UI and the console, which therefore stay
// responsive during a scan; the audio tasks are on core 1.
//
// The index, 1.5 MB at most, is built and kept in a block of PSRAM that is
// allocated once at start-up. A second block, so that the old index could
// stay readable while the new one is built, would take half of all the
// PSRAM this board has mapped and buy nothing: a scan stops playback and
// the screen shows the scan. So a scan builds over the old index, and
// readers are kept off it:
//
// - s_lib is the published index. library_task_get hands it out and counts
//   a reader in; library_task_put counts it out. Both are a few instructions
//   inside a critical section, so they never block.
// - Before the arena is written (a load or a build), s_lib is set to NULL,
//   after which nobody new can get in, and the task waits for the count to
//   reach zero. Only then is the arena touched.
// - s_lib is set again only when the arena holds a whole index: one that
//   library_load has checked, or one that library_builder_finish returned.
//
// So a reader sees a whole index or none, never a half-built one, provided
// it follows the rule in library_task.h (put what you got, soon).
//
// THE SCAN
//
// libscan_run walks the card (see libscan.c for how). What a track costs on
// this board, measured over files one folder below the root: 5.3 ms, of
// which opening the file is 2.1 ms, reading its tags 1.5 ms, listing its
// folder entry 1.1 ms and everything else (closing, converting the text,
// adding to the index) 0.4 ms. Those times fit single-sector reads of
// about 1 ms each (not counted directly): FAT finds a file by reading its
// folders from the root down, each time one is opened, so a file costs a
// read per folder on its path, one to get back to the listing, and one or
// two for the tags. Two things follow. A file deeper in the tree costs
// about a millisecond more per level; and a folder holding hundreds of
// files costs more per file, because finding a name reads the folder from
// its start. Starting and finishing the index add a fixed 36 ms.
//
// The walk keeps one folder open per level, nine at most, and one file. In
// this version of ESP-IDF an open folder is a separate small heap block
// (vfs_fat_dir_t) and does not take one of the `max_files` slots that the
// mount sets aside, and FatFs's own open-object table is off
// (CONFIG_FATFS_FS_LOCK=0). So the scan uses one slot (for the file it is
// reading, then for the index it saves) and the player at most one: the
// mount's five are plenty.
//
// Playback is stopped first and the scan waits for the player to be idle:
// the player's reads and the scan's would otherwise queue for the one card.
// Nothing stops a `play` during the scan; it works, and both are slower.
//
// SAVING, AND A POWER CUT AT EACH STEP
//
// FAT cannot rename onto an existing file, so replacing the index takes
// three steps, in this order:
//
//   1. write the whole index to library.idx.tmp and close it
//      (one fwrite of the arena; every result checked, fclose included)
//   2. delete library.idx
//   3. rename library.idx.tmp to library.idx
//
// At start-up the .tmp file is tried first. It only exists between the
// start of step 1 and the end of step 3, so if it holds a whole, valid
// index, that is the newest one there is. What a boot finds, by where the
// power went:
//
//   during 1   .tmp is short or damaged (its length or checksum is wrong);
//              .idx is the old index, untouched. The .tmp is deleted and
//              the old index is used: the rescan is lost, nothing else.
//   after 1    both are whole. The .tmp is used, and steps 2 and 3 are
//              finished off there and then.
//   during 2,  .idx is gone (or going); .tmp is whole. The .tmp is used
//   after 2    and renamed.
//   during 3   a rename writes the new name before removing the old one, so
//              for a moment both names point at the same data. The .tmp is
//              valid and is used; finishing the job deletes .idx, which
//              frees the data they share, and renames .tmp, leaving an
//              index whose clusters the card believes are free. It reads
//              back correctly until something else is written over them;
//              then its checksum fails and the card is rescanned. This is
//              the one place where the result is a later rescan and not a
//              good index; the window is two sector writes long.
//   after 3    the normal case: only .idx exists.
//
// In every case the next boot ends with an index that passed its checksum
// or with a rescan; no boot can use a damaged one, and none gets stuck.
// A first scan (no .idx yet) is the same with step 2 finding nothing.
//
// If saving fails (card full, write-protected, pulled out) the index just
// built is still used; the next boot will scan again.
//
// The mount must exist before any of this runs; `library_task_boot` is
// called after the card is mounted and a rescan is refused without one.

#define TASK_STACK_BYTES 8192
#define TASK_PRIORITY 3
#define TASK_CORE 0

#define DIR_PATH STORAGE_MOUNT_POINT LIBRARY_DIR
#define INDEX_PATH DIR_PATH "/" LIBRARY_INDEX_NAME
#define TMP_PATH INDEX_PATH ".tmp"

// The screen shows a count while scanning. Four times a second is as often
// as anyone can read it.
#define PROGRESS_PERIOD_MS 250
// How long to give the player to stop before scanning regardless.
#define PLAYER_STOP_WAIT_MS 3000
#define READER_COMPLAINT_MS 5000

#define REQUEST_BOOT (1u << 0)
#define REQUEST_SCAN (1u << 1)

static const char *TAG = "library";

static void *s_arena;
static TaskHandle_t s_task;
static QueueHandle_t s_status;
static SemaphoreHandle_t s_boot_done;
static SemaphoreHandle_t s_readers_gone;

// Guards the four fields below it. Held for a few instructions at a time.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const library_t *s_lib; // the published index
static uint32_t s_readers;     // gets not yet put
static bool s_waiting;         // the task is waiting for s_readers to reach 0
static bool s_busy;            // a request is queued or being served

static uint32_t s_generation;
static TickType_t s_last_progress;

// --- Readers ---------------------------------------------------------------

const library_t *library_task_get(void) {
  portENTER_CRITICAL(&s_lock);
  const library_t *lib = s_lib;
  if (lib != NULL) {
    s_readers++;
  }
  portEXIT_CRITICAL(&s_lock);
  return lib;
}

void library_task_put(void) {
  bool wake = false;
  portENTER_CRITICAL(&s_lock);
  if (s_readers > 0) {
    s_readers--;
    if (s_readers == 0 && s_waiting) {
      s_waiting = false; // so the semaphore is given once per wait
      wake = true;
    }
  }
  portEXIT_CRITICAL(&s_lock);
  if (wake) {
    xSemaphoreGive(s_readers_gone);
  }
}

bool library_task_busy(void) {
  portENTER_CRITICAL(&s_lock);
  bool busy = s_busy;
  portEXIT_CRITICAL(&s_lock);
  return busy;
}

// Take the index away from readers and wait until none is still using it.
// After this the arena is the task's alone, until publish().
static void unpublish(void) {
  portENTER_CRITICAL(&s_lock);
  s_lib = NULL;
  bool wait = (s_readers > 0);
  s_waiting = wait;
  portEXIT_CRITICAL(&s_lock);
  // No new reader can get in now, so the count only falls, and the put that
  // takes it to zero gives the semaphore exactly once.
  while (wait &&
         xSemaphoreTake(s_readers_gone, pdMS_TO_TICKS(READER_COMPLAINT_MS)) !=
             pdTRUE) {
    ESP_LOGW(TAG, "waiting for a reader to let go of the index");
  }
}

static void post_status(bool scanning, uint32_t tracks, bool full) {
  if (s_status == NULL) {
    return;
  }
  const library_status_t status = {
      .scanning = scanning,
      .full = full,
      .tracks = tracks,
      .generation = s_generation,
  };
  xQueueOverwrite(s_status, &status);
}

static void publish(const library_t *lib) {
  s_generation++;
  portENTER_CRITICAL(&s_lock);
  s_lib = lib;
  portEXIT_CRITICAL(&s_lock);
}

static uint32_t ms_since(int64_t start_us) {
  return (uint32_t)((esp_timer_get_time() - start_us) / 1000);
}

// --- Loading ---------------------------------------------------------------

static const char *result_name(library_result_t result) {
  switch (result) {
  case LIBRARY_OK:
    return "ok";
  case LIBRARY_FULL:
    return "larger than the arena";
  case LIBRARY_BAD_FILE:
    return "not a valid index";
  case LIBRARY_IO:
    return "read error";
  }
  return "?";
}

// Load the index at `path` into the arena. `*missing` says whether the
// failure was simply that there is no such file.
static library_result_t load_file(const char *path, const library_t **out,
                                  bool *missing) {
  *out = NULL;
  *missing = false;
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    *missing = (errno == ENOENT);
    return LIBRARY_IO;
  }
  library_result_t result = library_load(s_arena, LIBRARY_ARENA_BYTES, f, out);
  fclose(f);
  return result;
}

// Steps 2 and 3 of a save. False (and why, in the log) if the new index did
// not end up under its proper name.
static bool replace_index(void) {
  if (unlink(INDEX_PATH) != 0 && errno != ENOENT) {
    ESP_LOGW(TAG, "could not remove the old index (%s)", strerror(errno));
    return false;
  }
  if (rename(TMP_PATH, INDEX_PATH) != 0) {
    ESP_LOGW(TAG, "could not rename the new index (%s)", strerror(errno));
    return false;
  }
  return true;
}

// The saved index, if there is a usable one. See SAVING for the order.
static const library_t *load_index(void) {
  const library_t *lib;
  bool missing;
  library_result_t result = load_file(TMP_PATH, &lib, &missing);
  if (result == LIBRARY_OK) {
    ESP_LOGW(TAG, "finishing a save that was interrupted");
    replace_index();
    return lib;
  }
  if (!missing) {
    // There is one and it is no good: a save that never finished.
    ESP_LOGW(TAG, "discarding an unfinished save (%s)", result_name(result));
    unlink(TMP_PATH);
  }

  result = load_file(INDEX_PATH, &lib, &missing);
  if (result == LIBRARY_OK) {
    return lib;
  }
  if (missing) {
    ESP_LOGI(TAG, "no index on the card");
  } else {
    ESP_LOGW(TAG, "the index cannot be used (%s)", result_name(result));
  }
  return NULL;
}

static bool load(void) {
  int64_t start = esp_timer_get_time();
  unpublish();
  const library_t *lib = load_index();
  if (lib == NULL) {
    return false;
  }
  ESP_LOGI(TAG,
           "loaded %" PRIu32 " tracks, %" PRIu32 " albums, %" PRIu32
           " artists in %" PRIu32 " ms",
           library_track_count(lib), library_album_count(lib),
           library_artist_count(lib), ms_since(start));
  publish(lib);
  post_status(false, library_track_count(lib), false);
  return true;
}

// --- Scanning and saving ---------------------------------------------------

// Step 1 of a save, then the rest. False, with the reason logged, if the
// card does not now hold this index under its proper name.
static bool save_index(const library_t *lib) {
  if (mkdir(DIR_PATH, 0775) != 0 && errno != EEXIST) {
    ESP_LOGW(TAG, "could not create " DIR_PATH " (%s)", strerror(errno));
    return false;
  }
  FILE *f = fopen(TMP_PATH, "wb");
  if (f == NULL) {
    ESP_LOGW(TAG, "could not create the index file (%s)", strerror(errno));
    return false;
  }
  library_result_t result = library_save(lib, f);
  // The close writes the file's length and its last data to the card, so
  // it can fail when the write did not.
  bool closed = (fclose(f) == 0);
  if (result != LIBRARY_OK || !closed) {
    ESP_LOGW(TAG, "could not write the index file (%s)", strerror(errno));
    unlink(TMP_PATH); // do not leave half a file for the next boot to read
    return false;
  }
  return replace_index();
}

// Called by the scan after each track. Never waits: the status queue is
// overwritten, not queued behind.
static bool on_progress(void *ctx, const libscan_stats_t *stats) {
  (void)ctx;
  TickType_t now = xTaskGetTickCount();
  if ((TickType_t)(now - s_last_progress) >= pdMS_TO_TICKS(PROGRESS_PERIOD_MS)) {
    s_last_progress = now;
    post_status(true, stats->tracks, false);
  }
  return true;
}

// Stop playback and wait until the player has let go of the card. There is
// no event for "stopped", so this looks every tick, for a few ticks; it
// happens once per scan and not at all if nothing is playing.
static void stop_player(void) {
  player_status_t status;
  player_get_status(&status);
  if (status.state == PLAYER_STATE_IDLE) {
    return;
  }
  player_stop();
  for (int waited = 0; waited < PLAYER_STOP_WAIT_MS; waited += 10) {
    vTaskDelay(pdMS_TO_TICKS(10));
    player_get_status(&status);
    if (status.state == PLAYER_STATE_IDLE) {
      return;
    }
  }
  ESP_LOGW(TAG, "the player did not stop; scanning anyway");
}

static void scan(void) {
  post_status(true, 0, false);
  s_last_progress = xTaskGetTickCount();
  stop_player();
  unpublish();

  int64_t start = esp_timer_get_time();
  const library_t *lib = NULL;
  libscan_stats_t stats = {0};
  library_builder_t *builder =
      library_builder_begin(s_arena, LIBRARY_ARENA_BYTES);
  if (builder != NULL &&
      libscan_run(STORAGE_MOUNT_POINT, builder, on_progress, NULL, &stats)) {
    lib = library_builder_finish(builder);
  }
  if (lib == NULL) {
    // The card could not be listed at all. There is no library until the
    // next scan; the player still plays what it is given.
    ESP_LOGE(TAG, "scan failed: the card could not be read");
    post_status(false, 0, false);
    return;
  }
  uint32_t scan_ms = ms_since(start);
  ESP_LOGI(TAG,
           "scanned %" PRIu32 " tracks in %" PRIu32 " ms (%" PRIu32
           " unreadable, %" PRIu32 " skipped)",
           stats.tracks, scan_ms, stats.unreadable, stats.skipped);
  if (stats.full) {
    ESP_LOGW(TAG, "full");
  }

  int64_t save_start = esp_timer_get_time();
  if (save_index(lib)) {
    ESP_LOGI(TAG, "saved %" PRIu32 " bytes in %" PRIu32 " ms",
             library_bytes(lib), ms_since(save_start));
  } else {
    ESP_LOGW(TAG, "the index was not saved; the next start will scan again");
  }

  publish(lib);
  post_status(false, stats.tracks, stats.full);
}

// --- The task --------------------------------------------------------------

static void library_task(void *arg) {
  (void)arg;
  for (;;) {
    uint32_t request = 0;
    xTaskNotifyWait(0, UINT32_MAX, &request, portMAX_DELAY);
    if (request & REQUEST_BOOT) {
      if (!load()) {
        scan();
      }
    } else if (request & REQUEST_SCAN) {
      scan();
    }
    ESP_LOGI(TAG, "stack free %u, heap internal %u (least ever %u), psram %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    if (request & REQUEST_BOOT) {
      xSemaphoreGive(s_boot_done);
    }
  }
}

// Claim the task for one request. False if it already has one.
static bool claim(void) {
  portENTER_CRITICAL(&s_lock);
  bool claimed = !s_busy;
  s_busy = true;
  portEXIT_CRITICAL(&s_lock);
  return claimed;
}

esp_err_t library_task_start(QueueHandle_t status) {
  s_status = status;
  s_boot_done = xSemaphoreCreateBinary();
  s_readers_gone = xSemaphoreCreateBinary();
  void *arena = heap_caps_malloc(LIBRARY_ARENA_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_boot_done == NULL || s_readers_gone == NULL || arena == NULL ||
      xTaskCreatePinnedToCore(library_task, "library", TASK_STACK_BYTES, NULL,
                              TASK_PRIORITY, &s_task, TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "no memory for the library (%u bytes of PSRAM wanted, %u "
                  "free); running without one",
             (unsigned)LIBRARY_ARENA_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    heap_caps_free(arena);
    s_task = NULL;
    return ESP_ERR_NO_MEM;
  }
  s_arena = arena;
  ESP_LOGI(TAG, "arena %u bytes; free now: psram %u, heap internal %u",
           (unsigned)LIBRARY_ARENA_BYTES,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return ESP_OK;
}

void library_task_boot(void) {
  if (s_arena == NULL) {
    return;
  }
  // A rescan asked for in the instant between the mount and this call has
  // the task already; let it finish. (A sleep, and only in that case.)
  while (!claim()) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  xTaskNotify(s_task, REQUEST_BOOT, eSetBits);
  xSemaphoreTake(s_boot_done, portMAX_DELAY);
}

esp_err_t library_task_rescan(void) {
  if (s_arena == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (!storage_is_mounted() || !claim()) {
    return ESP_ERR_INVALID_STATE;
  }
  xTaskNotify(s_task, REQUEST_SCAN, eSetBits);
  return ESP_OK;
}
