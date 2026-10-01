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

static const char *TAG = "lcd";

static const uint8_t ADDRESSES[] = {0x27, 0x3F};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_backlight = BIT_BACKLIGHT;

// What the LCD is currently showing, so lcd_show can send only differences.
static screen_t s_shown;
static bool s_shown_valid;

// Wait at least `ms`. vTaskDelay rounds down to whole ticks and the first
// tick may be almost over already, so add one.
static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// Append the expander bytes that send one byte to the LCD. `rs` is 0 for a
// command or BIT_RS for character data. Returns the new length of `buf`.
//
// A whole run of these goes out in one I2C transfer. Each expander byte takes
// 90 us on the wire, which is longer than the 37 us the LCD needs between
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
  uint8_t buf[BYTES_PER_LCD_BYTE];
  size_t len = append_byte(buf, 0, command, 0);
  return i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
}

// Send only the upper half of a byte. Needed at power-up, while the LCD is
// still in 8-bit mode and reads all eight (half-connected) data lines at once.
static esp_err_t send_half(uint8_t half) {
  uint8_t value = (uint8_t)((half << 4) | s_backlight);
  uint8_t buf[2] = {value | BIT_EN, value};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
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

  screen_run_t runs[SCREEN_MAX_RUNS];
  int count;
  if (s_shown_valid) {
    count = screen_diff(&s_shown, wanted, runs);
    if (count == 0) {
      return ESP_OK; // nothing changed: no I2C traffic at all
    }
  } else {
    // A previous transfer failed part-way, so the LCD's contents are unknown.
    // Send both rows in full.
    for (int row = 0; row < SCREEN_ROWS; row++) {
      runs[row] = (screen_run_t){
          .row = (uint8_t)row, .col = 0, .len = SCREEN_COLS};
    }
    count = SCREEN_ROWS;
  }

  // The whole update goes out as one I2C transfer: for each run, a cursor
  // move followed by its characters. Sized for the worst case, every cell
  // plus one cursor move per run.
  uint8_t buf[BYTES_PER_LCD_BYTE *
              (SCREEN_MAX_RUNS + SCREEN_ROWS * SCREEN_COLS)];
  size_t len = 0;
  for (int i = 0; i < count; i++) {
    uint8_t address =
        (uint8_t)((runs[i].row ? ROW_1_ADDRESS : 0) + runs[i].col);
    len = append_byte(buf, len, CMD_SET_DDRAM | address, 0);
    const char *cells = &wanted->cells[runs[i].row][runs[i].col];
    for (int c = 0; c < runs[i].len; c++) {
      len = append_byte(buf, len, (uint8_t)cells[c], BIT_RS);
    }
  }

  esp_err_t err = i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
  if (err != ESP_OK) {
    s_shown_valid = false;
    return err;
  }
  s_shown = *wanted;
  s_shown_valid = true;
  return ESP_OK;
}

esp_err_t lcd_define_char(int index, const uint8_t rows[8]) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  if (index < 0 || index > 7) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t buf[BYTES_PER_LCD_BYTE * (1 + 8)];
  size_t len = append_byte(buf, 0, (uint8_t)(CMD_SET_CGRAM | (index << 3)), 0);
  for (int i = 0; i < 8; i++) {
    len = append_byte(buf, len, rows[i] & 0x1F, BIT_RS);
  }
  // This leaves the LCD addressing its character-generator memory. That is
  // harmless: every lcd_show run starts by setting the display address.
  return i2c_master_transmit(s_dev, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t lcd_set_backlight(bool on) {
  if (!s_dev) {
    return ESP_ERR_INVALID_STATE;
  }
  s_backlight = on ? BIT_BACKLIGHT : 0;
  // A byte with EN low changes the backlight and nothing else.
  uint8_t value = s_backlight;
  return i2c_master_transmit(s_dev, &value, 1, I2C_TIMEOUT_MS);
}
