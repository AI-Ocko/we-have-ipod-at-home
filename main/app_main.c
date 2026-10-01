#include "audio_out.h"
#include "esp_log.h"
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

static const char *TAG = "dap";

// Bring up the LCD and show the first screen. The LCD is not required for
// audio, so a failure here is logged and the player carries on.
static void start_display(void) {
  esp_err_t err = lcd_init(PIN_LCD_SDA, PIN_LCD_SCL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Running without an LCD (%s)", esp_err_to_name(err));
    return;
  }
  lcd_define_char(KEYTEST_GLYPH_CODE - 8, KEYTEST_GLYPH);

  screen_t screen;
  keytest_render(&screen, NULL, 0);
  lcd_show(&screen);
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
  // Screen first: it works with or without a card.
  start_display();

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
