#include "debug_console.h"

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "keys.h"
#include "player.h"
#include "player_status.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLE_UART ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define CONSOLE_TASK_STACK 4096
#define CONSOLE_TASK_PRIORITY 4
#define CONSOLE_TASK_CORE 0
#define RX_BUFFER_BYTES 1024
#define LINE_MAX_BYTES 400 // a long path and a start position

static const char *TAG = "dbg";

static QueueHandle_t s_key_events;

// The console's copy of what the LCD shows. The UI task writes it and the
// console task reads it, each inside a critical section that lasts one
// 32-byte copy: neither can see the other half-way through, and the longest
// either can be held up is that copy (well under a microsecond). A mutex
// would do too, but the UI task could then wait behind the console task
// while that was itself waiting for the processor.
static portMUX_TYPE s_screen_lock = portMUX_INITIALIZER_UNLOCKED;
static screen_t s_screen;
static bool s_screen_known;

// The line being received, and a copy that the parser may cut up. One task
// uses them, so they need not be on its stack.
static char s_line[LINE_MAX_BYTES];
static char s_work[LINE_MAX_BYTES];

static char *skip_spaces(char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  return s;
}

static void trim_end(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
}

// A whole number and nothing else.
static bool parse_number(const char *s, long long *out) {
  if (*s == '\0') {
    return false;
  }
  char *end;
  errno = 0;
  long long value = strtoll(s, &end, 10);
  if (errno != 0 || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

static bool all_digits(const char *s) {
  if (*s == '\0') {
    return false;
  }
  for (; *s; s++) {
    if (!isdigit((unsigned char)*s)) {
      return false;
    }
  }
  return true;
}

static const char *player_result(esp_err_t err) {
  return err == ESP_OK ? NULL : esp_err_to_name(err);
}

// Each command returns NULL if it worked, or why it did not.

static const char *cmd_play(char *args) {
  if (*args == '\0') {
    return "no path";
  }
  // The rule for the optional start position is in debug_console.h.
  unsigned long long start_ms = 0;
  char *last = strrchr(args, ' ');
  if (last && all_digits(last + 1)) {
    errno = 0;
    start_ms = strtoull(last + 1, NULL, 10);
    if (errno != 0 || start_ms > UINT32_MAX) {
      return "start position out of range";
    }
    *last = '\0';
    trim_end(args);
  }
  return player_result(player_play(args, (uint32_t)start_ms));
}

static const char *cmd_vol(char *args) {
  long long level;
  if (!parse_number(args, &level)) {
    return "expected a number";
  }
  // Far out of range is still a level; keep it inside an int.
  if (level < -1000) {
    level = -1000;
  } else if (level > 1000) {
    level = 1000;
  }
  return player_result(player_set_volume((int)level));
}

static const char *cmd_seek(char *args) {
  long long delta;
  if (!parse_number(args, &delta) || delta < INT32_MIN || delta > INT32_MAX) {
    return "expected a signed number of milliseconds";
  }
  return player_result(player_seek_rel((int32_t)delta));
}

static const char *cmd_stall(char *args) {
  long long ms;
  if (!parse_number(args, &ms) || ms < 0 || ms > PLAYER_DEBUG_STALL_MAX_MS) {
    return "expected milliseconds, 0 to 30000";
  }
  return player_result(player_debug_stall((uint32_t)ms));
}

static bool post_key(key_event_type_t type, char key) {
  const key_event_t event = {.type = type, .key = key};
  return xQueueSend(s_key_events, &event, pdMS_TO_TICKS(100)) == pdTRUE;
}

static const char *cmd_key(char *args, bool held) {
  if (args[0] == '\0' || args[1] != '\0') {
    return "expected one key";
  }
  char key = (char)toupper((unsigned char)args[0]);
  if (!strchr("123A456B789C*0#D", key)) {
    return "no such key";
  }
  if (!post_key(KEY_PRESS, key) || (held && !post_key(KEY_LONG, key)) ||
      !post_key(KEY_RELEASE, key)) {
    return "key queue full";
  }
  return NULL;
}

static const char *cmd_stat(void) {
  static const char *const STATE[] = {"idle", "playing", "paused"};
  player_status_t s;
  player_get_status(&s);
  ESP_LOGI(TAG,
           "stat state=%s pos_ms=%u vol=%d fill=%u low=%d underruns=%u "
           "wall_ms=%u dec_busy_ms=%u out_busy_ms=%u dec_stack_free=%u "
           "out_stack_free=%u heap_int=%u heap_int_largest=%u",
           STATE[s.state], (unsigned)s.position_ms, s.volume,
           (unsigned)s.ring_fill, (int)s.ring_low, (unsigned)s.underruns,
           (unsigned)s.wall_ms, (unsigned)s.decoder_busy_ms,
           (unsigned)s.output_busy_ms, (unsigned)s.decoder_stack_free,
           (unsigned)s.output_stack_free,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return NULL;
}

static const char *cmd_screen(void) {
  screen_t screen;
  portENTER_CRITICAL(&s_screen_lock);
  screen = s_screen;
  bool known = s_screen_known;
  portEXIT_CRITICAL(&s_screen_lock);
  if (!known) {
    return "nothing shown yet";
  }

  // The formatting and the logging are slow, so they work on the copy.
  char rows[SCREEN_ROWS][SCREEN_COLS + 1];
  for (int r = 0; r < SCREEN_ROWS; r++) {
    for (int c = 0; c < SCREEN_COLS; c++) {
      unsigned char cell = (unsigned char)screen.cells[r][c];
      rows[r][c] = (cell < 0x20 || cell > 0x7e) ? '?' : (char)cell;
    }
    rows[r][SCREEN_COLS] = '\0';
  }
  ESP_LOGI(TAG, "screen \"%s\" \"%s\"", rows[0], rows[1]);
  return NULL;
}

// The screen shows the player's state but is redrawn only when the UI task
// has something to handle. A key event with no key makes it look again, so
// that a change made from here shows up.
static void refresh_screen(void) {
  const key_event_t nothing = {.type = KEY_RELEASE, .key = '\0'};
  xQueueSend(s_key_events, &nothing, 0);
}

static void run_line(const char *line) {
  strlcpy(s_work, line, sizeof(s_work));
  char *word = skip_spaces(s_work);
  trim_end(word);
  if (*word == '\0') {
    return;
  }
  char *args = word;
  while (*args != '\0' && *args != ' ' && *args != '\t') {
    args++;
  }
  if (*args != '\0') {
    *args++ = '\0';
    args = skip_spaces(args);
  }

  const char *why;
  bool no_args = (*args == '\0');
  if (strcmp(word, "play") == 0) {
    why = cmd_play(args);
  } else if (strcmp(word, "pause") == 0 && no_args) {
    why = player_result(player_pause());
  } else if (strcmp(word, "resume") == 0 && no_args) {
    why = player_result(player_resume());
  } else if (strcmp(word, "stop") == 0 && no_args) {
    why = player_result(player_stop());
  } else if (strcmp(word, "vol") == 0) {
    why = cmd_vol(args);
  } else if (strcmp(word, "seek") == 0) {
    why = cmd_seek(args);
  } else if (strcmp(word, "key") == 0) {
    why = cmd_key(args, false);
  } else if (strcmp(word, "long") == 0) {
    why = cmd_key(args, true);
  } else if (strcmp(word, "stall") == 0) {
    why = cmd_stall(args);
  } else if (strcmp(word, "stat") == 0 && no_args) {
    why = cmd_stat();
  } else if (strcmp(word, "screen") == 0 && no_args) {
    why = cmd_screen();
  } else {
    why = "unknown command";
  }

  const char *shown = skip_spaces((char *)line);
  if (why) {
    ESP_LOGW(TAG, "err %s: %s", shown, why);
  } else {
    ESP_LOGI(TAG, "ok %s", shown);
    refresh_screen();
  }
}

static void console_task(void *arg) {
  (void)arg;
  size_t len = 0;
  bool too_long = false;

  for (;;) {
    // Sleep until at least one byte is in, then take whatever came with it.
    uint8_t rx[64];
    int n = uart_read_bytes(CONSOLE_UART, rx, 1, portMAX_DELAY);
    if (n < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000)); // the driver refused: do not spin
      continue;
    }
    if (n == 1) {
      size_t more = 0;
      uart_get_buffered_data_len(CONSOLE_UART, &more);
      if (more > sizeof(rx) - 1) {
        more = sizeof(rx) - 1;
      }
      if (more > 0) {
        int extra = uart_read_bytes(CONSOLE_UART, rx + 1, more, 0);
        if (extra > 0) {
          n += extra;
        }
      }
    }

    for (int i = 0; i < n; i++) {
      char c = (char)rx[i];
      if (c == '\n' || c == '\r') {
        // CRLF gives an empty second line, which is ignored like any other.
        s_line[len] = '\0';
        if (too_long) {
          ESP_LOGW(TAG, "err %.20s...: line too long", s_line);
        } else if (len > 0) {
          run_line(s_line);
        }
        len = 0;
        too_long = false;
      } else if ((unsigned char)c < 0x20 && c != '\t') {
        // Stray control characters (the port being opened, a reset) are
        // not part of any command.
      } else if (len < sizeof(s_line) - 1) {
        s_line[len++] = c;
      } else {
        too_long = true;
      }
    }
  }
}

void debug_console_note_screen(const screen_t *shown) {
  portENTER_CRITICAL(&s_screen_lock);
  s_screen = *shown;
  s_screen_known = true;
  portEXIT_CRITICAL(&s_screen_lock);
}

esp_err_t debug_console_start(QueueHandle_t key_events) {
  if (!key_events) {
    return ESP_ERR_INVALID_ARG;
  }
  s_key_events = key_events;

  // Only receiving goes through the driver. The log keeps writing to the same
  // UART directly, as before; no transmit buffer is asked for, so the driver
  // leaves the transmit side alone.
  esp_err_t err =
      uart_driver_install(CONSOLE_UART, RX_BUFFER_BYTES, 0, 0, NULL, 0);
  if (err != ESP_OK) {
    return err;
  }
  if (xTaskCreatePinnedToCore(console_task, "dbg", CONSOLE_TASK_STACK, NULL,
                              CONSOLE_TASK_PRIORITY, NULL,
                              CONSOLE_TASK_CORE) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
