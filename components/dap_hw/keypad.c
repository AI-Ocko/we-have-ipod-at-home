#include "keypad.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"
#include "keys.h"

#define SCAN_PERIOD_MS 10
#define TASK_STACK 3072
#define TASK_PRIORITY 6
#define TASK_CORE 0

// Time for a column to settle after its row is pulled low. The line is a few
// centimetres of wire through a 10k pull-up, which settles in about 1 us;
// this leaves a wide margin and is far too short to hand to the scheduler.
#define SETTLE_US 10

_Static_assert(pdMS_TO_TICKS(SCAN_PERIOD_MS) >= 1,
               "the scan period must be at least one tick");

static keypad_pins_t s_pins;
static QueueHandle_t s_events;

esp_err_t keypad_init(const keypad_pins_t *pins) {
  s_pins = *pins;

  uint64_t row_mask = 0;
  uint64_t col_mask = 0;
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    row_mask |= 1ULL << s_pins.rows[r];
    // Released (floating) before the pin becomes an output, so no row is
    // driven low by accident.
    gpio_set_level((gpio_num_t)s_pins.rows[r], 1);
  }
  for (int c = 0; c < KEYPAD_COLS; c++) {
    col_mask |= 1ULL << s_pins.cols[c];
  }

  gpio_config_t row_config = {
      .pin_bit_mask = row_mask,
      .mode = GPIO_MODE_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&row_config);
  if (err != ESP_OK) {
    return err;
  }

  // The column pins are input-only and have no internal pull resistors; the
  // external 10k pull-ups do that job.
  gpio_config_t col_config = {
      .pin_bit_mask = col_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&col_config);
}

uint16_t keypad_scan(void) {
  uint16_t down = 0;
  for (int r = 0; r < KEYPAD_ROWS; r++) {
    gpio_set_level((gpio_num_t)s_pins.rows[r], 0);
    esp_rom_delay_us(SETTLE_US);
    for (int c = 0; c < KEYPAD_COLS; c++) {
      if (gpio_get_level((gpio_num_t)s_pins.cols[c]) == 0) {
        down |= (uint16_t)(1u << (r * KEYPAD_COLS + c));
      }
    }
    gpio_set_level((gpio_num_t)s_pins.rows[r], 1);
  }
  return down;
}

uint8_t keypad_low_columns(void) {
  uint8_t low = 0;
  for (int c = 0; c < KEYPAD_COLS; c++) {
    if (gpio_get_level((gpio_num_t)s_pins.cols[c]) == 0) {
      low |= (uint8_t)(1u << c);
    }
  }
  return low;
}

static void keypad_task(void *arg) {
  (void)arg;
  keys_t keys;
  keys_init(&keys);

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SCAN_PERIOD_MS));
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    key_event_t event;
    if (keys_update(&keys, keypad_scan(), now_ms, &event)) {
      xQueueSend(s_events, &event, 0); // never block the scan
    }
  }
}

esp_err_t keypad_start(QueueHandle_t events) {
  if (!events) {
    return ESP_ERR_INVALID_ARG;
  }
  s_events = events;
  BaseType_t ok = xTaskCreatePinnedToCore(keypad_task, "keypad", TASK_STACK,
                                          NULL, TASK_PRIORITY, NULL, TASK_CORE);
  return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
