#include "options.h"

#include <stddef.h>

// One action differs between the two menus: a track can be played next, a
// list of tracks can be played shuffled. So the menus are written out here
// rather than kept as tables.
uint32_t options_for(browse_target_kind_t kind, option_t out[OPTIONS_MAX]) {
  if (out == NULL) {
    return 0;
  }
  switch (kind) {
  case BROWSE_TARGET_TRACK:
    out[1] = OPTION_PLAY_NEXT;
    break;
  case BROWSE_TARGET_ALBUM:
  case BROWSE_TARGET_ARTIST:
    out[1] = OPTION_SHUFFLE_PLAY;
    break;
  default:
    return 0;
  }
  out[0] = OPTION_PLAY;
  out[2] = OPTION_ADD_TO_QUEUE;
  return 3;
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
  }
  return "";
}
