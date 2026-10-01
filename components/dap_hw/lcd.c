#include "lcd.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>

// How the backpack's PCF8574 outputs are wired to the LCD. The top four bits
// are the LCD's D4..D7. RW (0x02) is left low: we only ever write.
#define BIT_RS 0x01 // 0 = command, 1 = character data
#define BIT_EN 0x04 // the LCD reads the data lines when this falls
#define BIT_BACKLIGHT 0x08

#define CMD_CLEAR 0x01
#define CMD_ENTRY_MODE 0x06   // cursor moves right, display does not shift
#define CMD_DISPLAY_OFF 0x08
#define CMD_DISPLAY_ON 0x0C   // display on, no cursor, no blink
#define CMD_FUNCTION_SET 0x28 // 4-bit bus, two lines, 5x8 font
#define CMD_SET_CGRAM 0x40
#define CMD_SET_DDRAM 0x80
#define ROW_1_ADDRESS 0x40

#define I2C_SPEED_HZ 100000 // the PCF8574's limit
#define I2C_TIMEOUT_MS 50

// One LCD byte takes four expander bytes: each half is put on the data lines
// with EN high, then again with EN low.
#define BYTES_PER_LCD_BYTE 4

// One expander byte that puts RS at its new level before EN rises.
#define BYTES_PER_RS_SETUP 1

#define GLYPH_COUNT 8 // user-defined characters the LCD can hold
#define GLYPH_ROWS 8  // pixel rows in each

static const char *TAG = "lcd";

static const uint8_t ADDRESSES[] = {0x27, 0x3F};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_address; // where the LCD answered, for probing it again
static uint8_t s_backlight = BIT_BACKLIGHT;

// What the LCD is currently showing, so lcd_show can send only differences.
// Not valid means the LCD's state is unknown: it has not been initialised, or
// a transfer failed and may have left it half a byte out of step.
static screen_t s_shown;
static bool s_shown_valid;

// The user-defined characters, kept so they can be sent again when the LCD
// is re-initialised. Bit `i` of the mask is set once glyph `i` is defined.
static uint8_t s_glyphs[GLYPH_COUNT][GLYPH_ROWS];
static uint8_t s_glyph_mask;

// Wait at least `ms`. vTaskDelay rounds down to whole ticks and the first
// tick may be almost over already, so add one.
static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// Every transfer to the LCD goes through here. A failure means the LCD may
// have seen only part of a byte, so its state is no longer known: lcd_show
// re-initialises it before the next update. Only the first failure is logged,
// so an unplugged LCD gives one line rather than one per update.
static esp_err_t transmit(const uint8_t *buf, size_t len) {
  esp_err_t err = i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    if (s_shown_valid) {
      ESP_LOGW(TAG,
               "LCD transfer failed (%s); will re-initialise on the next update",
               esp_err_to_name(err));
    }
    s_shown_valid = false;
  }
  return err;
}

// Append one expander byte that sets RS with EN low and the data lines at 0.
// `rs` is 0 for commands or BIT_RS for character data. Returns the new length
// of `buf`.
//
// The LCD needs RS to be steady for at least 40 ns before EN rises. All the
// expander's pins change at the same instant, so raising EN in the same byte
// that changes RS would give no margin at all. This byte goes in front of
// every group of LCD bytes whose RS may differ from the group before.
static size_t append_rs_setup(uint8_t *buf, size_t len, uint8_t rs) {
  buf[len++] = (uint8_t)(rs | s_backlight);
  return len;
}

// Append the expander bytes that send one byte to the LCD. `rs` is 0 for a
// command or BIT_RS for character data, and must already be on the RS line:
// see append_rs_setup. Returns the new length of `buf`.
//
// A whole run of these goes out in one I2C transfer. Each expander byte takes
// 90 us on the wire, so EN is high for 90 us with the data steady, and one
// LCD byte takes 360 us. That is longer than the 37 us the LCD needs between
// operations, so no delays are needed inside a transfer.
static size_t append_byte(uint8_t *buf, size_t len, uint8_t value, uint8_t rs) {
  uint8_t high = (uint8_t)((value & 0xF0) | rs | s_backlight);
  uint8_t low = (uint8_t)(((value << 4) & 0xF0) | rs | s_backlight);
  buf[len++] = high | BIT_EN;
  buf[len++] = high;
  buf[len++] = low | BIT_EN;
  buf[len++] = low;
  return len;
}

static esp_err_t send_command(uint8_t command) {
  uint8_t buf[BYTES_PER_RS_SETUP + BYTES_PER_LCD_BYTE];
  size_t len = append_rs_setup(buf, 0, 0);
  len = append_byte(buf, len, command, 0);
  return transmit(buf, len);
}

// Send only the upper half of a byte. Needed at power-up, while the LCD is
// still in 8-bit mode and reads all eight (half-connected) data lines at once.
// These are all commands, so RS stays low and needs no set-up byte.
static esp_err_t send_half(uint8_t half) {
  uint8_t value = (uint8_t)((half << 4) | s_backlight);
  uint8_t buf[2] = {value | BIT_EN, value};
  return transmit(buf, sizeof(buf));
}

// The HD44780's documented power-up sequence for a 4-bit bus.
static esp_err_t reset_controller(void) {
  esp_err_t err;
  wait_ms(50); // the LCD needs 40 ms after power-on before it listens

  // Three times "8-bit mode", which gets the LCD into a known state whichever
  // mode and half-byte it was left in, then "4-bit mode".
  for (int i = 0; i < 3; i++) {
    if ((err = send_half(0x3)) != ESP_OK) {
      return err;
    }
    wait_ms(5);
  }
  if ((err = send_half(0x2)) != ESP_OK) {
    return err;
  }
  wait_ms(1);

  if ((err = send_command(CMD_FUNCTION_SET)) != ESP_OK ||
      (err = send_command(CMD_DISPLAY_OFF)) != ESP_OK ||
      (err = send_command(CMD_CLEAR)) != ESP_OK) {
    return err;
  }
  wait_ms(2); // clearing is the one slow command: 1.5 ms
  if ((err = send_command(CMD_ENTRY_MODE)) != ESP_OK ||
      (err = send_command(CMD_DISPLAY_ON)) != ESP_OK) {
    return err;
  }
  return ESP_OK;
}

// Send one cached user-defined character to the LCD.
static esp_err_t send_glyph(int index) {
  uint8_t buf[2 * BYTES_PER_RS_SETUP + BYTES_PER_LCD_BYTE * (1 + GLYPH_ROWS)];
  size_t len = append_rs_setup(buf, 0, 0);
  len = append_byte(buf, len, (uint8_t)(CMD_SET_CGRAM | (index << 3)), 0);
  len = append_rs_setup(buf, len, BIT_RS);
  for (int i = 0; i < GLYPH_ROWS; i++) {
    len = append_byte(buf, len, s_glyphs[index][i], BIT_RS);
  }
  // This leaves the LCD addressing its character-generator memory. That is
  // harmless: every lcd_show run starts by setting the display address.
  return transmit(buf, len);
}

// Bring the LCD back after a failed transfer: start it again from scratch and
// restore the user-defined characters. It ends up blank, and s_shown says so.
static esp_err_t recover(void) {
  // Ask first whether the LCD is there at all. If it has been unplugged this
  // fails at once, so each update costs one short probe and none of the
  // delays in the reset sequence.
  esp_err_t err = i2c_master_probe(s_bus, s_address, I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    return err;
  }
  if ((err = reset_controller()) != ESP_OK) {
    return err;
  }
  for (int i = 0; i < GLYPH_COUNT; i++) {
    if ((s_glyph_mask & (1u << i)) && (err = send_glyph(i)) != ESP_OK) {
      return err;
    }
  }
  screen_clear(&s_shown); // matches the LCD after CMD_CLEAR
  s_shown_valid = true;
  ESP_LOGI(TAG, "LCD re-initialised");
  return ESP_OK;
}

esp_err_t lcd_init(int sda_pin, int scl_pin) {
  if (s_dev) {
    return ESP_OK;
  }

  if (!s_bus) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1, // any free port
        .sda_io_num = (gpio_num_t)sda_pin,
        .scl_io_num = (gpio_num_t)scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The backpack has its own pull-ups. These weak internal ones only
        // matter when no LCD is connected: they stop the lines floating, so
        // the probe below fails quickly and cleanly.
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
      s_bus = NULL;
      return err;
    }
  }

  uint8_t address = 0;
  for (size_t i = 0; i < sizeof(ADDRESSES); i++) {
    if (i2c_master_probe(s_bus, ADDRESSES[i], I2C_TIMEOUT_MS) == ESP_OK) {
      address = ADDRESSES[i];
      break;
    }
  }
  if (address == 0) {
    ESP_LOGW(TAG, "No LCD found at 0x27 or 0x3F");
    return ESP_ERR_NOT_FOUND;
  }

  i2c_device_config_t device_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = I2C_SPEED_HZ,
  };
  esp_err_t err = i2c_master_bus_add_device(s_bus, &device_config, &s_dev);
  if (err != ESP_OK) {
    s_dev = NULL;
    return err;
  }

  err = reset_controller();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LCD at 0x%02X did not initialise (%s)", address,
             esp_err_to_name(err));
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
  }

  s_address = address;
  screen_clear(&s_shown); // matches the LCD after CMD_CLEAR
  s_shown_valid = true;
  ESP_LOGI(TAG, "LCD found at 0x%02X", address);
  return ESP_OK;
}

bool lcd_is_present(void) { return s_dev != NULL; }

esp_err_t lcd_show(const screen_t *wanted) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!s_shown_valid) {
    // A previous transfer failed, so the LCD may be half a byte out of step
    // and nothing sent to it would be understood. Start it again; it comes
    // back blank, and the diff below then redraws whatever is wanted.
    esp_err_t err = recover();
    if (err != ESP_OK) {
      return err;
    }
  }

  screen_run_t runs[SCREEN_MAX_RUNS];
  int count = screen_diff(&s_shown, wanted, runs);
  if (count == 0) {
    return ESP_OK; // nothing changed: no I2C traffic at all
  }

  // The whole update goes out as one I2C transfer: for each run, a cursor
  // move followed by its characters, with an RS set-up byte in front of each
  // of the two. Sized for the worst case: every cell, plus one cursor move
  // and two set-up bytes per run.
  uint8_t buf[SCREEN_MAX_RUNS * (2 * BYTES_PER_RS_SETUP + BYTES_PER_LCD_BYTE) +
              BYTES_PER_LCD_BYTE * SCREEN_ROWS * SCREEN_COLS];
  size_t len = 0;
  for (int i = 0; i < count; i++) {
    uint8_t address =
        (uint8_t)((runs[i].row ? ROW_1_ADDRESS : 0) + runs[i].col);
    len = append_rs_setup(buf, len, 0);
    len = append_byte(buf, len, CMD_SET_DDRAM | address, 0);
    len = append_rs_setup(buf, len, BIT_RS);
    const char *cells = &wanted->cells[runs[i].row][runs[i].col];
    for (int c = 0; c < runs[i].len; c++) {
      len = append_byte(buf, len, (uint8_t)cells[c], BIT_RS);
    }
  }

  esp_err_t err = transmit(buf, len);
  if (err != ESP_OK) {
    return err;
  }
  s_shown = *wanted;
  return ESP_OK;
}

esp_err_t lcd_define_char(int index, const uint8_t rows[8]) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  if (index < 0 || index >= GLYPH_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  // Remember it before sending, so that a glyph defined while the link is
  // down still reaches the LCD when it is re-initialised.
  for (int i = 0; i < GLYPH_ROWS; i++) {
    s_glyphs[index][i] = rows[i] & 0x1F;
  }
  s_glyph_mask |= (uint8_t)(1u << index);
  return send_glyph(index);
}

esp_err_t lcd_set_backlight(bool on) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  s_backlight = on ? BIT_BACKLIGHT : 0;
  // A byte with EN low changes the backlight and nothing else.
  uint8_t value = s_backlight;
  return transmit(&value, 1);
}
