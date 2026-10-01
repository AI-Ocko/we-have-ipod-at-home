#include "audio_out.h"
#include "debug_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad.h"
#include "keys.h"
#include "lcd.h"
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

static const char *TAG = "dap";
// The player's events are logged here, by the task that reads them, under
// the player's name: the audio tasks themselves log nothing while they run.
static const char *PLAYER_TAG = "player";

static QueueHandle_t s_key_events;
static QueueHandle_t s_player_events;
static QueueSetHandle_t s_ui_events; // both of the above

// ---- the temporary playing screen (replaced in stage 5) ---------------------

typedef struct {
  char title[SCREEN_COLS + 1]; // file name, without folder or extension
  uint32_t elapsed_ms;
  uint32_t duration_ms;
  bool failed; // the last `play` ended in an error
  uint32_t underruns; // how many have been reported so far
  bool backlight;
  char path[PATH_MAX_BYTES]; // scratch, for STARTED
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

// "> 0:12/4:15  V10": playing or paused, elapsed and total time, volume.
static void ui_render(const ui_t *ui, screen_t *screen) {
  player_status_t status;
  player_get_status(&status);

  screen_set_row(screen, 0, ui->title);

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

// Owns the LCD. Sleeps until a key event or a player event arrives; the
// queue set is what lets it wait for both at once.
static void ui_task(void *arg) {
  (void)arg;
  static ui_t ui; // too big for comfort on the stack
  ui.backlight = true;
  screen_t screen;
  screen_clear(&screen);
  ui_render(&ui, &screen);
  lcd_show(&screen); // does nothing useful if there is no LCD; that is fine

  for (;;) {
    QueueSetMemberHandle_t ready =
        xQueueSelectFromSet(s_ui_events, portMAX_DELAY);
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
    }
    ui_render(&ui, &screen);
    lcd_show(&screen);
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
  s_ui_events = xQueueCreateSet(KEY_QUEUE_LENGTH + PLAYER_EVENT_QUEUE_LENGTH);
  configASSERT(s_key_events && s_player_events && s_ui_events);
  // Queues must join the set while still empty, so before anything posts.
  xQueueAddToSet(s_key_events, s_ui_events);
  xQueueAddToSet(s_player_events, s_ui_events);

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

  if (storage_mount() != ESP_OK) {
    ESP_LOGE(TAG, "No usable SD card; nothing to play");
    return;
  }

  // Play the first FLAC we find. The card stays mounted afterwards.
  char track[PATH_MAX_BYTES];
  if (!find_flac(track, sizeof(track))) {
    ESP_LOGW(TAG, "No .flac found in " STORAGE_MOUNT_POINT "/music");
    return;
  }
  ESP_LOGI(TAG, "Playing %s", track);
  player_play(track, 0);
}
