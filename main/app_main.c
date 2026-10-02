#include "audio_out.h"
#include "debug_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad.h"
#include "keys.h"
#include "lcd.h"
#include "library_task.h"
#include "marquee.h"
#include "pcm.h"
#include "pins.h"
#include "player.h"
#include "player_status.h"
#include "screen.h"
#include "storage.h"
#include "translit.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define KEY_QUEUE_LENGTH 16
#define PLAYER_EVENT_QUEUE_LENGTH 16
#define UI_TASK_STACK 8192
#define UI_TASK_PRIORITY 5
#define UI_TASK_CORE 0
#define PATH_MAX_BYTES 320
// The top row's text: a file name (at most 255 characters on the card), or a
// title and an artist from the library (at most 127 each) with " - " between.
#define TITLE_MAX_BYTES 260
#define CARD_EVENT_QUEUE_LENGTH 2
#define CARD_RETRY_MS 2000
#define LIBRARY_STATUS_QUEUE_LENGTH 1 // the latest status, overwritten

static const char *TAG = "dap";
// The player's events are logged here, by the task that reads them, under
// the player's name: the audio tasks themselves log nothing while they run.
static const char *PLAYER_TAG = "player";

static QueueHandle_t s_key_events;
static QueueHandle_t s_player_events;
static QueueHandle_t s_card_events;
static QueueHandle_t s_library_status;
static QueueSetHandle_t s_ui_events; // all of the above

// What the main task tells the UI task about the card at start-up. The UI
// task owns the LCD, so the task that waits for the card cannot draw the
// "insert card" screen itself; it sends one of these instead.
typedef enum {
  CARD_EVENT_WAITING, // the card did not mount; the mount is being retried
  CARD_EVENT_MOUNTED, // the wait is over
} card_event_t;

// ---- the temporary playing screen (replaced in stage 5) ---------------------

typedef struct {
  // "Title - Artist" from the library, or the file name without folder or
  // extension for a track the library does not have. A copy: nothing here
  // points into the index.
  char title[TITLE_MAX_BYTES];
  int64_t title_since_us;      // when the track started: the marquee's zero
  uint32_t elapsed_ms;
  uint32_t duration_ms;
  bool failed; // the last `play` ended in an error
  bool no_card; // start-up is still waiting for a card that mounts
  bool scanning; // the library is being scanned: show the count
  uint32_t scan_tracks;
  uint32_t underruns; // how many have been reported so far
  bool backlight;
  char path[PATH_MAX_BYTES]; // the track last started, as the player has it
  uint32_t library_generation; // the index the title was last looked up in
} ui_t;

static const char *error_name(player_error_t error) {
  switch (error) {
  case PLAYER_ERROR_NOT_FOUND:
    return "not_found";
  case PLAYER_ERROR_UNSUPPORTED:
    return "unsupported";
  case PLAYER_ERROR_DECODE:
    return "decode";
  case PLAYER_ERROR_IO:
    return "io";
  case PLAYER_ERROR_NO_MEM:
    return "no_mem";
  }
  return "decode";
}

// How long the title has been on show, for the marquee. Counted from the
// clock and not from the player's position, so the title keeps moving while
// the track is paused. (It wraps after 49 days of one track; the marquee
// then jumps once.)
static uint32_t ui_title_ms(const ui_t *ui) {
  return (uint32_t)((esp_timer_get_time() - ui->title_since_us) / 1000);
}

// How long until the screen changes by itself, with no event: UINT32_MAX if
// it never will. Only a title too long for its row does that.
static uint32_t ui_next_change_ms(const ui_t *ui, uint32_t title_ms) {
  if (ui->no_card || ui->scanning) {
    return UINT32_MAX;
  }
  return marquee_next_change_ms(ui->title, SCREEN_COLS, title_ms);
}

// The title, moving if it is too long, over
// "> 0:12/4:15  V10": playing or paused, elapsed and total time, volume.
// `title_ms` is how long the title has been on show.
static void ui_render(const ui_t *ui, uint32_t title_ms, screen_t *screen) {
  if (ui->no_card) {
    // Nothing can play without a card, so this replaces the playing screen
    // whatever else happens (a `play` from the console fails, for one).
    screen_set_row(screen, 0, "Insert SD card");
    screen_set_row(screen, 1, "");
    return;
  }
  if (ui->scanning) {
    // The scan has stopped playback, so there is nothing else to show. The
    // count arrives at most four times a second.
    char count[2 * SCREEN_COLS];
    snprintf(count, sizeof(count), "%" PRIu32 " tracks", ui->scan_tracks);
    screen_set_row(screen, 0, "Scanning...");
    screen_set_row(screen, 1, count);
    return;
  }

  player_status_t status;
  player_get_status(&status);

  marquee_window(ui->title, SCREEN_COLS, title_ms, screen->cells[0]);

  char text[2 * SCREEN_COLS];
  if (ui->failed) {
    snprintf(text, sizeof(text), "  Can't play");
  } else {
    char mark = status.state == PLAYER_STATE_PLAYING  ? '>'
                : status.state == PLAYER_STATE_PAUSED ? '='
                                                      : ' ';
    uint32_t elapsed = ui->elapsed_ms / 1000;
    uint32_t total = ui->duration_ms / 1000;
    snprintf(text, sizeof(text),
             "%c %" PRIu32 ":%02" PRIu32 "/%" PRIu32 ":%02" PRIu32, mark,
             elapsed / 60, elapsed % 60, total / 60, total % 60);
  }
  screen_set_row(screen, 1, text);

  int len = snprintf(text, sizeof(text), "V%d", status.volume);
  screen_put(screen, 1, SCREEN_COLS - len, text);
}

static void ui_on_key(ui_t *ui, const key_event_t *event) {
  player_status_t status;
  player_get_status(&status);
  bool press = (event->type == KEY_PRESS);
  bool press_or_repeat = press || event->type == KEY_REPEAT;

  if (event->key == 'D' && press) {
    if (status.state == PLAYER_STATE_PLAYING) {
      player_pause();
    } else if (status.state == PLAYER_STATE_PAUSED) {
      player_resume();
    }
  } else if (event->key == 'A' && press_or_repeat) {
    if (status.volume < PCM_VOLUME_MAX) {
      player_set_volume(status.volume + 1);
    }
  } else if (event->key == 'B' && press_or_repeat) {
    if (status.volume > 0) {
      player_set_volume(status.volume - 1);
    }
  } else if (event->key == '#' && event->type == KEY_LONG) {
    ui->backlight = !ui->backlight;
    lcd_set_backlight(ui->backlight);
  }
}

// Put the title and artist of the track last started on the top row, if the
// library has that track. Reads no file: one search of the index, and the
// index is held only while the two strings are copied out of it.
//
// When there is no index (none loaded yet, a scan is running) or the track
// is not in it (a file added since the last scan), the title is left as it
// is: the file name. Nothing waits or tries again on a timer; the UI task
// calls this again when the library publishes a new index.
//
// A track with no tags is in the index under the fallbacks, so it shows as
// "<file name> - Unknown Artist".
static void ui_title_from_library(ui_t *ui) {
  // The index has paths relative to the card: "music/a.flac".
  static const char root[] = STORAGE_MOUNT_POINT "/";
  const size_t root_len = sizeof(root) - 1;
  if (strncmp(ui->path, root, root_len) != 0) {
    return; // nothing has started yet
  }
  const library_t *lib = library_task_get();
  if (lib == NULL) {
    return;
  }
  char text[TITLE_MAX_BYTES] = "";
  uint32_t track;
  library_track_t info;
  if (library_find_path(lib, ui->path + root_len, &track) &&
      library_track(lib, track, &info) && info.title[0] != '\0') {
    if (info.artist[0] != '\0') {
      snprintf(text, sizeof(text), "%s - %s", info.title, info.artist);
    } else {
      snprintf(text, sizeof(text), "%s", info.title);
    }
  }
  library_task_put(); // `info` and its strings are dead from here

  if (text[0] != '\0' && strcmp(text, ui->title) != 0) {
    // Different words on the row: they are read from the beginning.
    strcpy(ui->title, text);
    ui->title_since_us = esp_timer_get_time();
  }
}

static void ui_on_player_event(ui_t *ui, const player_event_t *event) {
  switch (event->type) {
  case PLAYER_EVENT_STARTED: {
    player_get_path(ui->path, sizeof(ui->path));
    ESP_LOGI(PLAYER_TAG,
             "STARTED path=%s rate=%" PRIu32 " bits=%u ch=%u duration_ms=%"
             PRIu32,
             ui->path, event->sample_rate, event->bits, event->channels,
             event->duration_ms);
    const char *slash = strrchr(ui->path, '/');
    translit_title(NULL, slash ? slash + 1 : ui->path, ui->title,
                   sizeof(ui->title));
    ui_title_from_library(ui);
    // A new track: its title is read from the beginning. Apart from the
    // library supplying the title late, nothing else (pause, volume, the
    // clock) restarts the marquee.
    ui->title_since_us = esp_timer_get_time();
    ui->elapsed_ms = 0;
    ui->duration_ms = event->duration_ms;
    ui->failed = false;
    break;
  }
  case PLAYER_EVENT_POSITION:
    ESP_LOGI(PLAYER_TAG, "POSITION ms=%" PRIu32, event->position_ms);
    ui->elapsed_ms = event->position_ms;
    break;

  case PLAYER_EVENT_ENDED: {
    // The event has no room for frame counts; the player keeps those of the
    // last track that ended.
    player_status_t status;
    player_get_status(&status);
    ESP_LOGI(PLAYER_TAG, "ENDED frames=%" PRIu32 " expected=%" PRIu32,
             status.ended_frames, status.ended_expected);
    ui->elapsed_ms = event->position_ms;
    break;
  }
  case PLAYER_EVENT_ERROR:
    ESP_LOGE(PLAYER_TAG, "ERROR reason=%s", error_name(event->error));
    ui->failed = true;
    break;
  }

  // The output task cannot log an underrun itself. It counts them, and the
  // count is looked at here, twice a second while a track plays.
  player_status_t status;
  player_get_status(&status);
  if (status.underruns != ui->underruns) {
    ESP_LOGW(PLAYER_TAG, "UNDERRUN count=%" PRIu32 " (the buffer ran dry)",
             status.underruns);
    ui->underruns = status.underruns;
  }
}

// How long to sleep for a change that is `ms` away. A wait of n ticks ends
// at the n-th tick from now, which is anything from n - 1 to n tick periods
// away, so round up and add one: waking early would find nothing to draw and
// have to go round again.
static TickType_t ticks_until(uint32_t ms) {
  if (ms == UINT32_MAX) {
    return portMAX_DELAY;
  }
  return (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

// Owns the LCD. Sleeps until a key event, a player event, a card event or
// news of the library arrives (the queue set is what lets it wait for all of them at once), or
// until a moving title is due its next step, whichever comes first. With a
// title that fits there is no such step and it sleeps until an event.
static void ui_task(void *arg) {
  (void)arg;
  static ui_t ui; // too big for comfort on the stack
  ui.backlight = true;
  screen_t shown;        // what the LCD was last told to show
  bool shown_ok = false; // and it took it

  for (;;) {
    // One reading of the clock decides both what is drawn and how long to
    // sleep, so the sleep always ends on a window that differs from this one.
    uint32_t title_ms = ui_title_ms(&ui);
    screen_t wanted;
    ui_render(&ui, title_ms, &wanted);
    // Most events change nothing that is on the screen (a key release, a
    // clock tick inside the same second). Comparing two screens costs
    // nothing next to an I2C transfer. A transfer that failed is tried
    // again whether or not anything changed; with no LCD at all that is a
    // call that returns at once.
    if (!shown_ok || memcmp(&wanted, &shown, sizeof(shown)) != 0) {
      shown = wanted;
      shown_ok = (lcd_show(&shown) == ESP_OK);
      debug_console_note_screen(&shown);
    }

    QueueSetMemberHandle_t ready = xQueueSelectFromSet(
        s_ui_events, ticks_until(ui_next_change_ms(&ui, title_ms)));
    if (ready == s_key_events) {
      key_event_t event;
      if (xQueueReceive(s_key_events, &event, 0) == pdTRUE) {
        ui_on_key(&ui, &event);
      }
    } else if (ready == s_player_events) {
      player_event_t event;
      if (xQueueReceive(s_player_events, &event, 0) == pdTRUE) {
        ui_on_player_event(&ui, &event);
      }
    } else if (ready == s_card_events) {
      card_event_t event;
      if (xQueueReceive(s_card_events, &event, 0) == pdTRUE) {
        ui.no_card = (event == CARD_EVENT_WAITING);
        // A `play` that failed for want of a card says nothing about now.
        ui.failed = false;
      }
    } else if (ready == s_library_status) {
      library_status_t status;
      if (xQueueReceive(s_library_status, &status, 0) == pdTRUE) {
        ui.scanning = status.scanning;
        ui.scan_tracks = status.tracks;
        // A new index (the first load, or a scan that has finished): the
        // track on show may have started while there was none, or its tags
        // may have changed, so look it up again.
        if (!status.scanning && status.generation != ui.library_generation) {
          ui.library_generation = status.generation;
          ui_title_from_library(&ui);
        }
      }
    }
    // Otherwise the wait ran out: the title is due its next step.
  }
}

// Bring up the LCD, the UI task and the keypad. The LCD and keypad are not
// required for audio, so their failures are logged and the player carries
// on. The queues and the UI task are one-time start-up allocations; without
// them there is nothing to run.
static void start_display_and_keys(void) {
  s_key_events = xQueueCreate(KEY_QUEUE_LENGTH, sizeof(key_event_t));
  s_player_events =
      xQueueCreate(PLAYER_EVENT_QUEUE_LENGTH, sizeof(player_event_t));
  s_card_events = xQueueCreate(CARD_EVENT_QUEUE_LENGTH, sizeof(card_event_t));
  s_library_status =
      xQueueCreate(LIBRARY_STATUS_QUEUE_LENGTH, sizeof(library_status_t));
  s_ui_events = xQueueCreateSet(KEY_QUEUE_LENGTH + PLAYER_EVENT_QUEUE_LENGTH +
                                CARD_EVENT_QUEUE_LENGTH +
                                LIBRARY_STATUS_QUEUE_LENGTH);
  configASSERT(s_key_events && s_player_events && s_card_events &&
               s_library_status && s_ui_events);
  // Queues must join the set while still empty, so before anything posts.
  xQueueAddToSet(s_key_events, s_ui_events);
  xQueueAddToSet(s_player_events, s_ui_events);
  xQueueAddToSet(s_card_events, s_ui_events);
  xQueueAddToSet(s_library_status, s_ui_events);

  // The LCD is set up here, before the UI task exists; from then on only
  // that task touches it.
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
  }
  BaseType_t made = xTaskCreatePinnedToCore(
      ui_task, "ui", UI_TASK_STACK, NULL, UI_TASK_PRIORITY, NULL, UI_TASK_CORE);
  configASSERT(made == pdPASS);

  const keypad_pins_t keypad_pins = {
      .rows = {PIN_KEYPAD_ROW_1, PIN_KEYPAD_ROW_2, PIN_KEYPAD_ROW_3,
               PIN_KEYPAD_ROW_4},
      .cols = {PIN_KEYPAD_COL_1, PIN_KEYPAD_COL_2, PIN_KEYPAD_COL_3,
               PIN_KEYPAD_COL_4},
  };
  err = keypad_init(&keypad_pins);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Keypad setup failed (%s)", esp_err_to_name(err));
    return;
  }
  uint8_t low = keypad_low_columns();
  for (int c = 0; c < KEYPAD_COLS; c++) {
    if (low & (1u << c)) {
      ESP_LOGW(TAG,
               "Keypad column %d reads low with no key pressed: check its 10k "
               "pull-up to 3.3V",
               c + 1);
    }
  }
  if (low == 0) {
    ESP_LOGI(TAG, "Keypad columns all read high");
  }
  err = keypad_start(s_key_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Keypad task failed to start (%s)", esp_err_to_name(err));
  }
}

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

// Mount the card, however long that takes. On this board a cold plug-in to a
// computer's USB port leaves the card deaf until it is taken out and put
// back, so a failed mount is not final: say so on the LCD and try again
// every two seconds. Returns once the card is mounted.
//
// This runs in the main task, which has nothing else to do and has the
// lowest priority of all our tasks, so the keys, the LCD, the console and
// the player carry on while it waits. Both the mount attempt and the pause
// between attempts block; neither spins.
static void wait_for_card(void) {
  ESP_LOGI(TAG, "Mounting SD card");
  uint32_t attempts = 1;
  esp_err_t err = storage_mount();
  if (err == ESP_OK) {
    return;
  }

  ESP_LOGW(TAG, "No usable SD card (%s); retrying every %d seconds",
           esp_err_to_name(err), CARD_RETRY_MS / 1000);
  // At most two card events are ever sent, and the queue holds two, so
  // neither send can block or be lost.
  card_event_t event = CARD_EVENT_WAITING;
  xQueueSend(s_card_events, &event, portMAX_DELAY);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(CARD_RETRY_MS));
    attempts++;
    err = storage_mount();
    if (err == ESP_OK) {
      break;
    }
    ESP_LOGW(TAG, "Mount attempt %" PRIu32 " failed (%s)", attempts,
             esp_err_to_name(err));
  }

  ESP_LOGI(TAG, "SD card mounted after %" PRIu32 " attempts", attempts);
  event = CARD_EVENT_MOUNTED;
  xQueueSend(s_card_events, &event, portMAX_DELAY);
}

void app_main(void) {
  // Screen and keys first: they work with or without a card.
  start_display_and_keys();

  const audio_out_pins_t pins = {
      .bck = PIN_I2S_BCK,
      .lrck = PIN_I2S_LRCK,
      .dout = PIN_I2S_DOUT,
  };
  audio_out_init(&pins);
  esp_err_t err = player_start(s_player_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The player failed to start (%s)", esp_err_to_name(err));
    return;
  }
  // The console is a test aid; the player works without it.
  err = debug_console_start(s_key_events);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No debug console (%s)", esp_err_to_name(err));
  }

  // The library is optional too: without its memory the player still plays
  // whatever it is told to (library_task_start has said why in the log).
  library_task_start(s_library_status);

  wait_for_card();

  // Load the library's index, or scan the card if there is none. Nothing
  // plays until that is done: a scan wants the card to itself.
  library_task_boot();

  // Play the first FLAC we find. The card stays mounted afterwards.
  char track[PATH_MAX_BYTES];
  if (!find_flac(track, sizeof(track))) {
    ESP_LOGW(TAG, "No .flac found in " STORAGE_MOUNT_POINT "/music");
    return;
  }
  ESP_LOGI(TAG, "Playing %s", track);
  player_play(track, 0);
}
