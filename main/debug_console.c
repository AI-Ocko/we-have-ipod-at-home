#include "debug_console.h"

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "keys.h"
#include "library_task.h"
#include "libscan.h"
#include "player.h"
#include "player_status.h"
#include "playlist_task.h"
#include "state_task.h"
#include "ui.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLE_UART ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define CONSOLE_TASK_STACK 4096
#define CONSOLE_TASK_PRIORITY 4
#define CONSOLE_TASK_CORE 0
#define RX_BUFFER_BYTES 1024
#define LINE_MAX_BYTES 400 // a long path and a start position
#define LIB_LIST_MAX 50     // lines per `lib` listing: more would flood the port
#define QUEUE_LIST_DEFAULT 10 // lines of a `queue` that does not say how many
#define HOLD_MAX_REPEATS 2000
// How long `hold` waits for room in the key queue before it gives up.
#define HOLD_ROOM_WAIT_MS 1000
// How long a `pl` command waits for the playlist task's answer: an edit of
// a 1,000-entry playlist takes well under a second, but the UI's requests
// may be ahead of it in the queue.
#define PL_WAIT_MS 15000
#define PL_CAT_WIDTH 200 // characters of a `pl cat` line per log line
_Static_assert(PL_CAT_WIDTH + 8 <= PLAYLIST_FILE_MAX + 1, "a pl cat line fits");

static const char KEY_CHARS[] = "123A456B789C*0#D";

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
static char s_path[LIBSCAN_PATH_MAX]; // a track's path, for the `lib` lines

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
  uint32_t id;
  esp_err_t err = player_play(args, (uint32_t)start_ms, &id);
  if (err == ESP_OK) {
    // The number the track's events will carry (`EVENT id=` in the UI
    // task's log). The track can start, or fail, before this line is out.
    ESP_LOGI(TAG, "play id=%" PRIu32, id);
  }
  return player_result(err);
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
  if (!strchr(KEY_CHARS, key)) {
    return "no such key";
  }
  if (!post_key(KEY_PRESS, key) || (held && !post_key(KEY_LONG, key)) ||
      !post_key(KEY_RELEASE, key)) {
    return "key queue full";
  }
  return NULL;
}

// One event of a `hold`. Waits for room: the queue holds 16 and a hold can
// be 2003 events, so the UI task has to take them as they come. False if
// there was still no room after HOLD_ROOM_WAIT_MS.
static bool post_held(key_event_type_t type, char key, uint32_t *posted) {
  const key_event_t event = {.type = type, .key = key};
  if (xQueueSend(s_key_events, &event, pdMS_TO_TICKS(HOLD_ROOM_WAIT_MS)) !=
      pdTRUE) {
    return false;
  }
  (*posted)++;
  return true;
}

// `hold <char> <repeats>`: what is posted, and how, is in debug_console.h.
static const char *cmd_hold(char *args) {
  char *count = args;
  while (*count != '\0' && *count != ' ' && *count != '\t') {
    count++;
  }
  if (*count != '\0') {
    *count++ = '\0';
    count = skip_spaces(count);
  }
  if (args[0] == '\0' || args[1] != '\0') {
    return "expected one key and a repeat count";
  }
  char key = (char)toupper((unsigned char)args[0]);
  if (!strchr(KEY_CHARS, key)) {
    return "no such key";
  }
  if (*count == '\0') {
    return "no repeat count";
  }
  long long repeats;
  if (!all_digits(count) || !parse_number(count, &repeats) ||
      repeats > HOLD_MAX_REPEATS) {
    return "expected a repeat count, 0 to 2000";
  }

  uint32_t wanted = (uint32_t)repeats + 3;
  uint32_t posted = 0;
  bool room = post_held(KEY_PRESS, key, &posted) &&
              post_held(KEY_LONG, key, &posted);
  for (long long i = 0; room && i < repeats; i++) {
    room = post_held(KEY_REPEAT, key, &posted);
  }
  // The release is tried even when the queue stopped taking events, so that
  // the UI task is not left believing the key is still down.
  post_held(KEY_RELEASE, key, &posted);

  ESP_LOGI(TAG, "hold key=%c repeats=%lld posted=%" PRIu32 " dropped=%" PRIu32,
           key, repeats, posted, wanted - posted);
  return posted == wanted ? NULL : "key queue full: events were dropped";
}

static const char *cmd_stat(void) {
  // Internal RAM a buffer can use: MALLOC_CAP_INTERNAL alone counts IRAM-only
  // heap too, which holds no data (see debug_console.h).
  const uint32_t DATA_RAM = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  static const char *const STATE[] = {"idle", "playing", "paused"};
  player_status_t s;
  player_get_status(&s);
  ui_stats_t ui;
  ui_get_stats(&ui);
  ESP_LOGI(TAG,
           "stat state=%s pos_ms=%u vol=%d fill=%u low=%d underruns=%u "
           "wall_ms=%u dec_busy_ms=%u out_busy_ms=%u dec_stack_free=%u "
           "out_stack_free=%u codec_pool=%u codec_pool_free=%u "
           "codec_pool_min=%u heap_int=%u heap_int_largest=%u "
           "heap_data=%u heap_data_largest=%u heap_data_min=%u psram=%u "
           "psram_largest=%u "
           "ui_stack_free=%u ui_passes=%u ui_lcd=%u ui_light=%s "
           "ui_backlight=%u ui_sleep_s=%u ui_fading=%d",
           STATE[s.state], (unsigned)s.position_ms, s.volume,
           (unsigned)s.ring_fill, (int)s.ring_low, (unsigned)s.underruns,
           (unsigned)s.wall_ms, (unsigned)s.decoder_busy_ms,
           (unsigned)s.output_busy_ms, (unsigned)s.decoder_stack_free,
           (unsigned)s.output_stack_free, (unsigned)s.codec_pool_size,
           (unsigned)s.codec_pool_free, (unsigned)s.codec_pool_free_min,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(DATA_RAM),
           (unsigned)heap_caps_get_largest_free_block(DATA_RAM),
           (unsigned)heap_caps_get_minimum_free_size(DATA_RAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
           (unsigned)ui.stack_free, (unsigned)ui.passes,
           (unsigned)ui.lcd_updates, ui.lit ? "on" : "off",
           (unsigned)ui.backlight, (unsigned)ui.sleep_left_s, (int)ui.fading);
  return NULL;
}

// What `screen` prints for a cell: the character itself if it is printable
// ASCII, a stand-in for each of the UI's user-defined characters, `?` for
// anything else.
static char screen_char(unsigned char cell) {
  switch (cell) {
  case UI_GLYPH_PLAY:
    return '>';
  case UI_GLYPH_PAUSE:
    return '=';
  case UI_GLYPH_BAR_FULL:
    return '#';
  case UI_GLYPH_BAR_HALF:
    return '+';
  case UI_GLYPH_BAR_EMPTY:
    return '-';
  case UI_GLYPH_LOOP_ALL:
    return 'L';
  case UI_GLYPH_LOOP_ONE:
    return '1';
  default:
    return (cell < 0x20 || cell > 0x7e) ? '?' : (char)cell;
  }
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
      rows[r][c] = screen_char((unsigned char)screen.cells[r][c]);
    }
    rows[r][SCREEN_COLS] = '\0';
  }
  ESP_LOGI(TAG, "screen \"%s\" \"%s\"", rows[0], rows[1]);
  return NULL;
}

static const char *cmd_scan(void) {
  switch (library_task_rescan()) {
  case ESP_OK:
    return NULL;
  case ESP_ERR_NO_MEM:
    return "no memory for a library";
  default:
    return "no card, or a scan is already running";
  }
}

// --- `lib`: what is in the index ---------------------------------------------
//
// Each of these holds the index (library_task_get) only while it prints, at
// most LIB_LIST_MAX lines, and lets go before returning.

static void log_artist(const library_t *lib, uint32_t artist) {
  uint32_t first, albums, tracks;
  library_artist_albums(lib, artist, &first, &albums);
  library_artist_tracks(lib, artist, &first, &tracks);
  ESP_LOGI(TAG, "artist %" PRIu32 " \"%s\" albums=%" PRIu32 " tracks=%" PRIu32,
           artist, library_artist_name(lib, artist), albums, tracks);
}

static void log_album(const library_t *lib, uint32_t album) {
  uint32_t first, tracks;
  library_album_tracks(lib, album, &first, &tracks);
  ESP_LOGI(TAG, "album %" PRIu32 " \"%s\" artist=%" PRIu32 " tracks=%" PRIu32,
           album, library_album_name(lib, album),
           library_album_artist(lib, album), tracks);
}

static void log_track(const library_t *lib, uint32_t track) {
  library_track_t t;
  library_track(lib, track, &t);
  library_track_path(lib, track, s_path, sizeof(s_path));
  ESP_LOGI(TAG,
           "track %" PRIu32 " \"%s\" artist=\"%s\" album=%" PRIu32
           " no=%u.%u ms=%" PRIu32 " path=\"%s\"",
           track, t.title, t.artist, t.album, (unsigned)t.disc_no,
           (unsigned)t.track_no, t.duration_ms, s_path);
}

typedef enum { LIST_ARTISTS, LIST_ALBUMS, LIST_TRACKS, LIST_SONGS } list_t;

// Print items [from, from + count) of a list that starts at `first` and has
// `total` items, then one line that says what was printed.
static void log_list(const library_t *lib, list_t what, uint32_t first,
                     uint32_t total, uint32_t from, uint32_t count) {
  if (from > total) {
    from = total;
  }
  if (count > total - from) {
    count = total - from;
  }
  // Cut short by the limit, and not by the list or the request ending?
  bool capped = count > LIB_LIST_MAX;
  if (capped) {
    count = LIB_LIST_MAX;
  }
  for (uint32_t i = from; i < from + count; i++) {
    switch (what) {
    case LIST_ARTISTS:
      log_artist(lib, first + i);
      break;
    case LIST_ALBUMS:
      log_album(lib, first + i);
      break;
    case LIST_TRACKS:
      log_track(lib, first + i);
      break;
    case LIST_SONGS:
      log_track(lib, library_song_by_title(lib, i));
      break;
    }
  }
  ESP_LOGI(TAG, "lib listed=%" PRIu32 " from=%" PRIu32 " of=%" PRIu32 "%s",
           count, from, total, capped ? " capped" : "");
}

// Up to `max` numbers separated by spaces, and nothing else. Returns how
// many there were, or -1.
static int parse_numbers(char *args, uint32_t *out, int max) {
  int n = 0;
  char *save = NULL;
  for (char *word = strtok_r(args, " \t", &save); word != NULL;
       word = strtok_r(NULL, " \t", &save)) {
    long long value;
    if (n == max || !all_digits(word) || !parse_number(word, &value) ||
        value > UINT32_MAX) {
      return -1;
    }
    out[n++] = (uint32_t)value;
  }
  return n;
}

static const char *cmd_lib_with(const library_t *lib, char *args) {
  if (*args == '\0') {
    ESP_LOGI(TAG,
             "lib tracks=%" PRIu32 " albums=%" PRIu32 " artists=%" PRIu32
             " bytes=%" PRIu32,
             library_track_count(lib), library_album_count(lib),
             library_artist_count(lib), library_bytes(lib));
    return NULL;
  }
  char *rest = args;
  while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
    rest++;
  }
  if (*rest != '\0') {
    *rest++ = '\0';
    rest = skip_spaces(rest);
  }

  if (strcmp(args, "find") == 0) {
    uint32_t track;
    if (*rest == '\0') {
      return "no path";
    }
    if (!library_find_path(lib, rest, &track)) {
      return "not in the library";
    }
    log_track(lib, track);
    return NULL;
  }

  uint32_t n[2] = {0, 0};
  int given = parse_numbers(rest, n, 2);
  uint32_t first, total;
  if (strcmp(args, "artists") == 0 && given >= 0 && given <= 1) {
    log_list(lib, LIST_ARTISTS, 0, library_artist_count(lib), n[0],
             UINT32_MAX);
  } else if (strcmp(args, "albums") == 0 && given >= 1) {
    if (n[0] >= library_artist_count(lib)) {
      return "no such artist";
    }
    library_artist_albums(lib, n[0], &first, &total);
    log_list(lib, LIST_ALBUMS, first, total, n[1], UINT32_MAX);
  } else if (strcmp(args, "tracks") == 0 && given >= 1) {
    if (n[0] >= library_album_count(lib)) {
      return "no such album";
    }
    library_album_tracks(lib, n[0], &first, &total);
    log_list(lib, LIST_TRACKS, first, total, n[1], UINT32_MAX);
  } else if (strcmp(args, "songs") == 0 && given == 2) {
    log_list(lib, LIST_SONGS, 0, library_track_count(lib), n[0], n[1]);
  } else {
    return "expected: lib | lib artists [from] | lib albums <artist#> [from] "
           "| lib tracks <album#> [from] | lib songs <from> <count> | "
           "lib find <path>";
  }
  return NULL;
}

static const char *cmd_lib(char *args) {
  const library_t *lib = library_task_get();
  if (lib == NULL) {
    return "no library (none loaded yet, or a scan is running)";
  }
  const char *why = cmd_lib_with(lib, args);
  library_task_put();
  return why;
}

// `queue [from [count]]`: the lines printed are in debug_console.h.
static const char *cmd_queue(char *args) {
  uint32_t n[2] = {UINT32_MAX, QUEUE_LIST_DEFAULT};
  if (parse_numbers(args, n, 2) < 0) {
    return "expected: queue [from [count]]";
  }
  // Static: one task uses it, and it need not be on its stack.
  static ui_queue_report_t r;
  if (!ui_queue_report(n[0], n[1], &r)) {
    return "the UI task did not answer";
  }
  ESP_LOGI(TAG,
           "queue count=%" PRIu32 " position=%" PRIu32 " shuffle=%d loop=%d "
           "index=%08" PRIx32,
           r.count, r.position, (int)r.shuffle, (int)r.loop, r.index);
  // The numbers are the UI's and the paths the library's. They go together
  // only if the index now loaded is the one the queue was built for.
  const library_t *lib = library_task_get();
  bool same = lib != NULL && library_checksum(lib) == r.index;
  for (uint32_t i = 0; i < r.listed; i++) {
    s_path[0] = '\0';
    if (same) {
      library_track_path(lib, r.tracks[i], s_path, sizeof(s_path));
    }
    ESP_LOGI(TAG, "queue %" PRIu32 " track=%u path=\"%s\"", r.from + i,
             (unsigned)r.tracks[i], same ? s_path : "?");
  }
  if (lib != NULL) {
    library_task_put();
  }
  ESP_LOGI(TAG, "queue listed=%" PRIu32 " from=%" PRIu32 " of=%" PRIu32 "%s",
           r.listed, r.from, r.count,
           (n[1] > UI_QUEUE_REPORT_MAX && r.listed == UI_QUEUE_REPORT_MAX)
               ? " capped"
               : "");
  return NULL;
}

// A saved value, or `-` for one that is not saved.
static const char *saved(bool has, uint32_t value, char *text, size_t size) {
  if (!has) {
    return "-";
  }
  snprintf(text, size, "%" PRIu32, value);
  return text;
}

// `state` and `state clear`: the lines printed are in debug_console.h.
static const char *cmd_state(char *args) {
  if (strcmp(args, "clear") == 0) {
    return state_task_clear() ? NULL : "not everything could be erased";
  }
  if (*args != '\0') {
    return "expected: state | state clear";
  }
  state_report_t r;
  if (!state_task_report(&r)) {
    return "no state task";
  }
  char text[7][12];
  ESP_LOGI(TAG,
           "state flash=%s volume=%s shuffle=%s loop=%s backlight=%s "
           "position=%s track=%s elapsed_ms=%s stack_free=%u",
           r.flash ? "ok" : "unusable",
           saved(r.has_volume, r.volume, text[0], sizeof(text[0])),
           saved(r.has_shuffle, r.shuffle, text[1], sizeof(text[1])),
           saved(r.has_loop, r.loop, text[2], sizeof(text[2])),
           saved(r.has_backlight, r.backlight, text[3], sizeof(text[3])),
           saved(r.has_position, r.position.position, text[4], sizeof(text[4])),
           saved(r.has_position, r.position.track, text[5], sizeof(text[5])),
           saved(r.has_position, r.position.elapsed_ms, text[6],
                 sizeof(text[6])),
           (unsigned)r.stack_free);
  char index[12] = "-";
  if (r.index) {
    snprintf(index, sizeof(index), "%08" PRIx32, r.index_checksum);
  }
  if (r.file) {
    ESP_LOGI(TAG,
             "state queue_file=present bytes=%" PRIu32 " count=%" PRIu32
             " index=%08" PRIx32 " loaded_index=%s match=%s usable=%s "
             "unfinished=%s",
             r.file_bytes, r.file_count, r.file_index, index,
             (r.index && r.file_index == r.index_checksum) ? "yes" : "no",
             r.usable ? "yes" : "no", r.file_unfinished ? "yes" : "no");
  } else {
    ESP_LOGI(TAG, "state queue_file=absent loaded_index=%s unfinished=%s",
             index, r.file_unfinished ? "yes" : "no");
  }
  return NULL;
}

// --- `pl`: playlists ----------------------------------------------------------
//
// Every `pl` command goes through the playlist task's requests, as the UI's
// do, with PLAYLIST_CLIENT_CONSOLE: its events come to the console's own
// queue (playlist_task_wait), so it never takes one meant for the UI, and
// it never touches the card itself. A playlist is named by its number in
// the catalog, which each command lists afresh. The results are copied into
// the console's own buffers, in PSRAM, allocated once at start.

typedef struct {
  playlist_catalog_t catalog;
  playlist_loaded_t loaded; // also the tracks a command parses
  playlist_bytes_t bytes;
  char file[PLAYLIST_FILE_MAX + 1]; // the playlist a command names
  char text[PLAYLIST_FILE_MAX + 1]; // a name for `pl ls`, a line for `pl cat`
} pl_memory_t;
static pl_memory_t *s_pl;

// Wait for request `request` (0: it was refused) and log its event. NULL if
// it ended done, or why not.
static const char *pl_wait(uint32_t request) {
  if (request == 0) {
    return "busy: the playlist task cannot take a request";
  }
  playlist_event_t e;
  if (!playlist_task_wait(request, &e, PL_WAIT_MS)) {
    return "the playlist task did not answer";
  }
  static const char *const NAME[] = {"ok",      "empty",     "too long",
                                     "bad char", "bad end",  "bad start",
                                     "reserved"};
  if (e.outcome == PLAYLIST_TASK_BAD_NAME) {
    ESP_LOGI(TAG, "pl event request=%" PRIu32 " kind=%s outcome=%s name=%s",
             e.request, playlist_task_request_name(e.kind),
             playlist_task_outcome_name(e.outcome),
             e.name < sizeof(NAME) / sizeof(NAME[0]) ? NAME[e.name] : "?");
  } else {
    ESP_LOGI(TAG, "pl event request=%" PRIu32 " kind=%s outcome=%s",
             e.request, playlist_task_request_name(e.kind),
             playlist_task_outcome_name(e.outcome));
  }
  return e.outcome == PLAYLIST_TASK_DONE
             ? NULL
             : playlist_task_outcome_name(e.outcome);
}

// The catalog, listed now, into &s_pl->catalog.
static const char *pl_list(void) {
  uint32_t request = playlist_task_list(PLAYLIST_CLIENT_CONSOLE);
  const char *why = pl_wait(request);
  if (why == NULL &&
      !playlist_task_copy_catalog(PLAYLIST_CLIENT_CONSOLE, request,
                                  &s_pl->catalog)) {
    why = "the listing was replaced before it was copied";
  }
  return why;
}

// The next word of `*args` (cut off with a '\0'), or "" at the end.
static char *next_word(char **args) {
  char *word = skip_spaces(*args);
  char *end = word;
  while (*end != '\0' && *end != ' ' && *end != '\t') {
    end++;
  }
  if (*end != '\0') {
    *end++ = '\0';
  }
  *args = end;
  return word;
}

// Playlist number <n>, the next word: its file name into s_pl->file.
static const char *pl_file(char **args) {
  long long n;
  char *word = next_word(args);
  if (!all_digits(word) || !parse_number(word, &n)) {
    return "expected a playlist number (see pl ls)";
  }
  const char *why = pl_list();
  if (why != NULL) {
    return why;
  }
  if (n >= s_pl->catalog.count) {
    return "no such playlist";
  }
  strlcpy(s_pl->file, playlist_catalog_file(&s_pl->catalog, (uint32_t)n),
          sizeof(s_pl->file));
  return NULL;
}

// Track numbers, the rest of the line, into s_pl->loaded.tracks.
static const char *pl_tracks(char *args, uint32_t *count) {
  *count = 0;
  for (char *word = next_word(&args); *word != '\0';
       word = next_word(&args)) {
    long long t;
    if (!all_digits(word) || !parse_number(word, &t) || t >= UINT16_MAX) {
      return "expected track numbers";
    }
    if (*count == PLAYLIST_ENTRIES_MAX) {
      return "too many tracks";
    }
    s_pl->loaded.tracks[(*count)++] = (uint16_t)t;
  }
  return NULL;
}

// The checksum of the index now loaded (0 if none: the task then answers
// library busy).
static uint32_t pl_index(void) {
  const library_t *lib = library_task_get();
  uint32_t index = library_checksum(lib);
  if (lib != NULL) {
    library_task_put();
  }
  return index;
}

// An entry number, the next word.
static bool pl_entry(char **args, uint32_t *entry) {
  long long n;
  char *word = next_word(args);
  if (!all_digits(word) || !parse_number(word, &n) || n > UINT32_MAX) {
    return false;
  }
  *entry = (uint32_t)n;
  return true;
}

// A name: in double quotes, or the words up to `stop_at_digits` (the first
// word made of digits only) or the end.
static char *pl_name(char **args, bool stop_at_digits) {
  char *s = skip_spaces(*args);
  if (*s == '"') {
    char *end = strchr(s + 1, '"');
    if (end == NULL) {
      *args = s + strlen(s);
      return s + 1;
    }
    *end = '\0';
    *args = end + 1;
    return s + 1;
  }
  char *name = s;
  char *p = s;
  while (*p != '\0') {
    char *word = skip_spaces(p);
    char *end = word;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
      end++;
    }
    bool digits = end > word;
    for (char *q = word; q < end; q++) {
      digits = digits && isdigit((unsigned char)*q);
    }
    if (stop_at_digits && digits && word != name) {
      if (p > name) {
        *p = '\0'; // the space before the first number
      }
      *args = word;
      return name;
    }
    p = end;
  }
  *args = p;
  return name;
}

static const char *pl_ls(void) {
  const char *why = pl_list();
  if (why != NULL) {
    return why;
  }
  char *name = s_pl->text;
  for (uint32_t i = 0; i < s_pl->catalog.count; i++) {
    playlist_catalog_name(&s_pl->catalog, i, name, sizeof(s_pl->text));
    ESP_LOGI(TAG, "pl %" PRIu32 " \"%s\" file=\"%s\"", i, name,
             playlist_catalog_file(&s_pl->catalog, i));
  }
  ESP_LOGI(TAG, "pl playlists=%" PRIu32 "%s", s_pl->catalog.count,
           s_pl->catalog.truncated ? " truncated" : "");
  return NULL;
}

static const char *pl_show(char *args) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  uint32_t request = playlist_task_load(PLAYLIST_CLIENT_CONSOLE, s_pl->file);
  why = pl_wait(request);
  if (why != NULL) {
    return why;
  }
  if (!playlist_task_copy_load(PLAYLIST_CLIENT_CONSOLE, request,
                               &s_pl->loaded)) {
    return "the load was replaced before it was copied";
  }
  // The numbers go with the paths only if the index is still the one the
  // load used. It is held for one path at a time, not across the printing.
  for (uint32_t i = 0; i < s_pl->loaded.count; i++) {
    uint16_t t = s_pl->loaded.tracks[i];
    if (t == PLAYLIST_MISSING) {
      ESP_LOGI(TAG, "pl entry %" PRIu32 " missing", i);
      continue;
    }
    const library_t *lib = library_task_get();
    bool same = lib != NULL && library_checksum(lib) == s_pl->loaded.index;
    if (same) {
      library_track_path(lib, t, s_path, sizeof(s_path));
    }
    if (lib != NULL) {
      library_task_put();
    }
    ESP_LOGI(TAG, "pl entry %" PRIu32 " track=%u path=\"%s\"", i, (unsigned)t,
             same ? s_path : "?");
  }
  ESP_LOGI(TAG, "pl entries=%" PRIu32 " index=%08" PRIx32 "%s",
           s_pl->loaded.count, s_pl->loaded.index,
           s_pl->loaded.truncated ? " truncated" : "");
  return NULL;
}

// `pl cat`: one output line per line of the file; see debug_console.h.
static void pl_cat_flush(char *text, size_t *len, uint32_t line,
                         const char *after) {
  text[*len] = '\0';
  ESP_LOGI(TAG, "pl cat %" PRIu32 " \"%s\"%s", line, text, after);
  *len = 0;
}

static const char *pl_cat(char *args) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  char *text = s_pl->text; // PL_CAT_WIDTH and one escape fit
  size_t len = 0;
  uint32_t line = 0, offset = 0, size = 0;
  bool open_line = false; // bytes of a line not yet ended by an LF
  do {
    uint32_t request =
        playlist_task_read(PLAYLIST_CLIENT_CONSOLE, s_pl->file, offset);
    why = pl_wait(request);
    if (why != NULL) {
      return why;
    }
    if (!playlist_task_copy_bytes(PLAYLIST_CLIENT_CONSOLE, request,
                                  &s_pl->bytes)) {
      return "the read was replaced before it was copied";
    }
    size = s_pl->bytes.size;
    for (uint32_t i = 0; i < s_pl->bytes.length; i++) {
      unsigned char c = s_pl->bytes.bytes[i];
      if (c == '\n') {
        pl_cat_flush(text, &len, line++, "");
        open_line = false;
        continue;
      }
      open_line = true;
      if (len >= PL_CAT_WIDTH) {
        pl_cat_flush(text, &len, line, " more");
      }
      if (c == '\\' || c == '"') {
        len += (size_t)snprintf(text + len, 8, "\\%c", c);
      } else if (c == '\r') {
        len += (size_t)snprintf(text + len, 8, "\\r");
      } else if (c == '\t') {
        len += (size_t)snprintf(text + len, 8, "\\t");
      } else if (c < 0x20 || c > 0x7e) {
        len += (size_t)snprintf(text + len, 8, "\\x%02X", c);
      } else {
        text[len++] = (char)c;
      }
    }
    offset += s_pl->bytes.length;
    if (s_pl->bytes.length == 0) {
      break; // the file shrank under us
    }
  } while (offset < size);
  if (open_line) {
    pl_cat_flush(text, &len, line++, " nolf");
  }
  ESP_LOGI(TAG, "pl cat bytes=%" PRIu32 " lines=%" PRIu32, size, line);
  return NULL;
}

static const char *pl_new(char *args) {
  char *name = pl_name(&args, true);
  uint32_t count;
  const char *why = pl_tracks(args, &count);
  if (why != NULL) {
    return why;
  }
  return pl_wait(playlist_task_create(PLAYLIST_CLIENT_CONSOLE, name,
                                      s_pl->loaded.tracks, count, pl_index()));
}

static const char *pl_add(char *args) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  uint32_t count;
  why = pl_tracks(args, &count);
  if (why != NULL) {
    return why;
  }
  if (count == 0) {
    return "expected track numbers";
  }
  return pl_wait(playlist_task_append(PLAYLIST_CLIENT_CONSOLE, s_pl->file,
                                      s_pl->loaded.tracks, count,
                                      pl_index()));
}

// `pl rm`, `pl up`, `pl down`.
static const char *pl_entry_edit(char *args, playlist_request_t kind) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  uint32_t entry;
  if (!pl_entry(&args, &entry) || *skip_spaces(args) != '\0') {
    return "expected a playlist number and an entry number";
  }
  uint32_t request =
      kind == PLAYLIST_REQUEST_REMOVE
          ? playlist_task_remove(PLAYLIST_CLIENT_CONSOLE, s_pl->file, entry)
      : kind == PLAYLIST_REQUEST_MOVE_UP
          ? playlist_task_move_up(PLAYLIST_CLIENT_CONSOLE, s_pl->file, entry)
          : playlist_task_move_down(PLAYLIST_CLIENT_CONSOLE, s_pl->file, entry);
  return pl_wait(request);
}

static const char *pl_mv(char *args) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  char *name = pl_name(&args, false);
  return pl_wait(playlist_task_rename(PLAYLIST_CLIENT_CONSOLE, s_pl->file, name));
}

static const char *pl_del(char *args) {
  const char *why = pl_file(&args);
  if (why != NULL) {
    return why;
  }
  if (*skip_spaces(args) != '\0') {
    return "expected a playlist number";
  }
  return pl_wait(playlist_task_delete(PLAYLIST_CLIENT_CONSOLE, s_pl->file));
}

// The card's size and free space, read by the playlist task as About's is.
static const char *pl_free(void) {
  uint32_t request = playlist_task_space(PLAYLIST_CLIENT_CONSOLE);
  const char *why = pl_wait(request);
  if (why != NULL) {
    return why;
  }
  playlist_space_t space;
  if (!playlist_task_copy_space(PLAYLIST_CLIENT_CONSOLE, request, &space)) {
    return "the answer was replaced by a newer one";
  }
  ESP_LOGI(TAG, "pl free total=%" PRIu64 " free=%" PRIu64, space.total_bytes,
           space.free_bytes);
  return NULL;
}

static const char *cmd_pl(char *args) {
  if (s_pl == NULL) {
    return "no memory for the pl commands";
  }
  char *sub = next_word(&args);
  if (strcmp(sub, "ls") == 0 && *skip_spaces(args) == '\0') {
    return pl_ls();
  } else if (strcmp(sub, "show") == 0) {
    return pl_show(args);
  } else if (strcmp(sub, "cat") == 0) {
    return pl_cat(args);
  } else if (strcmp(sub, "new") == 0) {
    return pl_new(args);
  } else if (strcmp(sub, "add") == 0) {
    return pl_add(args);
  } else if (strcmp(sub, "rm") == 0) {
    return pl_entry_edit(args, PLAYLIST_REQUEST_REMOVE);
  } else if (strcmp(sub, "up") == 0) {
    return pl_entry_edit(args, PLAYLIST_REQUEST_MOVE_UP);
  } else if (strcmp(sub, "down") == 0) {
    return pl_entry_edit(args, PLAYLIST_REQUEST_MOVE_DOWN);
  } else if (strcmp(sub, "mv") == 0) {
    return pl_mv(args);
  } else if (strcmp(sub, "del") == 0) {
    return pl_del(args);
  } else if (strcmp(sub, "free") == 0 && *skip_spaces(args) == '\0') {
    return pl_free();
  }
  return "expected: pl ls | pl show <n> | pl cat <n> | pl new <name> "
         "[track ...] | pl add <n> <track> [track ...] | pl rm <n> <entry> | "
         "pl up <n> <entry> | pl down <n> <entry> | pl mv <n> <name> | "
         "pl del <n> | pl free";
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
    ui_stop(); // the UI stops the player, and knows that it has
    why = NULL;
  } else if (strcmp(word, "vol") == 0) {
    why = cmd_vol(args);
  } else if (strcmp(word, "seek") == 0) {
    why = cmd_seek(args);
  } else if (strcmp(word, "key") == 0) {
    why = cmd_key(args, false);
  } else if (strcmp(word, "long") == 0) {
    why = cmd_key(args, true);
  } else if (strcmp(word, "hold") == 0) {
    why = cmd_hold(args);
  } else if (strcmp(word, "stall") == 0) {
    why = cmd_stall(args);
  } else if (strcmp(word, "stat") == 0 && no_args) {
    why = cmd_stat();
  } else if (strcmp(word, "screen") == 0 && no_args) {
    why = cmd_screen();
  } else if (strcmp(word, "scan") == 0 && no_args) {
    why = cmd_scan();
  } else if (strcmp(word, "lib") == 0) {
    why = cmd_lib(args);
  } else if (strcmp(word, "state") == 0) {
    why = cmd_state(args);
  } else if (strcmp(word, "queue") == 0) {
    why = cmd_queue(args);
  } else if (strcmp(word, "pl") == 0) {
    why = cmd_pl(args);
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

  // The `pl` commands' copies of the playlist task's results (about 24 KB,
  // in PSRAM). Without them those commands say so; the rest works.
  s_pl = heap_caps_malloc(sizeof(*s_pl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

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
