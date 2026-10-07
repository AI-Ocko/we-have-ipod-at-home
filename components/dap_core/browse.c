#include "browse.h"

#include <string.h>

// Design notes
//
// A level is a kind of list, the number of the thing it lists (an artist or
// an album) and a cursor. What its items are is worked out from the index
// each time it is asked, with a fixed number of index queries per item
// whatever the size of the library; the queries are listed at item_text,
// browse_select and browse_target. Nothing here walks tracks, albums or
// artists.
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
// artist's songs, a playlist's entries) give every item '#': one group, so a
// jump made there anyway finds no next group and goes at most to the top.
//
// THE SETTINGS ROWS
//
// The Backlight and Sleep timer rows show values the caller keeps
// (browse_settings_t), read through the one pointer each time a row's text
// is asked for; without it they show their names. The values are not text
// in the index, so they are written straight into the caller's buffer
// (settings_row) rather than through item_text.
//
// THE PLAYLIST SCREENS
//
// What they list is not in the index: the caller lists the folder, loads a
// playlist's entries, and attaches both (browse_playlists_t). The stack
// keeps one pointer to that, never a copy, and reads it only inside a call,
// like the index. The Playlists level's rows are "New playlist", then the
// catalog's playlists, row r being playlist r - 1; a playlist's level has
// the playlist's number as its id. Row 0 of Playlists is treated as an
// artist's "All songs" is (see above): group '#', so letter jumps pass over
// it and "New playlist" is not under N; a playlist whose name starts with a
// digit shares its group.
//
// A playlist's level lists its entries only while the attached entries are
// that playlist's (`playlist` is the level's id) and the id is still in the
// catalog; otherwise it is an empty list, as a library level whose number
// the index no longer has. An entry is missing if its track is not one of
// the index's (PLAYLIST_MISSING never is), so with no index every entry is.
//
// browse_select does what browse_target says is under the cursor, where
// there is something: the two cannot disagree about what a row is. Only rows
// with no target (the fixed menus, "All songs", "New playlist") are decided
// in browse_select alone.

static const char *const MAIN_ITEMS[] = {
    "Now Playing", "Artists", "Albums", "Songs", "Playlists", "Settings"};
enum {
  MAIN_NOW_PLAYING,
  MAIN_ARTISTS,
  MAIN_ALBUMS,
  MAIN_SONGS,
  MAIN_PLAYLISTS,
  MAIN_SETTINGS,
  MAIN_COUNT
};

// The names of the Settings rows; with values attached, the Backlight and
// Sleep timer rows show them instead (settings_row).
static const char *const SETTINGS_ITEMS[] = {
    "Rescan library", "Backlight", "Sleep timer", "Keypad test", "About"};
enum {
  SETTINGS_RESCAN,
  SETTINGS_BACKLIGHT,
  SETTINGS_SLEEP,
  SETTINGS_KEYTEST,
  SETTINGS_ABOUT,
  SETTINGS_COUNT
};

static const char *const BACKLIGHT_ROWS[STANDBY_BACKLIGHT_COUNT] = {
    "Backlight: 15 s", "Backlight: 30 s", "Backlight: 60 s", "Backlight: on"};

// The most minutes the Sleep row shows, so that it stays within 15
// characters ("Sleep: 999 min"); the timer itself is at most 60.
#define SLEEP_ROW_MINUTES_MAX 999

static const char ALL_SONGS[] = "All songs";
static const char NEW_PLAYLIST[] = "New playlist";
static const char MISSING[] = "(missing)";

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

// The catalog attached, or NULL.
static const playlist_catalog_t *catalog_of(const browse_playlists_t *pl) {
  return pl != NULL ? pl->catalog : NULL;
}

// Whether the attached entries are those of the playlist this level lists
// (see THE PLAYLIST SCREENS).
static bool entries_loaded(const browse_level_t *level,
                           const browse_playlists_t *pl) {
  const playlist_catalog_t *catalog = catalog_of(pl);
  return catalog != NULL && level->id < catalog->count && pl->tracks != NULL &&
         pl->playlist == level->id;
}

// Whether a playlist's level has an entry `index` now.
static bool has_entry(const browse_level_t *level, const browse_playlists_t *pl,
                      uint32_t index) {
  return entries_loaded(level, pl) && index < pl->count;
}

// Whether entry `index` (which exists) is a track of this index.
static bool entry_plays(const browse_playlists_t *pl, const library_t *lib,
                        uint32_t index) {
  return pl->tracks[index] < library_track_count(lib);
}

// How many items the level has in this index: one query each.
static uint32_t count_of(const browse_level_t *level, const library_t *lib,
                         const browse_playlists_t *pl) {
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
  case BROWSE_PLAYLISTS: {
    // No listing yet: nothing, not even "New playlist", whose name could
    // not be checked against the playlists there are.
    const playlist_catalog_t *catalog = catalog_of(pl);
    return catalog != NULL ? catalog->count + 1 : 0;
  }
  case BROWSE_PLAYLIST:
    return entries_loaded(level, pl) ? pl->count : 0;
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
//   Playlists       nothing: for a playlist row the text is its FILE name
//                   (browse_item shows the name, without the ending; the
//                   first character, which is all browse_group reads, is
//                   the same)
//   a playlist      library_track_count, library_track_title
static const char *item_text(const browse_level_t *level, const library_t *lib,
                             const browse_playlists_t *pl, uint32_t index) {
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
  case BROWSE_PLAYLISTS:
    if (catalog_of(pl) == NULL) {
      return "";
    }
    return index == 0 ? NEW_PLAYLIST
                      : playlist_catalog_file(catalog_of(pl), index - 1);
  case BROWSE_PLAYLIST:
    if (!has_entry(level, pl, index)) {
      return "";
    }
    return entry_plays(pl, lib, index)
               ? library_track_title(lib, pl->tracks[index])
               : MISSING;
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

void browse_set_playlists(browse_t *b, const browse_playlists_t *p) {
  if (b != NULL) {
    b->playlists = p;
  }
}

void browse_set_settings(browse_t *b, const browse_settings_t *s) {
  if (b != NULL) {
    b->settings = s;
  }
}

// The text of Settings row `index` when it shows a value: written to `out`
// and true; false for a row that shows only its name. "Sleep: " and up to
// three digits and " min" is at most 14 characters.
static bool settings_row(const browse_settings_t *set, uint32_t index,
                         char *out, size_t out_size) {
  if (set == NULL) {
    return false;
  }
  if (index == SETTINGS_BACKLIGHT) {
    if ((unsigned)set->backlight >= STANDBY_BACKLIGHT_COUNT) {
      return false;
    }
    copy_out(BACKLIGHT_ROWS[set->backlight], out, out_size);
    return true;
  }
  if (index == SETTINGS_SLEEP) {
    if (set->sleep_minutes == 0) {
      copy_out("Sleep: off", out, out_size);
      return true;
    }
    uint32_t minutes = set->sleep_minutes < SLEEP_ROW_MINUTES_MAX
                           ? set->sleep_minutes
                           : SLEEP_ROW_MINUTES_MAX;
    char text[16] = "Sleep: ";
    size_t len = 7;
    char digits[3];
    size_t n = 0;
    do {
      digits[n++] = (char)('0' + minutes % 10);
      minutes /= 10;
    } while (minutes != 0);
    while (n > 0) {
      text[len++] = digits[--n];
    }
    static const char MIN[] = " min";
    for (size_t i = 0; i < sizeof(MIN); i++) { // the terminator too
      text[len++] = MIN[i];
    }
    copy_out(text, out, out_size);
    return true;
  }
  return false;
}

static bool is_playlist_level(const browse_level_t *level) {
  return level->screen == BROWSE_PLAYLISTS || level->screen == BROWSE_PLAYLIST;
}

void browse_refresh(browse_t *b, const library_t *lib) {
  if (b == NULL) {
    return;
  }
  browse_level_t *level = current(b);
  // The playlist levels under the current one too: the catalog can change
  // while a playlist is open (a rename, a delete), and going back must find
  // the Playlists level's count and cursor right. Library levels under the
  // current one cannot change without browse_library_changed.
  for (int d = 0; d < b->depth; d++) {
    browse_level_t *under = &b->levels[d];
    if (is_playlist_level(under)) {
      menu_set_count(&under->menu, count_of(under, lib, b->playlists));
    }
  }
  menu_set_count(&level->menu, count_of(level, lib, b->playlists));
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
    case BROWSE_PLAYLISTS:
      name = "Playlists";
      break;
    case BROWSE_PLAYLIST:
      // "" for a number the catalog does not have.
      playlist_catalog_name(catalog_of(b->playlists), level->id, out, out_size);
      return;
    }
  }
  copy_out(name, out, out_size);
}

void browse_item(const browse_t *b, const library_t *lib, uint32_t index,
                 char *out, size_t out_size) {
  if (b == NULL) {
    copy_out("", out, out_size);
    return;
  }
  const browse_level_t *level = level_of(b);
  if (level->screen == BROWSE_SETTINGS &&
      settings_row(b->settings, index, out, out_size)) {
    return;
  }
  if (level->screen == BROWSE_PLAYLISTS && index != 0 &&
      catalog_of(b->playlists) != NULL) {
    playlist_catalog_name(catalog_of(b->playlists), index - 1, out, out_size);
    return;
  }
  copy_out(item_text(level, lib, b->playlists, index), out, out_size);
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
  case BROWSE_PLAYLISTS: // see THE PLAYLIST SCREENS
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
  if ((level->screen == BROWSE_ARTIST || level->screen == BROWSE_PLAYLISTS) &&
      index == 0) {
    return '#'; // "All songs" or "New playlist"
  }
  // Only the first character is read, there and then.
  return menu_group(item_text(level, lib, b->playlists, index));
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

// A row with no target: what the fixed menus, "All songs" and "New
// playlist" do. What it asks the index: for "All songs",
// library_artist_albums (is there such an artist?); nothing otherwise.
static browse_result_t select_untargeted(browse_t *b, const library_t *lib,
                                         const browse_level_t *level,
                                         uint32_t cursor) {
  browse_result_t result = {.action = BROWSE_NOTHING,
                            .list = {.kind = TRACKLIST_NONE, .id = 0},
                            .index = 0};
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
    case MAIN_PLAYLISTS:
      opened = open_level(b, lib, BROWSE_PLAYLISTS, 0);
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
    case SETTINGS_BACKLIGHT:
      result.action = BROWSE_BACKLIGHT;
      break;
    case SETTINGS_SLEEP:
      result.action = BROWSE_SLEEP;
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

  case BROWSE_ARTIST:
    if (cursor == 0) {
      uint32_t first, albums;
      library_artist_albums(lib, level->id, &first, &albums);
      if (albums != 0) { // there is such an artist in this index
        opened = open_level(b, lib, BROWSE_ARTIST_SONGS, level->id);
      }
    }
    break;

  case BROWSE_PLAYLISTS:
    if (cursor == 0 && catalog_of(b->playlists) != NULL) {
      result.action = BROWSE_NEW_PLAYLIST;
    }
    break;

  default:
    break; // every other row has a target, or is nothing
  }
  if (opened) {
    result.action = BROWSE_ENTERED;
  }
  return result;
}

// What it asks the index before it acts is what browse_target asks, and for
// a row with no target what select_untargeted asks; opening a level then
// counts the new level, one query more. An entry asks library_track_count.
browse_result_t browse_select(browse_t *b, const library_t *lib) {
  browse_result_t result = {.action = BROWSE_NOTHING,
                            .list = {.kind = TRACKLIST_NONE, .id = 0},
                            .index = 0};
  if (b == NULL) {
    return result;
  }
  const browse_level_t *level = current(b);
  uint32_t cursor = level->menu.cursor;
  if (cursor >= level->menu.count) {
    return result; // an empty list
  }

  browse_target_t target = browse_target(b, lib);
  switch (target.kind) {
  case BROWSE_TARGET_TRACK:
    // The list to play is the list on the screen, from the row selected.
    result.action = BROWSE_PLAY;
    result.list = target.list;
    result.index = target.index;
    break;
  case BROWSE_TARGET_ALBUM:
    if (open_level(b, lib, BROWSE_ALBUM, target.list.id)) {
      result.action = BROWSE_ENTERED;
    }
    break;
  case BROWSE_TARGET_ARTIST:
    if (open_level(b, lib, BROWSE_ARTIST, target.list.id)) {
      result.action = BROWSE_ENTERED;
    }
    break;
  case BROWSE_TARGET_PLAYLIST:
    if (open_level(b, lib, BROWSE_PLAYLIST, target.index)) {
      result.action = BROWSE_OPEN_PLAYLIST;
      result.index = target.index;
    }
    break;
  case BROWSE_TARGET_ENTRY:
    result.action = entry_plays(b->playlists, lib, target.index)
                        ? BROWSE_PLAY_ENTRIES
                        : BROWSE_MISSING;
    result.index = target.index;
    break;
  case BROWSE_TARGET_NONE:
  case BROWSE_TARGET_NOW_PLAYING: // never given; as NONE
    result = select_untargeted(b, lib, level, cursor);
    break;
  }
  return result;
}

// What it asks the index, one query whatever the size of the library:
//   Main, Settings  nothing
//   Artists         library_artist_albums: is there such an artist?
//   an artist       library_artist_albums: the artist, and which album
//   Albums          library_album_by_name: which album
//   a track         tracklist_track: is there a track at this position?
//   Playlists, a playlist   nothing
// browse_select acts on what this finds, so what the options menu is
// offered for is what selecting the row would open or play.
browse_target_t browse_target(const browse_t *b, const library_t *lib) {
  browse_target_t target = {.kind = BROWSE_TARGET_NONE,
                            .list = {.kind = TRACKLIST_NONE, .id = 0},
                            .index = 0};
  if (b == NULL) {
    return target;
  }
  const browse_level_t *level = level_of(b);
  uint32_t cursor = level->menu.cursor;
  if (cursor >= level->menu.count) {
    return target; // an empty list
  }

  switch (level->screen) {
  case BROWSE_MAIN:
  case BROWSE_SETTINGS:
    break;

  case BROWSE_ARTISTS: {
    uint32_t first, albums;
    library_artist_albums(lib, cursor, &first, &albums);
    if (albums != 0) {
      target.kind = BROWSE_TARGET_ARTIST;
      target.list.kind = TRACKLIST_ARTIST;
      target.list.id = cursor;
    }
    break;
  }

  case BROWSE_ARTIST: {
    uint32_t first, albums;
    library_artist_albums(lib, level->id, &first, &albums);
    // Row 0 is "All songs", which is not a thing to act on.
    if (cursor != 0 && cursor - 1 < albums) {
      target.kind = BROWSE_TARGET_ALBUM;
      target.list.kind = TRACKLIST_ALBUM;
      target.list.id = first + (cursor - 1);
    }
    break;
  }

  case BROWSE_ALBUMS: {
    uint32_t album = library_album_by_name(lib, cursor);
    if (album != UINT32_MAX) {
      target.kind = BROWSE_TARGET_ALBUM;
      target.list.kind = TRACKLIST_ALBUM;
      target.list.id = album;
    }
    break;
  }

  case BROWSE_ARTIST_SONGS:
  case BROWSE_ALBUM:
  case BROWSE_SONGS: {
    tracklist_t list = tracks_of(level);
    if (tracklist_track(lib, list, cursor) != UINT32_MAX) {
      target.kind = BROWSE_TARGET_TRACK;
      target.list = list;
      target.index = cursor;
    }
    break;
  }

  case BROWSE_PLAYLISTS: {
    // Row 0 is "New playlist", which is not a thing to act on.
    const playlist_catalog_t *catalog = catalog_of(b->playlists);
    if (cursor != 0 && catalog != NULL && cursor - 1 < catalog->count) {
      target.kind = BROWSE_TARGET_PLAYLIST;
      target.index = cursor - 1;
    }
    break;
  }

  case BROWSE_PLAYLIST:
    // Missing entries too: they can be moved and removed.
    if (has_entry(level, b->playlists, cursor)) {
      target.kind = BROWSE_TARGET_ENTRY;
      target.index = cursor;
    }
    break;
  }
  return target;
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
