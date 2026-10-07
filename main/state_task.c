#include "state_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "library_task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pcm.h"
#include "storage.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Design notes
//
// ONE TASK, WOKEN BY CHANGES
//
// The task sleeps on a notification until something is handed over or asked
// for. It runs at priority 2 on core 0: below the UI, the console and the
// library task, so nothing waits for it, and away from the audio tasks on
// core 1. Nothing is polled; asleep it costs nothing.
//
// A hand-over only notes the new value and notifies the task. The task then
// waits for quiet (STATE_SETTLE_MS since the last hand-over, or
// STATE_DEFER_MAX_MS since the first that is still unwritten, whichever
// comes first) and writes whatever is new: flash first, then the queue file.
// A hand-over that arrives while it is writing is not lost: its notification
// stays pending, the task goes round again and writes it after the next
// quiet period. What is written is always the latest that was handed over at
// the moment the write began.
//
// SETTINGS AND POSITION: FLASH (NVS)
//
// Namespace "dap", five items:
//
//   volume    u8   0..20
//   shuffle   u8   0 or 1
//   loop      u8   play_loop_t
//   backlight u8   standby_backlight_t (15 s, 30 s, 60 s, always on)
//   position  u64  queue position << 48 | track number << 32 | elapsed ms
//
// The UI's side is two small values behind a spinlock (s_lock): the settings
// and the position as they were last handed over. The task copies them out
// under the lock and compares each with what it knows to be in flash; only
// an item that differs is written. So a value that has not changed costs no
// flash, whoever calls and however often.
//
// The position is one item because NVS replaces an item whole: after a
// power cut it reads back as the old value or the new one. Three items
// could be left as the new position with the old time.
//
// The position is written every 30 seconds while a track plays, and each
// write uses one 32-byte entry of a 4 KB flash page (126 entries). When a
// page is full NVS copies what is live out of the oldest and erases it:
// about once an hour of playing, each of the six pages in its turn. That
// erase is the one slow flash operation there is (see WHAT THE AUDIO HEARS).
//
// If flash cannot be opened at start-up because its format is one this
// firmware does not know or it has no free page left
// (ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_ERR_NVS_NO_FREE_PAGES), it is erased
// and opened again; that is logged once and the defaults are used. (An area
// that is blank, or full of rubbish, NVS opens by itself.) Any other error
// (no memory, say) may be passing, so nothing is erased: the error is logged
// once and the player runs with the defaults and saves no settings, as it
// does if flash still cannot be opened after an erase. A write
// that fails is logged and forgotten: the value is tried again when it next
// changes.
//
// THE QUEUE: TWO COPIES, THEN A FILE
//
// The UI's queue changes under the user's fingers, and play_queue_save reads
// the queue it is given for as long as it runs. So the file is never written
// from the UI's queue: it is written from a copy. There are two (s_copy),
// each two arrays of STATE_QUEUE_CAPACITY track numbers: 32,768 bytes a
// copy, 65,536 in all, allocated once, in PSRAM.
//
//   - s_latest says which copy holds the queue as it was last handed over.
//   - s_claimed says which copy the task is using (writing it to the card,
//     or as scratch for the console's `state`), or that it is using none.
//
// s_queue_lock, a mutex, guards those and the rest of the queue's
// book-keeping. The rules for the arrays:
//
//   - The UI (state_task_save_queue, state_task_restore_queue) does all its
//     work with the lock held: it compares its queue with the latest copy
//     and, if they differ, copies it into the copy that is NOT claimed and
//     makes that one the latest. If the latest is not claimed it is
//     overwritten where it lies (an unwritten older request is replaced);
//     if the task is busy writing it, the new queue goes to the other copy.
//   - The task takes the lock only to claim a copy or to give it back,
//     which is a few instructions. Between the two it reads the claimed
//     copy's arrays without the lock, for as long as the card takes.
//
// Why no torn queue can be written: the file is written from the claimed
// copy, and the UI never writes to a claimed copy, so that copy cannot
// change while it is written. And a copy only ever holds a whole queue: it
// is filled inside one call from the UI task, under the lock, from the UI's
// own queue, which the UI is not changing during its own call. The count,
// position, shuffle and loop that go into the file's header are the task's
// own note of them, taken with the claim.
//
// Why the UI's cost is bounded: the task holds the lock for a few
// instructions at a time and never across card I/O, so the UI does not wait
// for it. Its own work is one comparison of the queue and at most one copy:
// for a full queue of 8,192 tracks, measured while a track plays, 2 to 14
// ms; in proportion for a shorter one (half a millisecond for the 1,028
// tracks of the test card).
//
// Why PSRAM, slow as it is (that cost is nearly all PSRAM: the same work
// in internal RAM takes 0.2 to 0.6 ms): there is no internal RAM for it.
// With a track playing, 39 KB of internal data RAM is free, in pieces of
// at most 31 KB (measured; `stat`'s heap_int also counts 74 KB that can
// only hold code). The first design had one copy internal; the I2S driver
// then could not allocate its buffers and nothing played. The card does
// not mind: FatFs's own sector buffer is in PSRAM as well, and the SD
// driver copies such data through a DMA buffer of its own. See MEMORY
// DURING A SAVE for the little internal RAM a save does take.
//
// How "unchanged" is told: by comparing, in the UI's call, the count,
// shuffle, the index checksum and the bytes of both orders with the latest
// copy. It is exact (no hash that could collide), it costs the UI the same
// pass over the queue that copying it would, and it is right because the
// latest copy is what the file holds or is about to: the task writes the
// latest copy and nothing else, a queue restored at power-on is put there
// too, and if a write fails the latest copy stays the queue the file is
// about to hold: it is written again (A FAILED SAVE) unless a newer
// hand-over replaces it first. A change that is only the position or
// loop is noted in the latest copy (the next file written carries it) but
// writes nothing.
//
// RESTORING: THE FILE IS READ BEFORE IT IS WANTED
//
// The UI restores the queue the moment it first sees an index, and must not
// wait for the card. So the files are read into memory as soon as the card
// is mounted (state_task_card_ready, before the index is loaded), as bytes:
// whether they are any good can only be told with the index.
// state_task_restore_queue then runs play_queue_restore over those bytes
// through a stream that reads memory. The bytes are in PSRAM (33 KB at most
// per file, 27 ms to read; 4 ms to find that there is no file), and are
// freed by this task once the restore has been made or can no longer be.
//
// SAVING THE FILE, AND A POWER CUT AT EACH STEP
//
// As for the index (library_task.c), and for the same reason (FAT cannot
// rename onto an existing file):
//
//   1. write the whole queue to queue.bin.tmp and close it
//   2. delete queue.bin
//   3. rename queue.bin.tmp to queue.bin
//
// At power-on both files are read, and the restore tries the .tmp first: it
// exists only between the start of 1 and the end of 3, so if it is whole
// and for this index it is the newest queue there is. What a restore finds,
// by where the power went:
//
//   during 1   .tmp is short or damaged and fails play_queue_restore's
//              checks; queue.bin is the queue before the change, and is
//              used. The .tmp is deleted. The last change is lost.
//   after 1    both are whole. The .tmp is used; 2 and 3 are finished off.
//   during 2,  queue.bin is gone or going; .tmp is whole, is used, and is
//   after 2    renamed.
//   during 3   both names point at the same data for a moment. The .tmp is
//              used; finishing the job deletes queue.bin, which frees the
//              data they share, and renames .tmp. The file then reads back
//              correctly until something is written over it, after which
//              its checksum fails and the queue starts empty. The window is
//              two sector writes long (the index has the same one).
//   after 3    the normal case: only queue.bin.
//
// Every case ends with a queue that passed all of play_queue_restore's
// checks for the index that is loaded, or with an empty queue. The tidying
// (deleting or renaming the .tmp) is done by this task after the restore; if
// no restore is ever made, the next save writes a new .tmp over the old one
// and replaces queue.bin, which tidies as well.
//
// Flash and the card are written at different moments, so a power cut can
// leave a position that belongs to an older or a newer queue than the file.
// state_task.h (WHO IS BELIEVED) says how the caller puts them together: a
// saved position is used as it is only if the queue has the saved track
// there; otherwise the saved track is looked for in the queue.
//
// A FAILED SAVE
//
// The file is written only when the queue's contents change, so a write
// that fails (the card pausing too long, the card full) is not followed by
// another hand-over that would try again: it must be retried here, or the
// old file would stand for as long as the queue stays as it is. So when a
// write fails and no newer queue has been handed over meanwhile, the copy
// is put back as unsaved and marked as a retry (s_retry). It is written
// again once its back-off is over: 30 s after the failure, doubled at each
// failure in a row up to 8 minutes, and back to 30 s after a write that
// works. The task wakes by itself for that (the back-off is one more time
// it sleeps until, nothing is polled); a save pass that comes earlier, for
// a position say, leaves the retry alone. So a card that keeps failing
// costs one attempt and two log lines per back-off, never a loop.
//
// A newer hand-over supersedes the retry: hand_over() clears s_retry, so
// the new queue is written on its own pass as usual, and the failed copy is
// never written. If a newer queue was already waiting when the write
// failed, nothing is put back at all.
//
// With no card mounted (it is mounted once, at start-up, and never taken
// away, so this is only for a card that never came) the write stops at
// that check and logs it, as before; the task does not set its own timer
// then, and the copy waits for the first save pass after the back-off.
//
// MEMORY DURING A SAVE
//
// The stream the file is written through has no stdio buffer (_IONBF).
// play_queue_save makes three large writes (header, play order, original
// order), so a buffer would only cut them into 4 KB pieces, and it would be
// 4 KB of internal RAM (stdio allocates it at the first write, and an
// allocation that small goes to internal RAM first) taken while a track
// plays, which is when internal RAM is shortest. Measured, playing the
// 305 s FLAC with a queue of 1,028 tracks: 39,003 bytes of internal data
// RAM free, largest piece 30,720, before the save; during it, with the
// buffer, 34,735 and 26,624; without, 38,819 and 30,720 (FatFs's FIL is in
// PSRAM; the low-water mark fell by 1.7 KB in all, the SD driver's and
// FatFs's short-lived pieces included). After a switch from an
// MP3 to that FLAC the largest piece was only 14,336 bytes, so the 4 KB
// would have been more than a quarter of it. The cost is time: a save of
// 1,028 tracks took 51 to 73 ms instead of 40 to 53 (eight of each, the
// FLAC playing), on this task, which the audio does not hear.
//
// WHAT THE AUDIO HEARS
//
// Nothing, but only because the I2S interrupt is built to run while flash
// is written (CONFIG_I2S_ISR_IRAM_SAFE; sdkconfig.defaults says why). The
// measurements, with the 305 s track playing while 1,193 flash saves and
// 200 saves of a full queue were forced through in four minutes:
//
//   flash, an item written for the first time   1.6 ms
//   flash, one item replaced                    10.5 ms
//   flash, three or four items replaced         12 ms typical, 15 ms at most
//   flash, with a page erase                    29 ms more: 41 to 45 ms
//                                               (27 erases in the run)
//   queue file, 8,192 tracks                    90 ms typical, nine in ten
//                                               under 122 ms, the longest
//                                               0.45 s (0.54 s in another
//                                               run): the card pausing
//   queue file, 300 tracks                      26 to 48 ms
//
// The frame count was exact and there was no underrun.
//
// While flash is written or erased, code in flash cannot run on either
// core, and interrupts that live there wait. The player rides out the 29 ms
// of an erase on its buffers. The I2S interrupt could not: it hands each
// buffer it has sent back to be refilled, one buffer per interrupt, every
// 11 ms. Held off for 29 ms it missed a buffer at every erase, and the DMA
// then plays that buffer's old contents again: 11 ms of sound from 180 ms
// earlier. Neither the frame count nor the underrun count can see that;
// timing the interrupt did (a gap of 29 to 32 ms at each erase, and one
// interrupt fewer). With the interrupt in RAM no gap was longer than 11.5
// ms in the whole run.
//
// The card is shared with the decoder, which waits while the queue file is
// written. The player's ring buffer holds over two seconds, and its lowest
// level in the run was nine tenths full.

#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY 2
#define TASK_CORE 0

#define FLASH_NAMESPACE "dap"
#define KEY_VOLUME "volume"
#define KEY_SHUFFLE "shuffle"
#define KEY_LOOP "loop"
#define KEY_BACKLIGHT "backlight"
#define KEY_POSITION "position"

#define DEFAULT_VOLUME 10 // the player's own starting level

#define QUEUE_PATH STORAGE_MOUNT_POINT LIBRARY_DIR "/queue.bin"
#define TMP_PATH QUEUE_PATH ".tmp"
#define DIR_PATH STORAGE_MOUNT_POINT LIBRARY_DIR

#define ORDER_BYTES (STATE_QUEUE_CAPACITY * sizeof(uint16_t))
// No queue file is larger than its two orders and a header. A file that is
// (the header is 32 bytes; the slack is so that this file need not know)
// is not read at all.
#define FILE_BYTES_MAX (2 * ORDER_BYTES + 4096)

#define REQUEST_SAVE (1u << 0)   // something was handed over
#define REQUEST_CARD (1u << 1)   // the card is mounted: read the files
#define REQUEST_TIDY (1u << 2)   // the files read at start-up are done with
#define REQUEST_REPORT (1u << 3) // the console's `state`
#define REQUEST_CLEAR (1u << 4)  // the console's `state clear`

// A queue file that could not be written is tried again after this long,
// doubled at each failure in a row up to the most. See A FAILED SAVE.
#define QUEUE_RETRY_FIRST_MS 30000
#define QUEUE_RETRY_MAX_MS 480000

static const char *TAG = "state";

static TaskHandle_t s_task;
static SemaphoreHandle_t s_card_done; // state_task_card_ready waits on it
static SemaphoreHandle_t s_answered;  // the console's two calls wait on it

// --- Settings and position ---------------------------------------------------

// What was read at start-up. Written before the task exists, never after.
static state_settings_t s_loaded_settings = {
    .volume = DEFAULT_VOLUME, .backlight = STANDBY_BACKLIGHT_30S};
static state_position_t s_loaded_position;
static bool s_loaded_has_position;

// What was last handed over. s_lock guards these four, for a few
// instructions at a time.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_want_settings; // settings have been handed over
static bool s_want_position;
static state_settings_t s_settings;
static uint64_t s_position;

// Flash, and what it is known to hold: -1 for an item that is not there or
// whose last write failed. Only the task uses these once it is running.
static nvs_handle_t s_nvs;
static bool s_flash_ok;
static int s_flash_volume = -1;
static int s_flash_shuffle = -1;
static int s_flash_loop = -1;
static int s_flash_backlight = -1;
static bool s_flash_has_position;
static uint64_t s_flash_position;

static uint64_t pack_position(const state_position_t *p) {
  return (uint64_t)p->position << 48 | (uint64_t)p->track << 32 |
         p->elapsed_ms;
}

static void unpack_position(uint64_t packed, state_position_t *p) {
  p->position = (uint32_t)(packed >> 48);
  p->track = (uint32_t)(packed >> 32) & 0xFFFFu;
  p->elapsed_ms = (uint32_t)packed;
}

void state_task_loaded_settings(state_settings_t *out) {
  *out = s_loaded_settings;
}

bool state_task_loaded_position(state_position_t *out) {
  *out = s_loaded_position;
  return s_loaded_has_position;
}

void state_task_save_settings(const state_settings_t *settings) {
  if (s_task == NULL) {
    return;
  }
  // What goes to flash is what could have come from it.
  state_settings_t s = *settings;
  if (s.volume < 0) {
    s.volume = 0;
  } else if (s.volume > PCM_VOLUME_MAX) {
    s.volume = PCM_VOLUME_MAX;
  }
  if (s.loop != PLAY_LOOP_ALL && s.loop != PLAY_LOOP_ONE) {
    s.loop = PLAY_LOOP_OFF;
  }
  if ((unsigned)s.backlight >= STANDBY_BACKLIGHT_COUNT) {
    s.backlight = STANDBY_BACKLIGHT_30S;
  }
  portENTER_CRITICAL(&s_lock);
  bool changed = !s_want_settings || s.volume != s_settings.volume ||
                 s.shuffle != s_settings.shuffle || s.loop != s_settings.loop ||
                 s.backlight != s_settings.backlight;
  s_settings = s;
  s_want_settings = true;
  portEXIT_CRITICAL(&s_lock);
  if (changed) {
    xTaskNotify(s_task, REQUEST_SAVE, eSetBits);
  }
}

void state_task_save_position(const state_position_t *position) {
  if (s_task == NULL || position->position > UINT16_MAX ||
      position->track > UINT16_MAX) {
    return;
  }
  uint64_t packed = pack_position(position);
  portENTER_CRITICAL(&s_lock);
  bool changed = !s_want_position || packed != s_position;
  s_position = packed;
  s_want_position = true;
  portEXIT_CRITICAL(&s_lock);
  if (changed) {
    xTaskNotify(s_task, REQUEST_SAVE, eSetBits);
  }
}

// Open flash and the namespace. See the design notes for the erase.
static bool open_flash(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG,
             "saved settings cannot be read (%s): erasing them and starting "
             "from the defaults",
             esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }
  if (err == ESP_OK) {
    err = nvs_open(FLASH_NAMESPACE, NVS_READWRITE, &s_nvs);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "flash cannot be used (%s): settings will not be saved",
             esp_err_to_name(err));
    return false;
  }
  return true;
}

// One u8 item: what flash holds, or -1.
static int read_u8(const char *key) {
  uint8_t value;
  return nvs_get_u8(s_nvs, key, &value) == ESP_OK ? value : -1;
}

// Read everything into s_flash_* and, where it is a value the setting can
// have, into s_loaded_*.
static void load_flash(void) {
  s_flash_volume = read_u8(KEY_VOLUME);
  s_flash_shuffle = read_u8(KEY_SHUFFLE);
  s_flash_loop = read_u8(KEY_LOOP);
  s_flash_backlight = read_u8(KEY_BACKLIGHT);
  s_flash_has_position =
      nvs_get_u64(s_nvs, KEY_POSITION, &s_flash_position) == ESP_OK;

  if (s_flash_volume >= 0 && s_flash_volume <= PCM_VOLUME_MAX) {
    s_loaded_settings.volume = s_flash_volume;
  }
  if (s_flash_shuffle == 0 || s_flash_shuffle == 1) {
    s_loaded_settings.shuffle = (s_flash_shuffle == 1);
  }
  if (s_flash_loop >= PLAY_LOOP_OFF && s_flash_loop <= PLAY_LOOP_ONE) {
    s_loaded_settings.loop = (play_loop_t)s_flash_loop;
  }
  if (s_flash_backlight >= 0 && s_flash_backlight < STANDBY_BACKLIGHT_COUNT) {
    s_loaded_settings.backlight = (standby_backlight_t)s_flash_backlight;
  }
  if (s_flash_has_position) {
    unpack_position(s_flash_position, &s_loaded_position);
    s_loaded_has_position = true;
  }
}

// Write one u8 item if flash does not already hold `value`.
static void save_u8(const char *key, int value, int *in_flash, bool *wrote) {
  if (*in_flash == value) {
    return;
  }
  esp_err_t err = nvs_set_u8(s_nvs, key, (uint8_t)value);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "could not save %s (%s)", key, esp_err_to_name(err));
    *in_flash = -1;
    return;
  }
  *in_flash = value;
  *wrote = true;
}

// Write to flash whatever was handed over and is not there yet.
static void save_flash(void) {
  portENTER_CRITICAL(&s_lock);
  bool want_settings = s_want_settings;
  bool want_position = s_want_position;
  state_settings_t settings = s_settings;
  uint64_t position = s_position;
  portEXIT_CRITICAL(&s_lock);
  if (!s_flash_ok) {
    return;
  }

  int64_t start = esp_timer_get_time();
  bool wrote = false;
  bool wrote_position = false;
  if (want_settings) {
    save_u8(KEY_VOLUME, settings.volume, &s_flash_volume, &wrote);
    save_u8(KEY_SHUFFLE, settings.shuffle ? 1 : 0, &s_flash_shuffle, &wrote);
    save_u8(KEY_LOOP, (int)settings.loop, &s_flash_loop, &wrote);
    save_u8(KEY_BACKLIGHT, (int)settings.backlight, &s_flash_backlight,
            &wrote);
  }
  if (want_position &&
      !(s_flash_has_position && s_flash_position == position)) {
    esp_err_t err = nvs_set_u64(s_nvs, KEY_POSITION, position);
    if (err == ESP_OK) {
      s_flash_position = position;
      s_flash_has_position = true;
      wrote_position = true;
    } else {
      ESP_LOGW(TAG, "could not save the position (%s)", esp_err_to_name(err));
      s_flash_has_position = false;
    }
  }
  if (!wrote && !wrote_position) {
    return;
  }
  esp_err_t err = nvs_commit(s_nvs);
  uint32_t us = (uint32_t)(esp_timer_get_time() - start);
  if (err != ESP_OK) {
    // Nothing is known about what flash holds now: write it all next time.
    ESP_LOGW(TAG, "could not save to flash (%s)", esp_err_to_name(err));
    s_flash_volume = s_flash_shuffle = s_flash_loop = s_flash_backlight = -1;
    s_flash_has_position = false;
    return;
  }
  // One line per save, naming only what was written.
  char text[128];
  int n = 0;
  text[0] = '\0';
  if (wrote) {
    n = snprintf(text, sizeof(text),
                 " volume=%d shuffle=%d loop=%d backlight=%d", s_flash_volume,
                 s_flash_shuffle, s_flash_loop, s_flash_backlight);
  }
  if (wrote_position) {
    state_position_t p;
    unpack_position(position, &p);
    snprintf(text + n, sizeof(text) - (size_t)n,
             " position=%" PRIu32 " track=%" PRIu32 " elapsed_ms=%" PRIu32,
             p.position, p.track, p.elapsed_ms);
  }
  ESP_LOGI(TAG, "saved%s in %" PRIu32 " us", text, us);
}

// --- The queue ---------------------------------------------------------------

// A queue and the index its track numbers belong to.
typedef struct {
  play_queue_t q;
  uint32_t index; // library_checksum of that index
} snapshot_t;

// The files found on the card at start-up, as bytes, until they are restored
// from or given up. [0] is the .tmp, [1] is queue.bin; NULL for one that is
// not there or cannot be a queue file.
enum { FOUND_TMP, FOUND_QUEUE, FOUND_FILES };

// What to do on the card once the restore has been made.
typedef enum {
  TIDY_NOTHING,
  TIDY_FINISH,  // the .tmp was the queue: put it in queue.bin's place
  TIDY_DISCARD, // the .tmp was no good: delete it
} tidy_t;

// s_queue_lock guards everything down to s_tidy, and the arrays of the two
// copies as THE QUEUE in the design notes says.
static SemaphoreHandle_t s_queue_lock;
static snapshot_t s_copy[2];
static int s_latest;       // which copy holds the queue as last handed over
static bool s_known;       // it does hold one (else: anything differs)
static bool s_unsaved;     // and the file does not hold it yet
static bool s_retry;       // because writing it failed (see A FAILED SAVE)
static int s_claimed = -1; // the copy the task is using, or -1
static uint8_t *s_found[FOUND_FILES];
static size_t s_found_bytes[FOUND_FILES];
static bool s_found_tmp;  // there was a .tmp file, usable or not
static bool s_found_open; // the files have been read and not yet given up
static tidy_t s_tidy;

static bool same_contents(const play_queue_t *a, const play_queue_t *b) {
  size_t bytes = a->count * sizeof(uint16_t);
  return a->count == b->count && a->shuffle == b->shuffle &&
         memcmp(a->play, b->play, bytes) == 0 &&
         memcmp(a->original, b->original, bytes) == 0;
}

// Make the copy the UI may write (never the one the task has claimed) the
// latest, holding `q`. With the lock held.
static void hand_over(const play_queue_t *q, uint32_t index_checksum) {
  if (s_claimed == s_latest) {
    s_latest = 1 - s_latest;
  }
  play_queue_t *to = &s_copy[s_latest].q;
  size_t bytes = q->count * sizeof(uint16_t);
  memcpy(to->play, q->play, bytes);
  memcpy(to->original, q->original, bytes);
  to->count = q->count;
  to->position = q->position;
  to->shuffle = q->shuffle;
  to->loop = q->loop;
  s_copy[s_latest].index = index_checksum;
  s_known = true;
  s_retry = false; // a new queue: written on its own pass, not a retry's
}

void state_task_save_queue(const play_queue_t *q, uint32_t index_checksum) {
  if (s_task == NULL || q->count > STATE_QUEUE_CAPACITY) {
    return;
  }
  uint32_t request = 0;
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  snapshot_t *latest = &s_copy[s_latest];
  if (s_known && latest->index == index_checksum &&
      same_contents(q, &latest->q)) {
    // Not worth a file: these two travel with the next one that is. (If the
    // task is writing this copy now, it took its own note of them first.)
    latest->q.position = q->position;
    latest->q.loop = q->loop;
  } else {
    hand_over(q, index_checksum);
    s_unsaved = true;
    request = REQUEST_SAVE;
  }
  if (s_found_open) {
    // The queue has moved on from whatever the card held at start-up.
    s_found_open = false;
    request |= REQUEST_TIDY;
  }
  xSemaphoreGive(s_queue_lock);
  if (request != 0) {
    xTaskNotify(s_task, request, eSetBits);
  }
}

// A file's bytes as a stream, for play_queue_restore.
typedef struct {
  const uint8_t *at;
  size_t left;
} bytes_t;

static int read_bytes(void *cookie, char *to, int wanted) {
  bytes_t *bytes = cookie;
  size_t n = (size_t)wanted;
  if (n > bytes->left) {
    n = bytes->left;
  }
  memcpy(to, bytes->at, n);
  bytes->at += n;
  bytes->left -= n;
  return (int)n;
}

// play_queue_restore over a file's bytes. On failure `q` is empty.
static bool restore_bytes(play_queue_t *q, const uint8_t *found, size_t n,
                          uint32_t checksum, uint32_t tracks) {
  bytes_t bytes = {.at = found, .left = n};
  // NULL if there are no bytes or no memory for the stream: the restore
  // then fails, and empties the queue, by itself.
  FILE *f = (found != NULL) ? fropen(&bytes, read_bytes) : NULL;
  bool ok = play_queue_restore(q, checksum, tracks, f);
  if (f != NULL) {
    fclose(f);
  }
  return ok;
}

bool state_task_restore_queue(play_queue_t *q, const library_t *lib) {
  if (s_task == NULL || lib == NULL) {
    play_queue_clear(q);
    return false;
  }
  uint32_t checksum = library_checksum(lib);
  uint32_t tracks = library_track_count(lib);

  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  if (!s_found_open) {
    xSemaphoreGive(s_queue_lock);
    play_queue_clear(q);
    return false;
  }
  s_found_open = false;
  // See SAVING THE FILE for why the .tmp comes first.
  bool ok = restore_bytes(q, s_found[FOUND_TMP], s_found_bytes[FOUND_TMP],
                          checksum, tracks);
  if (ok) {
    s_tidy = TIDY_FINISH;
  } else {
    s_tidy = s_found_tmp ? TIDY_DISCARD : TIDY_NOTHING;
    ok = restore_bytes(q, s_found[FOUND_QUEUE], s_found_bytes[FOUND_QUEUE],
                       checksum, tracks);
  }
  if (ok && q->count <= STATE_QUEUE_CAPACITY) {
    // The file holds this queue: handing it over again must not rewrite it.
    hand_over(q, checksum);
    s_unsaved = false;
  }
  xSemaphoreGive(s_queue_lock);
  xTaskNotify(s_task, REQUEST_TIDY, eSetBits);
  return ok;
}

// --- The task's side: the card -------------------------------------------------

static uint32_t us_since(int64_t start_us) {
  return (uint32_t)(esp_timer_get_time() - start_us);
}

// The whole of the file at `path`, in a block of PSRAM the caller frees.
// NULL if there is no such file (`*exists` says which), it is too large to
// be a queue file, or it cannot be read.
static uint8_t *read_file(const char *path, size_t *bytes, bool *exists) {
  struct stat st;
  *bytes = 0;
  *exists = (stat(path, &st) == 0);
  if (!*exists || st.st_size <= 0 || st.st_size > (off_t)FILE_BYTES_MAX) {
    return NULL;
  }
  size_t size = (size_t)st.st_size;
  uint8_t *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int fd = (block != NULL) ? open(path, O_RDONLY) : -1;
  ssize_t got = (fd >= 0) ? read(fd, block, size) : -1;
  if (fd >= 0) {
    close(fd);
  }
  if (got != (ssize_t)size) {
    ESP_LOGW(TAG, "could not read %s", path);
    heap_caps_free(block);
    return NULL;
  }
  *bytes = size;
  return block;
}

// The card has just been mounted: read both files (see RESTORING).
static void read_found(void) {
  int64_t start = esp_timer_get_time();
  uint8_t *found[FOUND_FILES];
  size_t bytes[FOUND_FILES];
  bool tmp_exists, queue_exists;
  found[FOUND_TMP] = read_file(TMP_PATH, &bytes[FOUND_TMP], &tmp_exists);
  found[FOUND_QUEUE] =
      read_file(QUEUE_PATH, &bytes[FOUND_QUEUE], &queue_exists);
  ESP_LOGI(TAG, "queue file: %u bytes%s, read in %" PRIu32 " us",
           (unsigned)bytes[FOUND_QUEUE],
           tmp_exists ? " (and an unfinished save)" : "", us_since(start));

  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  bool late = s_known; // a queue has been handed over already
  if (!late) {
    for (int i = 0; i < FOUND_FILES; i++) {
      s_found[i] = found[i];
      s_found_bytes[i] = bytes[i];
    }
    s_found_tmp = tmp_exists;
    s_found_open = true;
  }
  xSemaphoreGive(s_queue_lock);
  if (late) {
    heap_caps_free(found[FOUND_TMP]);
    heap_caps_free(found[FOUND_QUEUE]);
  }
}

// Steps 2 and 3 of a save. False (and why, in the log) if the new file did
// not end up under its proper name.
static bool replace_queue_file(void) {
  if (unlink(QUEUE_PATH) != 0 && errno != ENOENT) {
    ESP_LOGW(TAG, "could not remove the old queue file (%s)", strerror(errno));
    return false;
  }
  if (rename(TMP_PATH, QUEUE_PATH) != 0) {
    ESP_LOGW(TAG, "could not rename the new queue file (%s)", strerror(errno));
    return false;
  }
  return true;
}

// Claim a copy for the task: the latest (to write it) or the other one (as
// scratch). Until release() the UI does not write to it. With the lock held.
static play_queue_t claim(bool latest, uint32_t *index) {
  s_claimed = latest ? s_latest : 1 - s_latest;
  if (index != NULL) {
    *index = s_copy[s_claimed].index;
  }
  // The arrays are the copy's own; the count and the rest are the task's
  // note of them, which the UI's later calls do not change.
  return s_copy[s_claimed].q;
}

// Retrying a failed save (see A FAILED SAVE). Only the task uses these.
static bool s_failing;         // the last write failed
static bool s_retry_armed;     // the task wakes by itself to retry
static TickType_t s_failed_at; // when the last write failed
static TickType_t s_backoff = pdMS_TO_TICKS(QUEUE_RETRY_FIRST_MS);

// The latest copy did not reach the file. Unless a newer queue is waiting to
// be written anyway (it supersedes this one, and goes on its own pass), the
// copy is put back as unsaved, to be retried after the back-off. The latest
// copy still holds the queue the file is about to hold, so the UI's
// comparisons with it stay right. With the lock held.
static void file_is_stale(void) {
  if (!s_unsaved && s_known) {
    s_unsaved = true;
    s_retry = true;
  }
}

// After a write that failed: when to try again. Each failure in a row
// doubles the wait, from QUEUE_RETRY_FIRST_MS up to QUEUE_RETRY_MAX_MS. The
// task's own timer is set only if the card is mounted; with no card the copy
// waits for the next save pass after the back-off, which looks for the card
// before anything else.
static void back_off(void) {
  if (s_failing) {
    s_backoff *= 2;
    if (s_backoff > pdMS_TO_TICKS(QUEUE_RETRY_MAX_MS)) {
      s_backoff = pdMS_TO_TICKS(QUEUE_RETRY_MAX_MS);
    }
  }
  s_failing = true;
  s_failed_at = xTaskGetTickCount();
  s_retry_armed = storage_is_mounted();
  ESP_LOGW(TAG, "the queue will be saved again in %" PRIu32 " s",
           (uint32_t)(pdTICKS_TO_MS(s_backoff) / 1000));
}

// A write succeeded, or there is nothing left to retry.
static void back_off_done(void) {
  s_failing = false;
  s_retry_armed = false;
  s_backoff = pdMS_TO_TICKS(QUEUE_RETRY_FIRST_MS);
}

// Whether a retry is due: the back-off since the last failure is over.
static bool retry_due(TickType_t now) {
  return (TickType_t)(now - s_failed_at) >= s_backoff;
}

// Give the claimed copy back; `stale` if the file was not written from it.
// True if the copy is now waiting to be retried.
static bool release(bool stale) {
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  s_claimed = -1;
  if (stale) {
    file_is_stale();
  }
  bool retry = s_retry;
  xSemaphoreGive(s_queue_lock);
  return retry;
}

// The files read at start-up are done with: free them and finish on the card
// what the restore decided.
static void tidy(void) {
  uint8_t *found[FOUND_FILES];
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  if (s_found_open) {
    xSemaphoreGive(s_queue_lock);
    return;
  }
  for (int i = 0; i < FOUND_FILES; i++) {
    found[i] = s_found[i];
    s_found[i] = NULL;
    s_found_bytes[i] = 0;
  }
  tidy_t what = s_tidy;
  s_tidy = TIDY_NOTHING;
  xSemaphoreGive(s_queue_lock);

  heap_caps_free(found[FOUND_TMP]);
  heap_caps_free(found[FOUND_QUEUE]);
  if (what == TIDY_FINISH) {
    ESP_LOGW(TAG, "finishing a queue save that was interrupted");
    if (!replace_queue_file()) {
      xSemaphoreTake(s_queue_lock, portMAX_DELAY);
      file_is_stale();
      bool retry = s_retry;
      xSemaphoreGive(s_queue_lock);
      if (retry) {
        back_off();
      }
    }
  } else if (what == TIDY_DISCARD) {
    ESP_LOGW(TAG, "discarding an unfinished queue save");
    unlink(TMP_PATH);
  }
}

// Step 1 of a save, then the rest: `q` to the card. False, with the reason
// logged, if the card does not now hold it under its proper name.
static bool write_queue_file(const play_queue_t *q, uint32_t index) {
  if (!storage_is_mounted()) {
    ESP_LOGW(TAG, "no card: the queue was not saved");
    return false;
  }
  FILE *f = fopen(TMP_PATH, "wb");
  if (f == NULL && errno == ENOENT) {
    // A card that has never been scanned has no folder for it yet.
    mkdir(DIR_PATH, 0775);
    f = fopen(TMP_PATH, "wb");
  }
  if (f == NULL) {
    ESP_LOGW(TAG, "could not create the queue file (%s)", strerror(errno));
    return false;
  }
  // No stdio buffer: see MEMORY DURING A SAVE.
  setvbuf(f, NULL, _IONBF, 0);
  bool saved = play_queue_save(q, index, f);
  // The close writes the file's length and its last data to the card, so
  // it can fail when the write did not.
  bool closed = (fclose(f) == 0);
  if (!saved || !closed) {
    ESP_LOGW(TAG, "could not write the queue file (%s)", strerror(errno));
    unlink(TMP_PATH); // do not leave half a file for the next start to read
    return false;
  }
  return replace_queue_file();
}

// Write the queue file if a queue was handed over that it does not hold. A
// copy whose write failed is written again only once its back-off is over.
static void save_queue(TickType_t now) {
  uint32_t index;
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  bool unsaved = s_unsaved;
  bool waiting = unsaved && s_retry && !retry_due(now);
  play_queue_t q = {0};
  if (unsaved && !waiting) {
    q = claim(true, &index);
    s_unsaved = false;
    s_retry = false;
  }
  xSemaphoreGive(s_queue_lock);
  if (!unsaved) {
    back_off_done(); // nothing to retry: a newer queue was written instead
    return;
  }
  if (waiting) {
    return;
  }

  int64_t start = esp_timer_get_time();
  bool written = write_queue_file(&q, index);
  if (written) {
    ESP_LOGI(TAG, "saved queue: %" PRIu32 " tracks in %" PRIu32 " us", q.count,
             us_since(start));
  }
  bool retry = release(!written);
  if (written) {
    back_off_done();
  } else if (retry) {
    back_off();
  }
  // Else a newer queue came while this one was written: it supersedes this
  // one and is written on its own pass, like any hand-over.
}

// --- The task's side: the console ----------------------------------------------

static state_report_t s_report; // filled by the task, copied by the caller
static bool s_cleared;          // whether the last `state clear` did it all

static inline uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static void report(state_report_t *r) {
  memset(r, 0, sizeof(*r));
  r->stack_free = uxTaskGetStackHighWaterMark(NULL);
  r->flash = s_flash_ok;
  if (s_flash_ok) {
    uint64_t packed;
    r->has_volume = (nvs_get_u8(s_nvs, KEY_VOLUME, &r->volume) == ESP_OK);
    r->has_shuffle = (nvs_get_u8(s_nvs, KEY_SHUFFLE, &r->shuffle) == ESP_OK);
    r->has_loop = (nvs_get_u8(s_nvs, KEY_LOOP, &r->loop) == ESP_OK);
    r->has_backlight =
        (nvs_get_u8(s_nvs, KEY_BACKLIGHT, &r->backlight) == ESP_OK);
    r->has_position = (nvs_get_u64(s_nvs, KEY_POSITION, &packed) == ESP_OK);
    if (r->has_position) {
      unpack_position(packed, &r->position);
    }
  }

  // Numbers only are kept past the put.
  uint32_t tracks = 0;
  const library_t *lib = library_task_get();
  if (lib != NULL) {
    r->index = true;
    r->index_checksum = library_checksum(lib);
    tracks = library_track_count(lib);
    library_task_put();
  }

  struct stat st;
  r->file_unfinished = (stat(TMP_PATH, &st) == 0);
  r->file = (stat(QUEUE_PATH, &st) == 0);
  FILE *f = r->file ? fopen(QUEUE_PATH, "rb") : NULL;
  if (f == NULL) {
    return;
  }
  r->file_bytes = (uint32_t)st.st_size;
  // The count and the index checksum, where play_queue.c's header has them
  // (bytes 16 and 12). This is for the printout only: whether a file is
  // used is play_queue_restore's decision, here and everywhere.
  uint8_t header[20];
  if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
    r->file_index = get32(header + 12);
    r->file_count = get32(header + 16);
  }
  rewind(f);
  // The trial restore goes into the copy that is not the latest.
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  play_queue_t scratch = claim(false, NULL);
  xSemaphoreGive(s_queue_lock);
  r->usable = r->index &&
              play_queue_restore(&scratch, r->index_checksum, tracks, f);
  release(false);
  fclose(f);
}

static bool clear(void) {
  // Nothing handed over is still to be written, and nothing read at
  // start-up is still to be restored.
  portENTER_CRITICAL(&s_lock);
  s_want_settings = false;
  s_want_position = false;
  portEXIT_CRITICAL(&s_lock);
  xSemaphoreTake(s_queue_lock, portMAX_DELAY);
  s_known = false;
  s_unsaved = false;
  s_retry = false;
  s_found_open = false;
  s_tidy = TIDY_NOTHING;
  xSemaphoreGive(s_queue_lock);
  back_off_done();
  tidy(); // frees what was read

  bool ok = true;
  if (s_flash_ok) {
    esp_err_t err = nvs_erase_all(s_nvs);
    if (err == ESP_OK) {
      err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "could not erase the settings (%s)", esp_err_to_name(err));
      ok = false;
    }
    s_flash_volume = s_flash_shuffle = s_flash_loop = s_flash_backlight = -1;
    s_flash_has_position = false;
  }
  if (unlink(QUEUE_PATH) != 0 && errno != ENOENT) {
    ESP_LOGW(TAG, "could not remove the queue file (%s)", strerror(errno));
    ok = false;
  }
  if (unlink(TMP_PATH) != 0 && errno != ENOENT) {
    ok = false;
  }
  ESP_LOGI(TAG, "cleared");
  return ok;
}

bool state_task_report(state_report_t *out) {
  if (s_task == NULL) {
    return false;
  }
  xTaskNotify(s_task, REQUEST_REPORT, eSetBits);
  xSemaphoreTake(s_answered, portMAX_DELAY);
  *out = s_report;
  return true;
}

bool state_task_clear(void) {
  if (s_task == NULL) {
    return false;
  }
  xTaskNotify(s_task, REQUEST_CLEAR, eSetBits);
  xSemaphoreTake(s_answered, portMAX_DELAY);
  return s_cleared;
}

void state_task_card_ready(void) {
  if (s_task == NULL) {
    return;
  }
  xTaskNotify(s_task, REQUEST_CARD, eSetBits);
  xSemaphoreTake(s_card_done, portMAX_DELAY);
}

// --- The task ------------------------------------------------------------------

// See ONE TASK, WOKEN BY CHANGES.
static void state_task(void *arg) {
  (void)arg;
  const TickType_t settle = pdMS_TO_TICKS(STATE_SETTLE_MS);
  const TickType_t defer_max = pdMS_TO_TICKS(STATE_DEFER_MAX_MS);
  bool unsaved = false; // something handed over has not been written
  TickType_t first = 0; // when the oldest of it was handed over
  TickType_t last = 0;  // and the newest

  for (;;) {
    // Sleep until asked for something or, with changes waiting, until they
    // are due: `settle` after the last one, `defer_max` after the first.
    TickType_t wait = portMAX_DELAY;
    if (unsaved) {
      TickType_t now = xTaskGetTickCount();
      TickType_t quiet = now - last;
      TickType_t waited = now - first;
      if (quiet >= settle || waited >= defer_max) {
        wait = 0;
      } else {
        wait = settle - quiet;
        if (wait > defer_max - waited) {
          wait = defer_max - waited;
        }
      }
    }
    if (s_retry_armed) {
      // A failed queue save is due again at the end of its back-off.
      TickType_t since = xTaskGetTickCount() - s_failed_at;
      TickType_t left = (since >= s_backoff) ? 0 : s_backoff - since;
      if (left < wait) {
        wait = left;
      }
    }
    uint32_t request = 0;
    xTaskNotifyWait(0, UINT32_MAX, &request, wait);
    TickType_t now = xTaskGetTickCount();

    if (request & REQUEST_SAVE) {
      if (!unsaved) {
        unsaved = true;
        first = now;
      }
      last = now;
    }
    if (request & REQUEST_CARD) {
      read_found();
      xSemaphoreGive(s_card_done);
    }
    if (request & REQUEST_TIDY) {
      tidy();
    }
    if (request & REQUEST_CLEAR) {
      s_cleared = clear();
      unsaved = false;
      xSemaphoreGive(s_answered);
    }
    if (request & REQUEST_REPORT) {
      report(&s_report);
      xSemaphoreGive(s_answered);
    }
    if (unsaved && ((TickType_t)(now - last) >= settle ||
                    (TickType_t)(now - first) >= defer_max)) {
      // A hand-over from here on has its own notification and is written
      // on a later pass, if these writes do not already include it.
      unsaved = false;
      save_flash();
      save_queue(now);
    } else if (s_retry_armed && retry_due(now)) {
      save_queue(now);
    }
  }
}

static uint16_t *alloc_order(void) {
  return heap_caps_malloc(ORDER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

esp_err_t state_task_start(void) {
  int64_t start = esp_timer_get_time();
  s_flash_ok = open_flash();
  if (s_flash_ok) {
    load_flash();
  }
  ESP_LOGI(TAG,
           "loaded in %" PRIu32 " us: volume=%d shuffle=%d loop=%d "
           "backlight=%d%s",
           us_since(start), s_loaded_settings.volume,
           (int)s_loaded_settings.shuffle, (int)s_loaded_settings.loop,
           (int)s_loaded_settings.backlight,
           s_loaded_has_position ? "" : ", no position");
  if (s_loaded_has_position) {
    ESP_LOGI(TAG,
             "loaded position=%" PRIu32 " track=%" PRIu32
             " elapsed_ms=%" PRIu32,
             s_loaded_position.position, s_loaded_position.track,
             s_loaded_position.elapsed_ms);
  }

  // One-time start-up allocations.
  s_card_done = xSemaphoreCreateBinary();
  s_answered = xSemaphoreCreateBinary();
  s_queue_lock = xSemaphoreCreateMutex();
  uint16_t *orders[4];
  bool have = true;
  for (int i = 0; i < 4; i++) {
    orders[i] = alloc_order();
    have = have && orders[i] != NULL;
  }
  if (have && s_card_done != NULL && s_answered != NULL &&
      s_queue_lock != NULL) {
    play_queue_init(&s_copy[0].q, orders[0], orders[1], STATE_QUEUE_CAPACITY,
                    1);
    play_queue_init(&s_copy[1].q, orders[2], orders[3], STATE_QUEUE_CAPACITY,
                    1);
    if (xTaskCreatePinnedToCore(state_task, "state", TASK_STACK_BYTES, NULL,
                                TASK_PRIORITY, &s_task, TASK_CORE) != pdPASS) {
      s_task = NULL;
    }
  }
  if (s_task == NULL) {
    ESP_LOGE(TAG, "no memory for the state task (%u bytes free): nothing will "
                  "be saved",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    for (int i = 0; i < 4; i++) {
      heap_caps_free(orders[i]);
    }
    return ESP_ERR_NO_MEM;
  }
  return s_flash_ok ? ESP_OK : ESP_FAIL;
}
