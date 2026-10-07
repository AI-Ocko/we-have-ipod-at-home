#pragma once

#include "library.h"
#include "menu.h"
#include "playlist.h"
#include "playlist_catalog.h"
#include "standby.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The menu tree. It is a stack of levels, each a list with its own cursor,
// so that going back returns to where the user was. It holds numbers, never
// text or pointers into the index: every call that needs the index is given
// it, and may be given NULL when there is none. The two pointers it keeps
// are to what the playlist screens show (browse_playlists_t) and to what the
// Settings rows show (browse_settings_t); both belong to the caller.

#define BROWSE_DEPTH_MAX 5

typedef enum {
  BROWSE_MAIN,         // Now Playing, Artists, Albums, Songs, Playlists,
                       // Settings
  BROWSE_ARTISTS,      // every artist, A to Z
  BROWSE_ARTIST,       // id = artist: "All songs", then the artist's albums
  BROWSE_ARTIST_SONGS, // id = artist: all the artist's tracks
  BROWSE_ALBUMS,       // every album, A to Z
  BROWSE_ALBUM,        // id = album: its tracks
  BROWSE_SONGS,        // every song, A to Z
  BROWSE_SETTINGS,     // Rescan library, Backlight, Sleep timer, Keypad
                       // test, About
  BROWSE_PLAYLISTS,    // "New playlist", then every playlist A to Z
  BROWSE_PLAYLIST,     // id = the playlist's number in the catalog: its
                       // entries
} browse_screen_t;

typedef struct {
  browse_screen_t screen;
  uint32_t id;
  menu_t menu;
} browse_level_t;

// What the playlist screens show. It belongs to the caller, who keeps it up
// to date and tells browse with browse_refresh; browse only reads it.
typedef struct {
  const playlist_catalog_t *catalog; // NULL: no listing yet
  const uint16_t *tracks; // the open playlist's entries, PLAYLIST_MISSING
                          // for a missing one; NULL while it loads
  uint32_t count;
  uint32_t playlist; // which catalog entry `tracks` belongs to
} browse_playlists_t;

// What the Settings rows show. It belongs to the caller, who keeps it up to
// date; browse only reads it.
typedef struct {
  standby_backlight_t backlight;
  uint32_t sleep_minutes; // 0: off
} browse_settings_t;

typedef struct {
  browse_level_t levels[BROWSE_DEPTH_MAX];
  int depth;                           // index of the current level
  const browse_playlists_t *playlists; // NULL until browse_set_playlists
  const browse_settings_t *settings;   // NULL until browse_set_settings
} browse_t;

typedef enum {
  BROWSE_NOTHING,     // nothing to do (an empty list, an item with no effect)
  BROWSE_ENTERED,     // a new level is now current
  BROWSE_PLAY,        // play `list` starting at position `index`
  BROWSE_NOW_PLAYING, // show the Now Playing screen
  BROWSE_RESCAN,
  BROWSE_BACKLIGHT, // move the backlight setting on
  BROWSE_SLEEP,     // move the sleep timer on
  BROWSE_KEYTEST,
  BROWSE_ABOUT,
  BROWSE_NEW_PLAYLIST,  // "New playlist" was chosen
  BROWSE_OPEN_PLAYLIST, // a playlist's level is now current; `index` is its
                        // number in the catalog: load its entries
  BROWSE_PLAY_ENTRIES,  // play the open playlist from entry `index`
  BROWSE_MISSING,       // the entry chosen is missing: nothing to play
} browse_action_t;

typedef struct {
  browse_action_t action;
  tracklist_t list;
  uint32_t index;
} browse_result_t;

typedef enum {
  BROWSE_TARGET_NONE,        // nothing the options menu applies to
  BROWSE_TARGET_TRACK,       // `list` and `index` say which
  BROWSE_TARGET_ALBUM,       // `list` is the album
  BROWSE_TARGET_ARTIST,      // `list` is all the artist's tracks
  BROWSE_TARGET_PLAYLIST,    // `index` is the playlist's number in the catalog
  BROWSE_TARGET_ENTRY,       // `index` is the entry in the open playlist
  BROWSE_TARGET_NOW_PLAYING, // for options_for only; browse_target never
                             // gives it
} browse_target_kind_t;

typedef struct {
  browse_target_kind_t kind;
  tracklist_t list;
  uint32_t index; // for a track: its position in `list`
} browse_target_t;

// Start at the main menu, with no playlists attached.
void browse_init(browse_t *b);

// Attach what the playlist screens show (NULL: nothing). Only the pointer is
// kept; call browse_refresh after it changes.
void browse_set_playlists(browse_t *b, const browse_playlists_t *p);

// Attach what the Settings rows show (NULL: the rows show their names
// alone, "Backlight" and "Sleep timer"). Only the pointer is kept; it is
// read each time a row's text is asked for, so nothing needs refreshing.
// The Settings screen is: Rescan library, Backlight, Sleep timer, Keypad
// test, About; its rows show values (spec section 9): "Backlight: 30 s"
// ("15 s", "60 s", "on"), "Sleep: off", "Sleep: 27 min".
void browse_set_settings(browse_t *b, const browse_settings_t *s);

// Bring the current level's item count up to date with `lib`, and the
// counts of any playlist levels on the stack with the attached playlists.
// Call after anything that could have changed them. With a NULL library
// every library list is empty.
void browse_refresh(browse_t *b, const library_t *lib);

// The index has been replaced: numbers on the stack no longer mean what they
// did. Go back to the main menu (its cursor is kept).
void browse_library_changed(browse_t *b);

// The current level's menu, to move its cursor.
menu_t *browse_menu(browse_t *b);

// The current level's name for the top row: "Main", "Artists", an artist's
// or an album's name, "Albums", "Songs", "Settings", "Playlists", a
// playlist's name. Always terminated.
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
