#include "options.h"

#include <stddef.h>
#include <string.h>

// The menus of the spec, section 9 ("Options menus"), in menu order.
typedef struct {
  uint32_t count;
  option_t options[OPTIONS_MAX];
} menu_of_t;

static const menu_of_t TRACK = {4,
                                {OPTION_PLAY, OPTION_PLAY_NEXT,
                                 OPTION_ADD_TO_QUEUE, OPTION_ADD_TO_PLAYLIST}};
static const menu_of_t LIST = {4,
                               {OPTION_PLAY, OPTION_SHUFFLE_PLAY,
                                OPTION_ADD_TO_QUEUE, OPTION_ADD_TO_PLAYLIST}};
static const menu_of_t PLAYLIST = {
    4, {OPTION_PLAY, OPTION_SHUFFLE_PLAY, OPTION_RENAME, OPTION_DELETE}};
static const menu_of_t ENTRY = {
    4, {OPTION_PLAY, OPTION_MOVE_UP, OPTION_MOVE_DOWN, OPTION_REMOVE}};
static const menu_of_t NOW_PLAYING = {
    2, {OPTION_ADD_TO_PLAYLIST, OPTION_SAVE_QUEUE}};

uint32_t options_for(browse_target_kind_t kind, option_t out[OPTIONS_MAX]) {
  if (out == NULL) {
    return 0;
  }
  const menu_of_t *menu = NULL;
  switch (kind) {
  case BROWSE_TARGET_TRACK:
    menu = &TRACK;
    break;
  case BROWSE_TARGET_ALBUM:
  case BROWSE_TARGET_ARTIST:
    menu = &LIST;
    break;
  case BROWSE_TARGET_PLAYLIST:
    menu = &PLAYLIST;
    break;
  case BROWSE_TARGET_ENTRY:
    menu = &ENTRY;
    break;
  case BROWSE_TARGET_NOW_PLAYING:
    menu = &NOW_PLAYING;
    break;
  case BROWSE_TARGET_NONE:
  default:
    return 0;
  }
  memcpy(out, menu->options, menu->count * sizeof(option_t));
  return menu->count;
}

const char *options_text(option_t option) {
  switch (option) {
  case OPTION_PLAY:
    return "Play";
  case OPTION_PLAY_NEXT:
    return "Play next";
  case OPTION_ADD_TO_QUEUE:
    return "Add to queue";
  case OPTION_SHUFFLE_PLAY:
    return "Shuffle play";
  case OPTION_ADD_TO_PLAYLIST:
    return "Add to playlist";
  case OPTION_RENAME:
    return "Rename";
  case OPTION_DELETE:
    return "Delete";
  case OPTION_MOVE_UP:
    return "Move up";
  case OPTION_MOVE_DOWN:
    return "Move down";
  case OPTION_REMOVE:
    return "Remove";
  case OPTION_SAVE_QUEUE:
    return "Save queue";
  case OPTION_COUNT:
    break;
  }
  return "";
}
