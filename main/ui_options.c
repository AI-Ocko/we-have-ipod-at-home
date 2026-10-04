#include "library_task.h"
#include "options.h"
#include "ui_state.h"
#include <stdbool.h>

// The options menu: what `#` offers for the item under the cursor of a list.
// Part of the UI task: called from its loop (ui.c).
//
// It opens over the list and is not a screen of its own (ui.c, THE SCREENS):
// the list underneath keeps its place, and closing the menu shows it again.
// While it is open the top row is the item's name, cut to fit, and the
// bottom row one of its actions (options.h says which an item has):
//
//   |Kid A           |
//   |>Add to queue   |
//
// `#` opens it only on a row that is a track, an album or an artist
// (browse_target); on anything else the key does nothing. What it is about
// is kept as numbers (options_target), which belong to the index that was
// current when it opened: a scan, or a new index, closes the menu (ui.c).
//
// The actions:
//
//   Play          the queue becomes the track's list, starting at the track
//                 (what selecting it does), or the album's or artist's
//                 tracks from the first; it starts, and Now Playing comes up
//   Shuffle play  the same for an album or an artist, with shuffle turned
//                 on and a track picked at random to come first
//   Play next     the track goes into the queue after the current one
//   Add to queue  the track, or all the album's or artist's tracks, go to
//                 the end of the queue: all of them or, if they do not fit,
//                 none
//
// The last two change nothing that is playing and leave the menu open:
// "Added" or "Queue full" is shown on the bottom row for 1.5 s (the
// overlay). Each action takes the index once.

void ui_options_open(ui_t *ui) {
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    return; // the lists are empty: there is nothing under the cursor
  }
  browse_target_t target = browse_target(&ui->browse, lib);
  uint32_t count = options_for(target.kind, ui->options);
  if (count != 0) {
    // The name is read from the index, not from the copy the screen has:
    // the cursor may have moved since that was made.
    browse_item(&ui->browse, lib, browse_menu(&ui->browse)->cursor,
                ui->options_title, sizeof(ui->options_title));
    ui->options_target = target;
    menu_reset(&ui->options_menu, count);
    ui->options_open = true;
  }
  library_task_put();
}

void ui_options_close(ui_t *ui) {
  if (ui->options_open) {
    ui->options_open = false;
    ui->list_restart = true; // the item under the cursor is read afresh
    ui->list_dirty = true;
  }
}

// 5 or 6: do the action under the cursor.
static void choose(ui_t *ui, int64_t now_us) {
  const browse_target_t *target = &ui->options_target;
  option_t option = ui->options[ui->options_menu.cursor];
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    ui_options_close(ui); // no index: the target's numbers mean nothing
    return;
  }
  bool play = false;
  switch (option) {
  case OPTION_PLAY:
  case OPTION_SHUFFLE_PLAY:
    play = ui_play_load(
        ui, lib, target->list,
        target->kind == BROWSE_TARGET_TRACK ? target->index : 0,
        option == OPTION_SHUFFLE_PLAY, now_us);
    break;
  case OPTION_PLAY_NEXT:
  case OPTION_ADD_TO_QUEUE:
    ui_overlay_show(ui,
                    ui_play_add(ui, lib, target, option == OPTION_PLAY_NEXT,
                                now_us)
                        ? UI_OVERLAY_ADDED
                        : UI_OVERLAY_QUEUE_FULL,
                    now_us);
    break;
  case OPTION_ADD_TO_PLAYLIST:
  case OPTION_RENAME:
  case OPTION_DELETE:
  case OPTION_MOVE_UP:
  case OPTION_MOVE_DOWN:
  case OPTION_REMOVE:
  case OPTION_SAVE_QUEUE:
    // The playlist actions do nothing yet: stage 7 Task 5 wires them. Until
    // then "Add to playlist" is offered on tracks, albums and artists and
    // choosing it leaves the menu open with nothing changed.
    break;
  case OPTION_COUNT:
    break; // not an option
  }
  library_task_put();
  if (play) {
    ui_play_start(ui);
    ui_options_close(ui);
    ui_show_now_playing(ui, now_us);
  }
}

void ui_options_on_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  bool press = (event->type == KEY_PRESS);
  bool step = press || event->type == KEY_REPEAT;
  switch (event->key) {
  case '2':
  case '8':
    if (step) {
      // As in a list: a press goes round at the ends, a held key stops.
      menu_move(&ui->options_menu, event->key == '8' ? 1 : -1, press);
    }
    break;
  case '5':
  case '6':
    if (press) {
      choose(ui, now_us);
    }
    break;
  case '4':
  case '*':
  case '#':
    if (press) {
      ui_options_close(ui);
    }
    break;
  default:
    break;
  }
}
