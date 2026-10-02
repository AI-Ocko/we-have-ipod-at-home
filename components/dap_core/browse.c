#include "browse.h"

#include <string.h>

// Design notes
//
// A level is a kind of list, the number of the thing it lists (an artist or
// an album) and a cursor. What its items are is worked out from the index
// each time it is asked, with a fixed number of index queries per item
// whatever the size of the library; the queries are listed at item_text and
// browse_select. Nothing here walks tracks, albums or artists.
//
// The stack keeps no text and no pointers, because the index can be
// withdrawn between any two calls. Text is copied into the caller's buffer
// before a call returns. Inside a call a pointer into the index is used and
// dropped.
//
// A STALE STACK
//
// The numbers on the stack belong to one index. The caller says so when the
// index is replaced (browse_library_changed), but between that happening
// and the caller noticing, or with no index at all, calls arrive with a
// stack that does not match `lib`. They are safe because nothing trusts the
// stack's counts to reach into the index: every item is fetched by a query
// that answers "none" for a number the index does not have, and
// browse_select acts only on an item the index has now.
//
// Only browse_refresh reads how long a list is. A level that is opened is
// counted by the same code.
//
// AN ARTIST'S LEVEL AND LETTER JUMPS
//
// Row 0 is "All songs" and rows 1.. are the albums A to Z. The level is
// reported as alphabetical, and row 0 is given the group '#', which is
// before every letter: so the groups never go back down the level and the
// jump's search is right. ("All songs" under 'A' would put an 'A' before an
// album called "1999".) An album that starts with a digit shares '#' with
// row 0; jumping back from it lands on row 0, the top of the list, which is
// where that key should go.
//
// Levels that are not A to Z (the fixed menus, an album's tracks, an
// artist's songs) give every item '#': one group, so a jump made there
// anyway finds no next group and goes at most to the top.

static const char *const MAIN_ITEMS[] = {"Now Playing", "Artists", "Albums",
                                         "Songs", "Settings"};
enum {
  MAIN_NOW_PLAYING,
  MAIN_ARTISTS,
  MAIN_ALBUMS,
  MAIN_SONGS,
  MAIN_SETTINGS,
  MAIN_COUNT
};

static const char *const SETTINGS_ITEMS[] = {"Rescan library", "Keypad test",
                                             "About"};
enum { SETTINGS_RESCAN, SETTINGS_KEYTEST, SETTINGS_ABOUT, SETTINGS_COUNT };

static const char ALL_SONGS[] = "All songs";

// The current level's place in the stack, whatever `depth` holds.
static int depth_of(const browse_t *b) {
  if (b->depth < 0) {
    return 0;
  }
  return b->depth < BROWSE_DEPTH_MAX ? b->depth : BROWSE_DEPTH_MAX - 1;
}

static const browse_level_t *level_of(const browse_t *b) {
  return &b->levels[depth_of(b)];
}

// The same for a stack that may be changed; puts `depth` right first.
static browse_level_t *current(browse_t *b) {
  b->depth = depth_of(b);
  return &b->levels[b->depth];
}

// The tracks a level lists, if it is a level of tracks.
static tracklist_t tracks_of(const browse_level_t *level) {
  tracklist_t list = {.kind = TRACKLIST_NONE, .id = 0};
  switch (level->screen) {
  case BROWSE_ALBUM:
    list.kind = TRACKLIST_ALBUM;
    list.id = level->id;
    break;
  case BROWSE_ARTIST_SONGS:
    list.kind = TRACKLIST_ARTIST;
    list.id = level->id;
    break;
  case BROWSE_SONGS:
    list.kind = TRACKLIST_SONGS;
    break;
  default:
    break;
  }
  return list;
}

// How many items the level has in this index: one query each.
static uint32_t count_of(const browse_level_t *level, const library_t *lib) {
  switch (level->screen) {
  case BROWSE_MAIN:
    return MAIN_COUNT;
  case BROWSE_SETTINGS:
    return SETTINGS_COUNT;
  case BROWSE_ARTISTS:
    return library_artist_count(lib);
  case BROWSE_ALBUMS:
    return library_album_count(lib);
  case BROWSE_ARTIST: {
    // Every artist in the index has an album, so no albums means no such
    // artist, and then there is no "All songs" row either.
    uint32_t first, albums;
    library_artist_albums(lib, level->id, &first, &albums);
    return albums != 0 ? albums + 1 : 0;
  }
  case BROWSE_ARTIST_SONGS:
  case BROWSE_ALBUM:
  case BROWSE_SONGS:
    return tracklist_count(lib, tracks_of(level));
  }
  return 0;
}

// The text of item `index`: a fixed string or one inside the index, "" if
// there is no such item. Not to be kept past the call that asked.
//
// What it asks the index:
//   Main, Settings  nothing
//   Artists         library_artist_name
//   an artist       library_artist_albums; then, for an album row,
//                   library_album_name
//   Albums          library_album_by_name, library_album_name
//   an album        library_album_tracks, library_track_title
//   artist's songs  library_artist_tracks, library_track_title
//   Songs           library_song_by_title, library_track_title
static const char *item_text(const browse_level_t *level, const library_t *lib,
                             uint32_t index) {
  switch (level->screen) {
  case BROWSE_MAIN:
    return index < MAIN_COUNT ? MAIN_ITEMS[index] : "";
  case BROWSE_SETTINGS:
    return index < SETTINGS_COUNT ? SETTINGS_ITEMS[index] : "";
  case BROWSE_ARTISTS:
    return library_artist_name(lib, index);
  case BROWSE_ALBUMS:
    // An `index` past the end gives no album, and no album gives "".
    return library_album_name(lib, library_album_by_name(lib, index));
  case BROWSE_ARTIST: {
    uint32_t first, albums;
    library_artist_albums(lib, level->id, &first, &albums);
    if (index == 0) {
      return albums != 0 ? ALL_SONGS : "";
    }
    return index - 1 < albums ? library_album_name(lib, first + (index - 1))
                              : "";
  }
  case BROWSE_ARTIST_SONGS:
  case BROWSE_ALBUM:
  case BROWSE_SONGS:
    // Only the title is wanted, so only the track's own row is read; no
    // track at that position gives "".
    return library_track_title(lib,
                               tracklist_track(lib, tracks_of(level), index));
  }
  return "";
}

// Copy as much of `text` as fits, always terminated. A buffer of no size is
// left alone.
static void copy_out(const char *text, char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  size_t i = 0;
  for (; i + 1 < out_size && text[i] != '\0'; i++) {
    out[i] = text[i];
  }
  out[i] = '\0';
}

static void set_main(browse_level_t *level) {
  level->screen = BROWSE_MAIN;
  level->id = 0;
}

void browse_init(browse_t *b) {
  if (b == NULL) {
    return;
  }
  memset(b, 0, sizeof(*b));
  set_main(&b->levels[0]);
  menu_reset(&b->levels[0].menu, MAIN_COUNT);
}

void browse_refresh(browse_t *b, const library_t *lib) {
  if (b == NULL) {
    return;
  }
  browse_level_t *level = current(b);
  menu_set_count(&level->menu, count_of(level, lib));
}

void browse_library_changed(browse_t *b) {
  if (b == NULL) {
    return;
  }
  // The main menu is the one level that means the same in any index, and
  // its length is fixed, so no index is needed to be there.
  b->depth = 0;
  set_main(&b->levels[0]);
  menu_set_count(&b->levels[0].menu, MAIN_COUNT);
}

menu_t *browse_menu(browse_t *b) {
  return b != NULL ? &current(b)->menu : NULL;
}

void browse_name(const browse_t *b, const library_t *lib, char *out,
                 size_t out_size) {
  const char *name = "";
  if (b != NULL) {
    const browse_level_t *level = level_of(b);
    switch (level->screen) {
    case BROWSE_MAIN:
      name = "Main";
      break;
    case BROWSE_ARTISTS:
      name = "Artists";
      break;
    case BROWSE_ALBUMS:
      name = "Albums";
      break;
    case BROWSE_SONGS:
      name = "Songs";
      break;
    case BROWSE_SETTINGS:
      name = "Settings";
      break;
    case BROWSE_ARTIST:
    case BROWSE_ARTIST_SONGS:
      // Both of an artist's lists are under the artist's name; the position
      // beside it and the rows below say which one it is.
      name = library_artist_name(lib, level->id);
      break;
    case BROWSE_ALBUM:
      name = library_album_name(lib, level->id);
      break;
    }
  }
  copy_out(name, out, out_size);
}

void browse_item(const browse_t *b, const library_t *lib, uint32_t index,
                 char *out, size_t out_size) {
  copy_out(b != NULL ? item_text(level_of(b), lib, index) : "", out, out_size);
}

bool browse_is_alphabetical(const browse_t *b) {
  if (b == NULL) {
    return false;
  }
  switch (level_of(b)->screen) {
  case BROWSE_ARTISTS:
  case BROWSE_ARTIST: // see AN ARTIST'S LEVEL AND LETTER JUMPS
  case BROWSE_ALBUMS:
  case BROWSE_SONGS:
    return true;
  default:
    return false;
  }
}

char browse_group(const browse_t *b, const library_t *lib, uint32_t index) {
  if (!browse_is_alphabetical(b)) {
    return '#';
  }
  const browse_level_t *level = level_of(b);
  if (level->screen == BROWSE_ARTIST && index == 0) {
    return '#';
  }
  // Only the first character is read, there and then.
  return menu_group(item_text(level, lib, index));
}

// Put a new level on the stack and count it. False if the stack is full,
// which this tree cannot make it (its longest path is four levels).
static bool open_level(browse_t *b, const library_t *lib,
                       browse_screen_t screen, uint32_t id) {
  if (b->depth + 1 >= BROWSE_DEPTH_MAX) {
    return false;
  }
  b->depth++;
  browse_level_t *level = &b->levels[b->depth];
  level->screen = screen;
  level->id = id;
  menu_reset(&level->menu, 0);
  browse_refresh(b, lib);
  return true;
}

// What it asks the index before it acts (opening a level then counts the
// new level, one query more):
//   Main, Settings  nothing
//   Artists         library_artist_albums: is there such an artist?
//   an artist       library_artist_albums: the artist, and which album
//   Albums          library_album_by_name: which album
//   a track         tracklist_track: is there a track at this position?
browse_result_t browse_select(browse_t *b, const library_t *lib) {
  browse_result_t result = {.action = BROWSE_NOTHING,
                            .list = {.kind = TRACKLIST_NONE, .id = 0},
                            .index = 0};
  if (b == NULL) {
    return result;
  }
  browse_level_t *level = current(b);
  uint32_t cursor = level->menu.cursor;
  if (cursor >= level->menu.count) {
    return result; // an empty list
  }

  bool opened = false;
  switch (level->screen) {
  case BROWSE_MAIN:
    switch (cursor) {
    case MAIN_NOW_PLAYING:
      result.action = BROWSE_NOW_PLAYING;
      break;
    case MAIN_ARTISTS:
      opened = open_level(b, lib, BROWSE_ARTISTS, 0);
      break;
    case MAIN_ALBUMS:
      opened = open_level(b, lib, BROWSE_ALBUMS, 0);
      break;
    case MAIN_SONGS:
      opened = open_level(b, lib, BROWSE_SONGS, 0);
      break;
    case MAIN_SETTINGS:
      opened = open_level(b, lib, BROWSE_SETTINGS, 0);
      break;
    default:
      break;
    }
    break;

  case BROWSE_SETTINGS:
    switch (cursor) {
    case SETTINGS_RESCAN:
      result.action = BROWSE_RESCAN;
      break;
    case SETTINGS_KEYTEST:
      result.action = BROWSE_KEYTEST;
      break;
    case SETTINGS_ABOUT:
      result.action = BROWSE_ABOUT;
      break;
    default:
      break;
    }
    break;

  case BROWSE_ARTISTS: {
    uint32_t first, albums;
    library_artist_albums(lib, cursor, &first, &albums);
    if (albums != 0) {
      opened = open_level(b, lib, BROWSE_ARTIST, cursor);
    }
    break;
  }

  case BROWSE_ARTIST: {
    uint32_t first, albums;
    library_artist_albums(lib, level->id, &first, &albums);
    if (albums == 0) {
      break; // no such artist in this index
    }
    if (cursor == 0) {
      opened = open_level(b, lib, BROWSE_ARTIST_SONGS, level->id);
    } else if (cursor - 1 < albums) {
      opened = open_level(b, lib, BROWSE_ALBUM, first + (cursor - 1));
    }
    break;
  }

  case BROWSE_ALBUMS: {
    uint32_t album = library_album_by_name(lib, cursor);
    if (album != UINT32_MAX) {
      opened = open_level(b, lib, BROWSE_ALBUM, album);
    }
    break;
  }

  case BROWSE_ARTIST_SONGS:
  case BROWSE_ALBUM:
  case BROWSE_SONGS: {
    // The list to play is the list on the screen, from the row selected.
    tracklist_t list = tracks_of(level);
    if (tracklist_track(lib, list, cursor) != UINT32_MAX) {
      result.action = BROWSE_PLAY;
      result.list = list;
      result.index = cursor;
    }
    break;
  }
  }
  if (opened) {
    result.action = BROWSE_ENTERED;
  }
  return result;
}

bool browse_back(browse_t *b) {
  if (b == NULL) {
    return false;
  }
  current(b);
  if (b->depth == 0) {
    return false;
  }
  b->depth--;
  return true;
}
