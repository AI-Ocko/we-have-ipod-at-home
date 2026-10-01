#pragma once

// Every GPIO the player uses. No other file may contain a pin number.
//
// SD card (1-bit SDMMC) is not listed as a setting because the ESP32 fixes
// those pins in silicon: CLK = 14, CMD = 15, D0 = 2. The camera connector
// shares what would be D1-D3, so 4-bit mode is impossible on this board.
//
// Never use GPIO 0 or 12: they are boot strapping pins, and 12 held high at
// reset stops this module from booting.
//
// Spare, reserved for future hardware: 5, 18, 19, 23 (the VSPI group).

// PCM5102A DAC, I2S. The DAC's SCK pin is tied to GND; it makes its own clock.
#define PIN_I2S_BCK 26  // DAC "BCK"
#define PIN_I2S_LRCK 25 // DAC "LCK"
#define PIN_I2S_DOUT 22 // DAC "DIN"

// LCD1602 behind a PCF8574 I2C backpack. SCL is not the board's default
// (22), because 22 is the DAC data line.
#define PIN_LCD_SDA 21
#define PIN_LCD_SCL 27

// 4x4 keypad. Rows are driven by the ESP32.
#define PIN_KEYPAD_ROW_1 32
#define PIN_KEYPAD_ROW_2 33
#define PIN_KEYPAD_ROW_3 4
#define PIN_KEYPAD_ROW_4 13

// Columns are read. These four pins are input-only and have no internal
// pull-up, so each needs an external 10k resistor to 3.3V.
#define PIN_KEYPAD_COL_1 34
#define PIN_KEYPAD_COL_2 35
#define PIN_KEYPAD_COL_3 36
#define PIN_KEYPAD_COL_4 39
