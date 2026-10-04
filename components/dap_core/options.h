#pragma once

#include "browse.h"
#include <stdint.h>

// What the # key offers for the thing under the cursor.

typedef enum {
  OPTION_PLAY,
  OPTION_PLAY_NEXT,
  OPTION_ADD_TO_QUEUE,
  OPTION_SHUFFLE_PLAY,
  OPTION_ADD_TO_PLAYLIST,
  OPTION_RENAME,
  OPTION_DELETE,
  OPTION_MOVE_UP,
  OPTION_MOVE_DOWN,
  OPTION_REMOVE,
  OPTION_SAVE_QUEUE,
  OPTION_COUNT, // not an option: how many there are
} option_t;

#define OPTIONS_MAX 4

// The actions offered for a target, in menu order. Returns how many (0 for
// BROWSE_TARGET_NONE).
uint32_t options_for(browse_target_kind_t kind, option_t out[OPTIONS_MAX]);

// The menu text of an action.
const char *options_text(option_t option);
