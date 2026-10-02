#include "esp_app_desc.h"
#include "keytest.h"
#include "lcd.h"
#include "marquee.h"
#include "options.h"
#include "pcm.h"
#include "ui.h"
#include "ui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Drawing: every screen as a function of the UI task's state. Nothing here
// changes that state, takes the index or waits; ui.c calls ui_render once per
// pass of its loop and sends the LCD whatever differs from what it shows.
//
// THE LAYOUTS (16 columns; `>` `=` `#` `+` `-` `L` `1` stand for the
// user-defined characters play, pause, a full, a half and an empty cell of
// the bar, and the loop-all and loop-one marks, which is how the console's
// `screen` command prints them)
//
//   a list              |Artists    14/17|   menu_header: name and position
//                       |>Radiohead      |   `>` (plain ASCII) and the item,
//                                            a marquee in 15 columns
//   an empty list       |Artists         |   the name alone
//                       |(empty)         |
//   the options menu    |Kid A           |   the item's name, cut to fit
//                       |>Add to queue   |   `>` and the action
//   Now Playing         |Hey Mama - Kanye|   "Title - Artist", a marquee
//                       |> 0:12/5:05  S L|   symbol, space, m:ss/m:ss; in
//                                            column 13 `S` while shuffle is
//                                            on, in column 15 the loop mark
//                                            while loop is all or one
//   ... a notice        |Hey Mama - Kanye|
//                       |Can't play      |   or "File missing", for 1.5 s
//   ... no track        |Nothing playing |
//                       |             S L|   the marks alone
//   the overlay         |Vol#####+---- 11|   replaces the bottom row of any
//                       |Shuffle on      |   screen but the key test: the
//                                            volume bar, or "Shuffle on",
//                                            "Shuffle off", "Loop off",
//                                            "Loop all", "Loop one", "Added",
//                                            "Queue full"
//   About               |1028 tracks     |
//                       |Built 2026-10-02|
//   scanning            |Scanning...     |
//                       |412 tracks      |
//   waiting             |Insert SD card  |
//                       |                |
//   key test            keytest_render (keytest.h)

#define ITEM_COL 1 // the item starts after the `>`
#define ITEM_WIDTH (SCREEN_COLS - ITEM_COL)

// One cell of the bar stands for two volume levels.
#define BAR_COL 3
#define BAR_CELLS (PCM_VOLUME_MAX / 2)

// Now Playing's marks, at the right end of the bottom row. A time of a
// hundred minutes or more runs under them; they are drawn over it.
#define SHUFFLE_COL 13
#define LOOP_COL 15

// The bar segments are a tube: two rails, filled from the left.
static const uint8_t GLYPH_PAUSE[8] = {0x00, 0x1B, 0x1B, 0x1B,
                                       0x1B, 0x1B, 0x1B, 0x00};
static const uint8_t GLYPH_BAR_FULL[8] = {0x00, 0x1F, 0x1F, 0x1F,
                                          0x1F, 0x1F, 0x1F, 0x00};
static const uint8_t GLYPH_BAR_HALF[8] = {0x00, 0x1F, 0x1C, 0x1C,
                                          0x1C, 0x1C, 0x1F, 0x00};
static const uint8_t GLYPH_BAR_EMPTY[8] = {0x00, 0x1F, 0x00, 0x00,
                                           0x00, 0x00, 0x1F, 0x00};
// Loop all: two arrows chasing each other round. Loop one: the upper arrow
// over a 1.
//   . . . # .      . . . # .
//   # # # # #      # # # # #
//   # . . # .      . . . # .
//   # . . . .      . . . . .
//   . . . . #      . . # . .
//   . # . . #      . # # . .
//   # # # # #      . . # . .
//   . # . . .      . # # # .
static const uint8_t GLYPH_LOOP_ALL[8] = {0x02, 0x1F, 0x12, 0x10,
                                          0x01, 0x09, 0x1F, 0x08};
static const uint8_t GLYPH_LOOP_ONE[8] = {0x02, 0x1F, 0x02, 0x00,
                                          0x04, 0x0C, 0x04, 0x0E};

void ui_screens_define_glyphs(void) {
  // A code in a cell is 8 + the character's index. Failures are not
  // reported: with no LCD there is nothing to define, and with one that has
  // dropped off the bus the driver sends them again when it comes back.
  lcd_define_char(UI_GLYPH_PLAY - 8, KEYTEST_GLYPH);
  lcd_define_char(UI_GLYPH_PAUSE - 8, GLYPH_PAUSE);
  lcd_define_char(UI_GLYPH_BAR_FULL - 8, GLYPH_BAR_FULL);
  lcd_define_char(UI_GLYPH_BAR_HALF - 8, GLYPH_BAR_HALF);
  lcd_define_char(UI_GLYPH_BAR_EMPTY - 8, GLYPH_BAR_EMPTY);
  lcd_define_char(UI_GLYPH_LOOP_ALL - 8, GLYPH_LOOP_ALL);
  lcd_define_char(UI_GLYPH_LOOP_ONE - 8, GLYPH_LOOP_ONE);
}

// How long something has been on show, for its marquee. (It wraps after 49
// days; the marquee then jumps once.)
static uint32_t ms_since(int64_t since_us, int64_t now_us) {
  return (uint32_t)((now_us - since_us) / 1000);
}

// Milliseconds until `until_us`, rounded up: at least 1 for a time still to
// come, so that whoever sleeps this long wakes at or after it; 0 for one
// that has come.
static uint32_t ms_until(int64_t until_us, int64_t now_us) {
  if (until_us <= now_us) {
    return 0;
  }
  return (uint32_t)((until_us - now_us + 999) / 1000);
}

static bool overlay_on(const ui_t *ui, int64_t now_us) {
  return now_us < ui->overlay_until_us;
}

static void render_list(const ui_t *ui, int64_t now_us, screen_t *screen) {
  menu_header(ui->list_name, &ui->list_menu, screen->cells[0]);
  if (ui->list_menu.count == 0) {
    screen_set_row(screen, 1, "(empty)");
    return;
  }
  screen->cells[1][0] = '>';
  marquee_window(ui->list_item, ITEM_WIDTH, ms_since(ui->item_since_us, now_us),
                 &screen->cells[1][ITEM_COL]);
}

static void render_options(const ui_t *ui, screen_t *screen) {
  screen_set_row(screen, 0, ui->options_title);
  screen_set_row(screen, 1, ">");
  screen_put(screen, 1, ITEM_COL,
             options_text(ui->options[ui->options_menu.cursor]));
}

static void render_scanning(const ui_t *ui, screen_t *screen) {
  // The count arrives at most four times a second.
  char count[2 * SCREEN_COLS];
  snprintf(count, sizeof(count), "%" PRIu32 " tracks", ui->scan_tracks);
  screen_set_row(screen, 0, "Scanning...");
  screen_set_row(screen, 1, count);
}

// The firmware's build date as "2026-10-02". The compiler gives it as
// "Oct  2 2026"; anything else is shown as it comes.
static void build_date(char *out, size_t out_size) {
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *date = esp_app_get_description()->date;
  if (strlen(date) == 11) {
    for (int month = 0; month < 12; month++) {
      if (strncmp(date, MONTHS + 3 * month, 3) == 0) {
        snprintf(out, out_size, "%.4s-%02d-%c%c", date + 7, month + 1,
                 date[4] == ' ' ? '0' : date[4], date[5]);
        return;
      }
    }
  }
  snprintf(out, out_size, "%s", date);
}

static void render_about(const ui_t *ui, screen_t *screen) {
  char text[2 * SCREEN_COLS];
  snprintf(text, sizeof(text), "%" PRIu32 " tracks", ui->about_tracks);
  screen_set_row(screen, 0, text);
  char date[SCREEN_COLS];
  build_date(date, sizeof(date));
  snprintf(text, sizeof(text), "Built %s", date);
  screen_set_row(screen, 1, text);
}

// Shuffle and loop, each shown only while it is on.
static void render_marks(const play_queue_t *queue, screen_t *screen) {
  if (queue->shuffle) {
    screen->cells[1][SHUFFLE_COL] = 'S';
  }
  if (queue->loop == PLAY_LOOP_ALL) {
    screen->cells[1][LOOP_COL] = UI_GLYPH_LOOP_ALL;
  } else if (queue->loop == PLAY_LOOP_ONE) {
    screen->cells[1][LOOP_COL] = UI_GLYPH_LOOP_ONE;
  }
}

static void render_now_playing(const ui_t *ui, const player_status_t *player,
                               int64_t now_us, screen_t *screen) {
  const ui_play_t *play = &ui->play;
  if (play->path[0] == '\0') {
    screen_set_row(screen, 0, "Nothing playing");
    screen_set_row(screen, 1, "");
    render_marks(&ui->queue, screen);
    return;
  }
  marquee_window(play->title, SCREEN_COLS,
                 ms_since(play->title_since_us, now_us), screen->cells[0]);
  if (play->notice_until_us != 0) {
    screen_set_row(screen, 1, play->notice == UI_NOTICE_FILE_MISSING
                                  ? "File missing"
                                  : "Can't play");
    return;
  }
  // The symbol, a space, the time; then the marks. Minutes are not turned
  // into hours: 75 minutes is "75:00".
  char text[2 * SCREEN_COLS];
  uint32_t elapsed = play->elapsed_ms / 1000;
  uint32_t total = play->duration_ms / 1000;
  snprintf(text, sizeof(text), "  %" PRIu32 ":%02" PRIu32 "/%" PRIu32
           ":%02" PRIu32, elapsed / 60, elapsed % 60, total / 60, total % 60);
  screen_set_row(screen, 1, text);
  render_marks(&ui->queue, screen);
  // The play symbol while the track is the player's and not paused; the
  // pause symbol when it is paused, and when it is stopped and remembered
  // (D plays it from the time shown). "The player's" is what the events
  // have said (play->active), not the player's state: that changes a moment
  // after a `play` or a `stop` with no event to redraw on. Its pause flag
  // is set as the pause is asked for, so that much of it can be used.
  bool playing = play->active && player->state != PLAYER_STATE_PAUSED;
  screen->cells[1][0] = playing ? UI_GLYPH_PLAY : UI_GLYPH_PAUSE;
}

// The words of an overlay that is not the volume bar.
static const char *overlay_text(ui_overlay_t overlay) {
  switch (overlay) {
  case UI_OVERLAY_SHUFFLE_ON:
    return "Shuffle on";
  case UI_OVERLAY_SHUFFLE_OFF:
    return "Shuffle off";
  case UI_OVERLAY_LOOP_OFF:
    return "Loop off";
  case UI_OVERLAY_LOOP_ALL:
    return "Loop all";
  case UI_OVERLAY_LOOP_ONE:
    return "Loop one";
  case UI_OVERLAY_ADDED:
    return "Added";
  case UI_OVERLAY_QUEUE_FULL:
    return "Queue full";
  case UI_OVERLAY_VOLUME:
    break;
  }
  return "";
}

static void render_volume_bar(int volume, screen_t *screen) {
  char *row = screen->cells[1];
  memcpy(row, "Vol", BAR_COL);
  for (int cell = 0; cell < BAR_CELLS; cell++) {
    int level = volume - 2 * cell; // what this cell and those after it show
    row[BAR_COL + cell] = level >= 2   ? UI_GLYPH_BAR_FULL
                          : level == 1 ? UI_GLYPH_BAR_HALF
                                       : UI_GLYPH_BAR_EMPTY;
  }
  row[BAR_COL + BAR_CELLS] = ' ';
  row[SCREEN_COLS - 2] = volume >= 10 ? (char)('0' + volume / 10) : ' ';
  row[SCREEN_COLS - 1] = (char)('0' + volume % 10);
}

void ui_render(const ui_t *ui, const player_status_t *player, int64_t now_us,
               screen_t *screen) {
  switch (ui->mode) {
  case UI_MODE_KEYTEST:
    // The whole screen is the test's: no overlay here.
    keytest_render(screen, ui->keytest_any ? &ui->keytest_last : NULL,
                   ui->keytest_count);
    return;
  case UI_MODE_ABOUT:
    render_about(ui, screen);
    break;
  case UI_MODE_NOW_PLAYING:
    render_now_playing(ui, player, now_us, screen);
    break;
  case UI_MODE_MENU:
    if (ui->no_card) {
      screen_set_row(screen, 0, "Insert SD card");
      screen_set_row(screen, 1, "");
    } else if (ui->scanning) {
      render_scanning(ui, screen);
    } else if (ui->options_open) {
      render_options(ui, screen);
    } else {
      render_list(ui, now_us, screen);
    }
    break;
  }
  if (overlay_on(ui, now_us)) {
    if (ui->overlay == UI_OVERLAY_VOLUME) {
      render_volume_bar(player->volume, screen);
    } else {
      screen_set_row(screen, 1, overlay_text(ui->overlay));
    }
  }
}

uint32_t ui_next_change_ms(const ui_t *ui, int64_t now_us) {
  uint32_t next = UINT32_MAX;
  uint32_t ms;
  bool overlay = overlay_on(ui, now_us);
  if (overlay) {
    next = ms_until(ui->overlay_until_us, now_us);
  }
  if (ui->play.notice_until_us != 0) {
    // Whatever screen is up: when it runs out the next track starts.
    ms = ms_until(ui->play.notice_until_us, now_us);
    next = ms < next ? ms : next;
  }
  if (ui->mode == UI_MODE_NOW_PLAYING) {
    if (ui->play.path[0] != '\0') {
      ms = marquee_next_change_ms(ui->play.title, SCREEN_COLS,
                                  ms_since(ui->play.title_since_us, now_us));
      next = ms < next ? ms : next;
    }
  } else if (ui_list_on_show(ui) && !ui->options_open && !overlay &&
             ui->list_menu.count != 0) {
    // Under the overlay the item is not seen, so its steps are not worth
    // waking for; the overlay's end is, and the marquee is then drawn where
    // it has got to. The options menu's rows do not move.
    ms = marquee_next_change_ms(ui->list_item, ITEM_WIDTH,
                                ms_since(ui->item_since_us, now_us));
    next = ms < next ? ms : next;
  }
  return next;
}
