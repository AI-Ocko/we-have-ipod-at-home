#pragma once

#include "esp_err.h"
#include "screen.h"
#include <stdbool.h>
#include <stdint.h>

// LCD1602 (HD44780 controller) behind a PCF8574 I2C backpack.
//
// Not thread-safe: call from one task only (the UI task).

// Find the LCD and initialise it. Tries I2C address 0x27, then 0x3F.
// Returns ESP_ERR_NOT_FOUND if neither answers; the player is expected to
// carry on without a screen in that case.
esp_err_t lcd_init(int sda_pin, int scl_pin);

bool lcd_is_present(void);

// Make the LCD show `wanted`. Only the cells that differ from what is already
// displayed are sent: the I2C link costs about 0.4 ms per character, so this
// is what keeps the interface responsive.
esp_err_t lcd_show(const screen_t *wanted);

// Define one of the eight user-defined characters. `index` is 0..7 and
// `rows` holds eight 5-bit pixel rows, top first. The character is displayed
// by putting code 8 + index in a screen cell.
esp_err_t lcd_define_char(int index, const uint8_t rows[8]);

// Switch the backlight on or off. The display contents are kept.
esp_err_t lcd_set_backlight(bool on);
