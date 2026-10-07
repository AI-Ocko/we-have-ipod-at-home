#include "ui_state.h"
#include <string.h>
#include <strings.h>

// The text entry: typing a playlist's name by multi-tap (textentry.h). Part
// of the UI task: called from its loop (ui.c). It is a layer (ui.c, LAYERS)
// over the screen it was opened from, the menus or Now Playing, and closing
// it shows that screen again as it was.
//
//   |Name:           |
//   |ROAD TRIP_      |   textentry_row: the text, then the pending
//                         character or the `_` cursor
//
// It is opened for a new playlist ("New playlist", "New..." in the picker
// with the picker's tracks, "Save queue" with the queue's) or for a rename
// (starting with the playlist's name). Opening it asks for a listing, so
// that "Name in use" is checked against the folder as it is now; until the
// listing comes the copy in hand is used. The playlist task checks again
// when it creates or renames (a catalog holds at most 200 playlists), and
// its "Name in use" is shown too, when its answer comes.
//
// Keys: only presses type; long presses and repeats do nothing, so a held
// key does not cycle. 0-9 type, `*` drops the pending character or deletes
// the last one, `#` checks the name, C cancels. A, B and D keep their global
// meaning (ui.c handles them first). A name that breaks a rule says why on
// the bottom row for 1.5 s and typing goes on; any typing key takes the
// message away. A name that passes is sent, and the layer closes.
//
// The commit after a second is not a timer: textentry_deadline is one of
// the times the UI loop sleeps until (ui_next_change_ms), and ui_text_on_time
// commits when it comes.

static int64_t ms_of(int64_t now_us) { return now_us / 1000; }

void ui_text_open(ui_t *ui, ui_text_purpose_t purpose, const char *initial,
                  int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  textentry_init(&pl->text, initial);
  pl->text_purpose = purpose;
  ui->layer = UI_LAYER_TEXT;
  ui_pl_list(ui, now_us); // for "Name in use"
}

void ui_text_cancel(ui_t *ui) {
  if (ui->layer == UI_LAYER_TEXT) {
    ui->layer = UI_LAYER_NONE;
    ui->list_dirty = true;
    ui->list_restart = true;
  }
}

// Whether another playlist in the catalog in hand has `name`. A rename to
// the playlist's own name (in any case) is not "in use": it is done, and
// changes nothing.
static bool in_use(const ui_t *ui, const char *name) {
  const ui_pl_t *pl = ui->pl;
  uint32_t i;
  if (!pl->catalog_ready || !playlist_catalog_find(&pl->catalog, name, &i)) {
    return false;
  }
  return !(pl->text_purpose == UI_TEXT_RENAME &&
           strcasecmp(name, pl->target_name) == 0);
}

// `#`: check the name and send it.
static void finish(ui_t *ui, int64_t now_us) {
  char typed[TEXTENTRY_MAX + 1];
  char name[PLAYLIST_NAME_MAX + 1];
  textentry_text(&ui->pl->text, typed, sizeof(typed));
  playlist_name_t why = playlist_name_check(typed, name);
  if (why != PLAYLIST_NAME_OK) {
    ui_overlay_show(ui, ui_name_overlay(why), now_us);
    return;
  }
  if (in_use(ui, name)) {
    ui_overlay_show(ui, UI_OVERLAY_NAME_IN_USE, now_us);
    return;
  }
  if (ui_pl_send_name(ui, name, now_us)) {
    ui_text_cancel(ui); // sent: back to where it was opened
  }
  // Refused ("Busy"): the text stays, and `#` can try again.
}

void ui_text_on_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  if (event->type != KEY_PRESS) {
    return;
  }
  char key = event->key;
  if (key == 'C') {
    ui_text_cancel(ui);
    return;
  }
  bool digit = key >= '0' && key <= '9';
  if (!digit && key != '*' && key != '#') {
    return;
  }
  textentry_t *t = &ui->pl->text;
  int64_t now_ms = ms_of(now_us);
  textentry_tick(t, now_ms); // a second may have passed since the last key
  if (key != '#') {
    ui->overlay_until_us = 0; // typing again: the message has been read
  }
  if (textentry_key(t, key, now_ms) == TEXTENTRY_DONE) {
    finish(ui, now_us);
  }
}

void ui_text_on_time(ui_t *ui, int64_t now_us) {
  if (ui->layer == UI_LAYER_TEXT) {
    textentry_tick(&ui->pl->text, ms_of(now_us));
  }
}
