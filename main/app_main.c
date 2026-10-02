#include "audio_out.h"
#include "debug_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad.h"
#include "library_task.h"
#include "pins.h"
#include "player.h"
#include "storage.h"
#include "ui.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PATH_MAX_BYTES 320
#define CARD_RETRY_MS 2000

static const char *TAG = "dap";

static ui_queues_t s_queues;

// Bring up the LCD, the UI task and the keypad. The LCD and keypad are not
// required for audio, so their failures are logged and the player carries
// on.
static void start_display_and_keys(void) {
  esp_err_t err = ui_start(&s_queues);
  configASSERT(err == ESP_OK);

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
  err = keypad_start(s_queues.keys);
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
  ui_card_waiting(true);

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
  ui_card_waiting(false);
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
  esp_err_t err = player_start(s_queues.player_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The player failed to start (%s)", esp_err_to_name(err));
    return;
  }
  // The console is a test aid; the player works without it.
  err = debug_console_start(s_queues.keys);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No debug console (%s)", esp_err_to_name(err));
  }

  // The library is optional too: without its memory the player still plays
  // whatever it is told to (library_task_start has said why in the log).
  library_task_start(s_queues.library_status);

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
  player_play(track, 0, NULL);
}
