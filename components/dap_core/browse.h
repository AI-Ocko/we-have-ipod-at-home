#pragma once

#include "library.h"
#include "menu.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The menu tree. It is a stack of levels, each a list with its own cursor,
// so that going back returns to where the user was. It holds numbers, never
// text or pointers: every call that needs the index is given it, and may be
// given NULL when there is none.

#define BROWSE_DEPTH_MAX 5

typedef enum {
  BROWSE_MAIN,         // Now Playing, Artists, Albums, Songs, Settings
  BROWSE_ARTISTS,      // every artist, A to Z
  BROWSE_ARTIST,       // id = artist: "All songs", then the artist's albums
  BROWSE_ARTIST_SONGS, // id = artist: all the artist's tracks
  BROWSE_ALBUMS,       // every album, A to Z
  BROWSE_ALBUM,        // id = album: its tracks
  BROWSE_SONGS,        // every song, A to Z
  BROWSE_SETTINGS,     // Rescan library, Keypad test, About
} browse_screen_t;

typedef struct {
  browse_screen_t screen;
  uint32_t id;
  menu_t menu;
} browse_level_t;

typedef struct {
  browse_level_t levels[BROWSE_DEPTH_MAX];
  int depth; // index of the current level
} browse_t;

typedef enum {
  BROWSE_NOTHING,     // nothing to do (an empty list, an item with no effect)
  BROWSE_ENTERED,     // a new level is now current
  BROWSE_PLAY,        // play `list` starting at position `index`
  BROWSE_NOW_PLAYING, // show the Now Playing screen
  BROWSE_RESCAN,
  BROWSE_KEYTEST,
  BROWSE_ABOUT,
} browse_action_t;

typedef struct {
  browse_action_t action;
  tracklist_t list;
  uint32_t index;
} browse_result_t;

typedef enum {
  BROWSE_TARGET_NONE,   // nothing the options menu applies to
  BROWSE_TARGET_TRACK,  // `list` and `index` say which
  BROWSE_TARGET_ALBUM,  // `list` is the album
  BROWSE_TARGET_ARTIST, // `list` is all the artist's tracks
} browse_target_kind_t;

typedef struct {
  browse_target_kind_t kind;
  tracklist_t list;
  uint32_t index; // for a track: its position in `list`
} browse_target_t;

// Start at the main menu.
void browse_init(browse_t *b);

// Bring the current level's item count up to date with `lib`. Call after
// anything that could have changed it. With a NULL library every library
// list is empty.
void browse_refresh(browse_t *b, const library_t *lib);

// The index has been replaced: numbers on the stack no longer mean what they
// did. Go back to the main menu (its cursor is kept).
void browse_library_changed(browse_t *b);

// The current level's menu, to move its cursor.
menu_t *browse_menu(browse_t *b);

// The current level's name for the top row: "Main", "Artists", an artist's
// or an album's name, "Albums", "Songs", "Settings". Always terminated.
void browse_name(const browse_t *b, const library_t *lib, char *out,
                 size_t out_size);

// The text of item `index` of the current level. Empty if there is none.
void browse_item(const browse_t *b, const library_t *lib, uint32_t index,
                 char *out, size_t out_size);

// Whether the current level is sorted A to Z, so letter jumps make sense.
bool browse_is_alphabetical(const browse_t *b);

// The letter group of item `index` of the current level (see menu_group).
char browse_group(const browse_t *b, const library_t *lib, uint32_t index);

// Open or act on the item under the cursor.
browse_result_t browse_select(browse_t *b, const library_t *lib);

// Go back one level. False at the main menu.
bool browse_back(browse_t *b);

// What is under the cursor, for the options menu. Does not change the level.
browse_target_t browse_target(const browse_t *b, const library_t *lib);
