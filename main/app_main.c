#include "audio_out.h"
#include "esp_heap_caps.h"
#include "debug_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "keypad.h"
#include "library_task.h"
#include "pins.h"
#include "player.h"
#include "player_status.h"
#include "playlist_task.h"
#include "state_task.h"
#include "storage.h"
#include "ui.h"
#include <inttypes.h>
#include <stdbool.h>

#define CARD_RETRY_MS 2000
// While the card is lost: how often to look whether it is free to unmount,
// how often to say that it is not yet, and to stop the player again.
#define CARD_FREE_POLL_MS 20
#define CARD_FREE_NAG_MS 5000
#define CARD_STOP_AGAIN_MS 1000
// The card task (look_after_card): the main task's priority and core.
#define CARD_TASK_STACK_BYTES 6144
#define CARD_TASK_PRIORITY 1
#define CARD_TASK_CORE 0

static const char *TAG = "dap";

static ui_queues_t s_queues;

// Start the keypad. It is not required for audio (the console can drive the
// player), so a failure is logged and the player carries on.
static void start_keys(void) {
  const keypad_pins_t keypad_pins = {
      .rows = {PIN_KEYPAD_ROW_1, PIN_KEYPAD_ROW_2, PIN_KEYPAD_ROW_3,
               PIN_KEYPAD_ROW_4},
      .cols = {PIN_KEYPAD_COL_1, PIN_KEYPAD_COL_2, PIN_KEYPAD_COL_3,
               PIN_KEYPAD_COL_4},
  };
  esp_err_t err = keypad_init(&keypad_pins);
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

// Mount the card, however long that takes. On this board a cold plug-in to a
// computer's USB port leaves the card deaf until it is taken out and put
// back, so a failed mount is not final: say so on the LCD and try again
// every two seconds. Returns once the card is mounted. `waiting` says the
// UI already shows that the card is being waited for (after a loss); it is
// told when the wait is over either way.
//
// This runs in the task that mounts the card: the main task at start-up,
// the card task (look_after_card) after a loss. Each has nothing else to do
// and the lowest priority of all our tasks, so the keys, the LCD, the
// console and the player carry on while it waits. Both the mount attempt
// and the pause between attempts block; neither spins.
static void mount_card(bool waiting) {
  uint32_t attempts = 0;
  esp_err_t err = ESP_FAIL;
  if (!waiting) {
    ESP_LOGI(TAG, "Mounting SD card");
    attempts = 1;
    err = storage_mount();
    if (err == ESP_OK) {
      return;
    }
    ESP_LOGW(TAG, "No usable SD card (%s); retrying every %d seconds",
             esp_err_to_name(err), CARD_RETRY_MS / 1000);
    ui_card_waiting(true);
  }

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

// The card has gone: wait until nothing is using it, so that the unmount
// pulls nothing from under a task in the middle of a card operation. The UI
// has been told to stop playback; the library, state and playlist tasks see
// that storage_hold refuses, finish (or fail) what they are doing and give
// their holds back. Polled, every CARD_FREE_POLL_MS, only during a loss.
static void wait_until_card_is_free(void) {
  uint32_t waited = 0;
  for (;;) {
    player_status_t status;
    player_get_status(&status);
    uint32_t holds = storage_holds();
    if (status.state == PLAYER_STATE_IDLE && holds == 0) {
      break;
    }
    if (waited != 0 && waited % CARD_FREE_NAG_MS == 0) {
      ESP_LOGW(TAG,
               "still waiting to unmount: player %s, %" PRIu32 " holds",
               status.state == PLAYER_STATE_IDLE ? "idle" : "busy", holds);
    }
    if (waited != 0 && waited % CARD_STOP_AGAIN_MS == 0 &&
        status.state != PLAYER_STATE_IDLE) {
      // A track the UI does not know of (the console's), or a stop the
      // player could not take: stopped here too. The UI's own stop has
      // already kept the track it had.
      player_stop();
    }
    vTaskDelay(pdMS_TO_TICKS(CARD_FREE_POLL_MS));
    waited += CARD_FREE_POLL_MS;
  }
  if (waited != 0) {
    ESP_LOGI(TAG, "the card was free after %" PRIu32 " ms", waited);
  }
}

// The card task, from start-up on: wait for a task to report a card
// failure; ask the card whether it still answers; if it does not, stop,
// unmount, retry every two seconds and, once the card is back, carry on as
// after a power-on (the index loaded again, or scanned), with no reset.
// Nothing here polls while nothing has failed.
//
// It is a task of its own, and not the main task carrying on, because the
// main task's stack is 16 KB of internal RAM (CONFIG_ESP_MAIN_TASK_STACK_SIZE)
// that is freed when app_main returns, and internal RAM is what is short
// while a track plays. This one's stack is in PSRAM, which is safe for the
// same reason as the playlist task's (playlist_task.c, MEMORY): it never
// writes flash, so the cache is never turned off on its behalf.
static void look_after_card(void *arg) {
  (void)arg;
  for (;;) {
    storage_wait_failure();
    if (storage_card_answers()) {
      ESP_LOGI(TAG, "a card operation failed, but the card answers");
      continue;
    }
    // storage_card_answers has marked the card as gone: no new holds.
    ESP_LOGE(TAG, "The SD card does not answer: stopping and unmounting");
    ui_card_lost(); // stops playback; "Card error", then "Insert SD card"
    wait_until_card_is_free();
    storage_unmount();
    ESP_LOGW(TAG, "SD card unmounted; retrying every %d seconds",
             CARD_RETRY_MS / 1000);
    mount_card(true);
    // Reports since the loss were about the card that went.
    storage_forget_failures();
    // A queue save that failed meanwhile is made now.
    state_task_card_back();
    // The index again, as at power-on. The queue in the UI's memory is
    // kept if this is the same index (ui_play.c, A NEW INDEX); the saved
    // queue file is not read again.
    library_task_boot();
    ESP_LOGI(TAG, "card task stack free %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
}

void app_main(void) {
  // Before any task that may report a card failure exists.
  storage_init();

  // What was saved comes first: the UI task starts from the saved settings.
  // It reads flash only (about 15 ms), and whatever goes wrong it has said
  // so in the log and the player runs on the defaults.
  state_task_start();

  // Then screen and keys: they work with or without a card. Without the
  // UI's queues nothing else can be started.
  esp_err_t err = ui_start(&s_queues);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The user interface failed to start (%s)",
             esp_err_to_name(err));
    return;
  }
  start_keys();

  const audio_out_pins_t pins = {
      .bck = PIN_I2S_BCK,
      .lrck = PIN_I2S_LRCK,
      .dout = PIN_I2S_DOUT,
  };
  audio_out_init(&pins);
  err = player_start(s_queues.player_events);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The player failed to start (%s)", esp_err_to_name(err));
    return;
  }
  state_settings_t settings;
  state_task_loaded_settings(&settings);
  player_set_volume(settings.volume);
  // The console is a test aid; the player works without it.
  err = debug_console_start(s_queues.keys);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No debug console (%s)", esp_err_to_name(err));
  }

  // The library is optional too: without its memory the player still plays
  // whatever it is told to (library_task_start has said why in the log).
  library_task_start(s_queues.library_status);

  // Playlists are optional in the same way. The UI's requests are answered
  // on its queue (created by ui_start, above).
  playlist_task_start(s_queues.playlist_events);

  mount_card(false);

  // The saved queue is read before the index is loaded, so that it is in
  // memory when the UI first sees the index and asks for it.
  state_task_card_ready();

  // Load the library's index, or scan the card if there is none. Nothing
  // plays by itself: the UI shows the main menu and waits for the user.
  library_task_boot();

  // From here on the card task looks after the card, and the main task
  // ends (its stack goes back to the heap).
  if (xTaskCreatePinnedToCoreWithCaps(
          look_after_card, "card", CARD_TASK_STACK_BYTES, NULL,
          CARD_TASK_PRIORITY, NULL, CARD_TASK_CORE,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "no memory for the card task: a card pulled out will need "
                  "a reset");
  }
}
