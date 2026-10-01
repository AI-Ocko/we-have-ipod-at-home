#include "audio_out.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad.h"
#include "keys.h"
#include "keytest.h"
#include "lcd.h"
#include "pins.h"
#include "player.h"
#include "screen.h"
#include "storage.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define KEY_QUEUE_LENGTH 16
#define UI_TASK_STACK 8192
#define UI_TASK_PRIORITY 5
#define UI_TASK_CORE 0

static const char *TAG = "dap";

static QueueHandle_t s_key_events;

// Temporary main screen for stage 2: the keypad test. Holding '#' toggles the
// backlight.
static void ui_task(void *arg) {
  (void)arg;
  screen_t screen;
  unsigned count = 0;
  bool backlight = true;

  keytest_render(&screen, NULL, 0);
  lcd_show(&screen); // does nothing useful if there is no LCD; that is fine

  for (;;) {
    key_event_t event;
    if (xQueueReceive(s_key_events, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    count++;
    ESP_LOGI(TAG, "key %c %s", event.key, keytest_event_name(event.type));

    if (event.type == KEY_LONG && event.key == '#') {
      backlight = !backlight;
      lcd_set_backlight(backlight);
    }
    keytest_render(&screen, &event, count);
    lcd_show(&screen);
  }
}

// Bring up the LCD and keypad. Neither is required for audio, so every
// failure here is logged and the player carries on.
static void start_display_and_keys(void) {
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err == ESP_OK) {
    lcd_define_char(KEYTEST_GLYPH_CODE - 8, KEYTEST_GLYPH);
  } else {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
  }

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

  s_key_events = xQueueCreate(KEY_QUEUE_LENGTH, sizeof(key_event_t));
  if (!s_key_events) {
    ESP_LOGE(TAG, "Out of memory for the key queue");
    return;
  }
  err = keypad_start(s_key_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Keypad task failed to start (%s)", esp_err_to_name(err));
    return;
  }
  if (xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL,
                              UI_TASK_PRIORITY, NULL, UI_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "UI task failed to start");
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

  if (storage_mount() != ESP_OK) {
    ESP_LOGE(TAG, "No usable SD card; nothing to play");
    return;
  }

  const audio_out_pins_t pins = {
      .bck = PIN_I2S_BCK,
      .lrck = PIN_I2S_LRCK,
      .dout = PIN_I2S_DOUT,
  };
  audio_out_init(&pins);
  player_init();

  // Play the first FLAC we find. The card stays mounted afterwards.
  char track[320];
  if (!find_flac(track, sizeof(track))) {
    ESP_LOGW(TAG, "No .flac found in " STORAGE_MOUNT_POINT "/music");
    return;
  }
  ESP_LOGI(TAG, "Playing %s", track);
  player_play_file(track);
}
