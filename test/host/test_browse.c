#include "browse.h"
#include "lib16.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

// The library of lib16.h, and room for a second one.
static uint32_t arena_words[LIB16_ARENA_BYTES / 4];
static uint32_t other_words[LIB16_ARENA_BYTES / 4];
static const library_t *lib;
static browse_t b;

void setUp(void) {
  lib = lib16_build(arena_words, LIB16_TRACKS);
  memset(&b, 0xA5, sizeof(b)); // browse_init must not rely on zeros
  browse_init(&b);
}

void tearDown(void) {}

// The numbers the comment in lib16.h gives.
enum { BJORK, DEEP_ARTIST, QUEEN, RADIOHEAD, UNKNOWN_ARTIST, VARIOUS };
enum {
  HOMOGENIC,
  DEEP_ALBUM,
  HITS_QUEEN,
  KID_A,
  OK_COMPUTER,
  UNKNOWN_ALBUM,
  HITS_VARIOUS
};
// The rows of the main menu.
enum { NOW_PLAYING, ARTISTS, ALBUMS, SONGS, SETTINGS };

// --- Helpers -----------------------------------------------------------------

static const browse_level_t *level(void) { return &b.levels[b.depth]; }

static const char *item_in(const library_t *l, uint32_t index) {
  static char text[128];
  memset(text, '?', sizeof(text));
  browse_item(&b, l, index, text, sizeof(text));
  TEST_ASSERT_NOT_NULL(memchr(text, '\0', sizeof(text)));
  return text;
}

static const char *item(uint32_t index) { return item_in(lib, index); }

static const char *name_in(const library_t *l) {
  static char text[128];
  memset(text, '?', sizeof(text));
  browse_name(&b, l, text, sizeof(text));
  TEST_ASSERT_NOT_NULL(memchr(text, '\0', sizeof(text)));
  return text;
}

static const char *name(void) { return name_in(lib); }

// The current level is exactly these items, with nothing after them.
static void assert_items(const char *const *items, uint32_t n) {
  TEST_ASSERT_EQUAL_UINT32(n, browse_menu(&b)->count);
  for (uint32_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL_STRING(items[i], item(i));
  }
  TEST_ASSERT_EQUAL_STRING("", item(n));
  TEST_ASSERT_EQUAL_STRING("", item(UINT32_MAX));
}

static void assert_at(browse_screen_t screen, uint32_t id, int depth) {
  TEST_ASSERT_EQUAL_INT(depth, b.depth);
  TEST_ASSERT_EQUAL_INT(screen, level()->screen);
  TEST_ASSERT_EQUAL_UINT32(id, level()->id);
  TEST_ASSERT_EQUAL_PTR(&b.levels[depth].menu, browse_menu(&b));
}

static browse_result_t select_in(const library_t *l, uint32_t row) {
  browse_menu(&b)->cursor = row;
  return browse_select(&b, l);
}

// Select `row`, which must open a level; the new level starts at its top.
static void enter(uint32_t row) {
  int depth = b.depth;
  browse_result_t r = select_in(lib, row);
  TEST_ASSERT_EQUAL_INT(BROWSE_ENTERED, r.action);
  TEST_ASSERT_EQUAL_INT(depth + 1, b.depth);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_UINT32(row, b.levels[depth].menu.cursor);
}

// Select `row`, which must do `action` and leave the stack alone.
static browse_result_t act_in(const library_t *l, uint32_t row,
                              browse_action_t action) {
  browse_t before = b;
  before.levels[b.depth].menu.cursor = row;
  browse_result_t r = select_in(l, row);
  TEST_ASSERT_EQUAL_INT(action, r.action);
  TEST_ASSERT_EQUAL_INT(before.depth, b.depth);
  for (int i = 0; i <= b.depth; i++) {
    TEST_ASSERT_EQUAL_INT(before.levels[i].screen, b.levels[i].screen);
    TEST_ASSERT_EQUAL_UINT32(before.levels[i].id, b.levels[i].id);
    TEST_ASSERT_EQUAL_UINT32(before.levels[i].menu.count,
                             b.levels[i].menu.count);
    TEST_ASSERT_EQUAL_UINT32(before.levels[i].menu.cursor,
                             b.levels[i].menu.cursor);
  }
  if (action != BROWSE_PLAY) {
    TEST_ASSERT_EQUAL_INT(TRACKLIST_NONE, r.list.kind);
  }
  return r;
}

static browse_result_t act(uint32_t row, browse_action_t action) {
  return act_in(lib, row, action);
}

// Selecting `row` asks to play `kind`/`id` from that row, and the track
// there is `title`.
static void assert_plays(uint32_t row, tracklist_kind_t kind, uint32_t id,
                         const char *title) {
  browse_result_t r = act(row, BROWSE_PLAY);
  TEST_ASSERT_EQUAL_INT(kind, r.list.kind);
  TEST_ASSERT_EQUAL_UINT32(id, r.list.id);
  TEST_ASSERT_EQUAL_UINT32(row, r.index);
  TEST_ASSERT_EQUAL_UINT32(browse_menu(&b)->count,
                           tracklist_count(lib, r.list));
  TEST_ASSERT_EQUAL_STRING(
      title, lib16_title(lib, tracklist_track(lib, r.list, r.index)));
  TEST_ASSERT_EQUAL_STRING(title, item(row));
}

// The groups of the current level's items, as a string.
static const char *groups(void) {
  static char text[64];
  uint32_t n = browse_menu(&b)->count;
  TEST_ASSERT_LESS_THAN_UINT32(sizeof(text), n);
  for (uint32_t i = 0; i < n; i++) {
    text[i] = browse_group(&b, lib, i);
  }
  text[n] = '\0';
  return text;
}

// A letter jump as the UI will make it.
typedef struct {
  const library_t *lib;
  uint32_t calls;
} jump_ctx_t;

static char group_for_jump(void *ctx, uint32_t index) {
  jump_ctx_t *j = ctx;
  j->calls++;
  TEST_ASSERT_LESS_THAN_UINT32(browse_menu(&b)->count, index);
  return browse_group(&b, j->lib, index);
}

static uint32_t jump_from(uint32_t row, int direction) {
  jump_ctx_t ctx = {.lib = lib, .calls = 0};
  TEST_ASSERT_TRUE(browse_is_alphabetical(&b));
  browse_menu(&b)->cursor = row;
  menu_jump_group(browse_menu(&b), direction, group_for_jump, &ctx);
  return browse_menu(&b)->cursor;
}

static const char *const MAIN_ITEMS[] = {"Now Playing", "Artists", "Albums",
                                         "Songs", "Settings"};
static const char *const SETTINGS_ITEMS[] = {"Rescan library", "Keypad test",
                                             "About"};
static const char *const ARTIST_NAMES[] = {"Bjork",          "Deep Artist",
                                           "Queen",          "Radiohead",
                                           "Unknown Artist", "Various Artists"};
static const char *const ALBUM_NAMES[] = {
    "Deep Album", "Greatest Hits", "Greatest Hits", "Homogenic",
    "Kid A",      "OK Computer",   "Unknown Album"};
static const char *const SONG_TITLES[LIB16_TRACKS] = {
    "Airbag",
    "Alpha",
    "also untagged",
    "Beta",
    "Bohemian Rhapsody",
    "Deep Track",
    "empty",
    "Everything In Its Right Place",
    "Gamma",
    "Hunter",
    "Joga",
    "Kid A",
    "Motion Picture Soundtrack",
    "Paranoid Android",
    "text",
    "untagged song"};
static const char *const KID_A_TITLES[] = {"Everything In Its Right Place",
                                           "Kid A",
                                           "Motion Picture Soundtrack"};
static const char *const RADIOHEAD_TITLES[] = {
    "Everything In Its Right Place", "Kid A", "Motion Picture Soundtrack",
    "Airbag", "Paranoid Android"};

// --- The tree ----------------------------------------------------------------

void test_starts_at_the_main_menu(void) {
  // Straight after browse_init, before any refresh and without an index.
  assert_at(BROWSE_MAIN, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(5, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_STRING("Main", name());
  TEST_ASSERT_EQUAL_STRING("Now Playing", item(0));
}

void test_main_menu_items_and_actions(void) {
  browse_refresh(&b, lib);
  assert_items(MAIN_ITEMS, 5);

  act(NOW_PLAYING, BROWSE_NOW_PLAYING);

  enter(ARTISTS);
  assert_at(BROWSE_ARTISTS, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(6, browse_menu(&b)->count);
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(ALBUMS);
  assert_at(BROWSE_ALBUMS, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(7, browse_menu(&b)->count);
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(SONGS);
  assert_at(BROWSE_SONGS, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(16, browse_menu(&b)->count);
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(SETTINGS);
  assert_at(BROWSE_SETTINGS, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(3, browse_menu(&b)->count);
  TEST_ASSERT_TRUE(browse_back(&b));
  assert_at(BROWSE_MAIN, 0, 0);
}

void test_settings_items_and_actions(void) {
  enter(SETTINGS);
  assert_items(SETTINGS_ITEMS, 3);
  act(0, BROWSE_RESCAN);
  act(1, BROWSE_KEYTEST);
  act(2, BROWSE_ABOUT);
}

void test_artists_are_listed_a_to_z(void) {
  enter(ARTISTS);
  assert_items(ARTIST_NAMES, 6);
}

void test_an_artist_lists_all_songs_then_its_albums(void) {
  enter(ARTISTS);
  enter(RADIOHEAD);
  assert_at(BROWSE_ARTIST, RADIOHEAD, 2);
  static const char *const RADIOHEAD_ROWS[] = {"All songs", "Kid A",
                                               "OK Computer"};
  assert_items(RADIOHEAD_ROWS, 3);
  enter(2);
  assert_at(BROWSE_ALBUM, OK_COMPUTER, 3);
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(1);
  assert_at(BROWSE_ALBUM, KID_A, 3);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));

  // The two albums called Greatest Hits each belong to one artist.
  static const char *const ONE_ALBUM[] = {"All songs", "Greatest Hits"};
  enter(QUEEN);
  assert_items(ONE_ALBUM, 2);
  enter(1);
  assert_at(BROWSE_ALBUM, HITS_QUEEN, 3);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(VARIOUS);
  assert_items(ONE_ALBUM, 2);
  enter(1);
  assert_at(BROWSE_ALBUM, HITS_VARIOUS, 3);
}

void test_all_songs_of_an_artist(void) {
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(0);
  assert_at(BROWSE_ARTIST_SONGS, RADIOHEAD, 3);
  assert_items(RADIOHEAD_TITLES, 5);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(UNKNOWN_ARTIST);
  enter(0);
  static const char *const LOOSE[] = {"also untagged", "empty", "text",
                                      "untagged song"};
  assert_items(LOOSE, 4);
}

void test_album_tracks_are_in_album_order(void) {
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  // Track 10 after track 2.
  assert_items(KID_A_TITLES, 3);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(VARIOUS);
  enter(1);
  // Disc 1 track 1, disc 1 track 2, disc 2 track 1.
  static const char *const HITS[] = {"Alpha", "Beta", "Gamma"};
  assert_items(HITS, 3);
}

void test_albums_and_songs_lists(void) {
  enter(ALBUMS);
  assert_items(ALBUM_NAMES, 7);
  // Each row opens its own album, the two of one name included.
  static const uint32_t OPENS[] = {DEEP_ALBUM, HITS_QUEEN,  HITS_VARIOUS,
                                   HOMOGENIC,  KID_A,       OK_COMPUTER,
                                   UNKNOWN_ALBUM};
  for (uint32_t row = 0; row < 7; row++) {
    enter(row);
    assert_at(BROWSE_ALBUM, OPENS[row], 2);
    TEST_ASSERT_EQUAL_STRING(ALBUM_NAMES[row], name());
    TEST_ASSERT_TRUE(browse_back(&b));
  }
  enter(1);
  static const char *const QUEEN_HITS[] = {"Bohemian Rhapsody"};
  assert_items(QUEEN_HITS, 1);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(SONGS);
  assert_items(SONG_TITLES, 16);
}

void test_selecting_a_track_asks_to_play_its_list_from_that_position(void) {
  // From an album.
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  assert_plays(2, TRACKLIST_ALBUM, KID_A, "Motion Picture Soundtrack");
  assert_plays(0, TRACKLIST_ALBUM, KID_A, "Everything In Its Right Place");
  assert_at(BROWSE_ALBUM, KID_A, 3);
  TEST_ASSERT_TRUE(browse_back(&b));

  // From an artist's songs.
  enter(0);
  assert_plays(3, TRACKLIST_ARTIST, RADIOHEAD, "Airbag");
  assert_plays(4, TRACKLIST_ARTIST, RADIOHEAD, "Paranoid Android");
  assert_at(BROWSE_ARTIST_SONGS, RADIOHEAD, 3);
  browse_library_changed(&b);

  // From an album reached through Albums.
  enter(ALBUMS);
  enter(2);
  assert_plays(1, TRACKLIST_ALBUM, HITS_VARIOUS, "Beta");
  browse_library_changed(&b);

  // From Songs.
  enter(SONGS);
  assert_plays(4, TRACKLIST_SONGS, 0, "Bohemian Rhapsody");
  assert_plays(15, TRACKLIST_SONGS, 0, "untagged song");
  assert_plays(0, TRACKLIST_SONGS, 0, "Airbag");
  assert_at(BROWSE_SONGS, 0, 1);
}

void test_back_returns_to_the_same_cursor(void) {
  enter(ARTISTS);
  menu_move(browse_menu(&b), 4, false); // the 5th
  enter(4);
  assert_at(BROWSE_ARTIST, UNKNOWN_ARTIST, 2);
  menu_move(browse_menu(&b), 1, false);
  enter(1);
  assert_at(BROWSE_ALBUM, UNKNOWN_ALBUM, 3);
  menu_move(browse_menu(&b), 3, false);

  TEST_ASSERT_TRUE(browse_back(&b));
  assert_at(BROWSE_ARTIST, UNKNOWN_ARTIST, 2);
  TEST_ASSERT_EQUAL_UINT32(1, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->count);

  TEST_ASSERT_TRUE(browse_back(&b));
  assert_at(BROWSE_ARTISTS, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(4, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_UINT32(6, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_STRING("Unknown Artist", item(browse_menu(&b)->cursor));

  TEST_ASSERT_TRUE(browse_back(&b));
  assert_at(BROWSE_MAIN, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(ARTISTS, browse_menu(&b)->cursor);

  // A level entered again starts at its top.
  enter(ARTISTS);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
}

void test_back_at_the_main_menu_is_refused(void) {
  TEST_ASSERT_FALSE(browse_back(&b));
  assert_at(BROWSE_MAIN, 0, 0);
  browse_menu(&b)->cursor = 3;
  TEST_ASSERT_FALSE(browse_back(&b));
  assert_at(BROWSE_MAIN, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(3, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_UINT32(5, browse_menu(&b)->count);
}

void test_names_for_the_top_row(void) {
  TEST_ASSERT_EQUAL_STRING("Main", name());
  enter(ARTISTS);
  TEST_ASSERT_EQUAL_STRING("Artists", name());
  enter(RADIOHEAD);
  TEST_ASSERT_EQUAL_STRING("Radiohead", name());
  enter(0);
  // An artist's songs are under the artist's name too.
  TEST_ASSERT_EQUAL_STRING("Radiohead", name());
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(2);
  TEST_ASSERT_EQUAL_STRING("OK Computer", name());
  browse_library_changed(&b);
  enter(ALBUMS);
  TEST_ASSERT_EQUAL_STRING("Albums", name());
  enter(6);
  TEST_ASSERT_EQUAL_STRING("Unknown Album", name());
  browse_library_changed(&b);
  enter(SONGS);
  TEST_ASSERT_EQUAL_STRING("Songs", name());
  browse_library_changed(&b);
  enter(SETTINGS);
  TEST_ASSERT_EQUAL_STRING("Settings", name());
}

void test_alphabetical_levels(void) {
  TEST_ASSERT_FALSE(browse_is_alphabetical(&b)); // Main
  enter(ARTISTS);
  TEST_ASSERT_TRUE(browse_is_alphabetical(&b));
  enter(RADIOHEAD);
  // An artist's albums are A to Z. The row above them, "All songs", is given
  // the group '#', which is before every letter, so the level as a whole is
  // still in group order (see test_groups_follow_the_items).
  TEST_ASSERT_TRUE(browse_is_alphabetical(&b));
  enter(0);
  TEST_ASSERT_FALSE(browse_is_alphabetical(&b)); // an artist's songs
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(1);
  TEST_ASSERT_FALSE(browse_is_alphabetical(&b)); // an album's tracks
  browse_library_changed(&b);
  enter(ALBUMS);
  TEST_ASSERT_TRUE(browse_is_alphabetical(&b));
  browse_library_changed(&b);
  enter(SONGS);
  TEST_ASSERT_TRUE(browse_is_alphabetical(&b));
  browse_library_changed(&b);
  enter(SETTINGS);
  TEST_ASSERT_FALSE(browse_is_alphabetical(&b));
}

void test_groups_follow_the_items(void) {
  enter(ARTISTS);
  TEST_ASSERT_EQUAL_STRING("BDQRUV", groups());
  TEST_ASSERT_EQUAL_UINT32(1, jump_from(0, 1));
  TEST_ASSERT_EQUAL_UINT32(3, jump_from(2, 1));
  TEST_ASSERT_EQUAL_UINT32(5, jump_from(5, 1));
  TEST_ASSERT_EQUAL_UINT32(4, jump_from(5, -1));

  enter(RADIOHEAD);
  // "All songs" is '#', not 'A': it is not one of the names.
  TEST_ASSERT_EQUAL_STRING("#KO", groups());
  TEST_ASSERT_EQUAL_UINT32(1, jump_from(0, 1));
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(1, 1));
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(2, 1));
  TEST_ASSERT_EQUAL_UINT32(1, jump_from(2, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump_from(1, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump_from(0, -1));
  browse_library_changed(&b);

  enter(ALBUMS);
  TEST_ASSERT_EQUAL_STRING("DGGHKOU", groups());
  TEST_ASSERT_EQUAL_UINT32(3, jump_from(1, 1));
  TEST_ASSERT_EQUAL_UINT32(3, jump_from(2, 1));
  TEST_ASSERT_EQUAL_UINT32(1, jump_from(2, -1));
  TEST_ASSERT_EQUAL_UINT32(1, jump_from(3, -1));
  browse_library_changed(&b);

  enter(SONGS);
  // Lower case titles ("also untagged", "empty", "text") are with their
  // letters.
  TEST_ASSERT_EQUAL_STRING("AAABBDEEGHJKMPTU", groups());
  TEST_ASSERT_EQUAL_UINT32(3, jump_from(0, 1));
  TEST_ASSERT_EQUAL_UINT32(5, jump_from(3, 1));
  TEST_ASSERT_EQUAL_UINT32(6, jump_from(5, 1));
  TEST_ASSERT_EQUAL_UINT32(8, jump_from(6, 1));
  TEST_ASSERT_EQUAL_UINT32(6, jump_from(7, -1));
  TEST_ASSERT_EQUAL_UINT32(5, jump_from(6, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump_from(2, -1));
  TEST_ASSERT_EQUAL_UINT32(15, jump_from(15, 1));
  // An item that is not there has the group of an empty text.
  TEST_ASSERT_EQUAL_CHAR('#', browse_group(&b, lib, 16));
  TEST_ASSERT_EQUAL_CHAR('#', browse_group(&b, lib, UINT32_MAX));
  TEST_ASSERT_EQUAL_CHAR('#', browse_group(&b, NULL, 3));
}

// Extra: the levels that are not A to Z give every item the same group, so a
// jump made there anyway finds no next group and cannot land somewhere
// arbitrary.
void test_levels_that_are_not_alphabetical_have_one_group(void) {
  TEST_ASSERT_EQUAL_STRING("#####", groups()); // Main
  enter(SETTINGS);
  TEST_ASSERT_EQUAL_STRING("###", groups());
  browse_library_changed(&b);
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(0);
  TEST_ASSERT_EQUAL_STRING("#####", groups()); // E K M A P by title
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(1);
  TEST_ASSERT_EQUAL_STRING("###", groups());
}

// Extra: an artist with an album whose name starts with a digit. "All songs"
// and that album are both '#', and the groups still never go back.
void test_an_artists_number_album_shares_the_first_group(void) {
  static const library_track_in_t TRACKS[] = {
      LIB16_ROW("p/s/1.flac", "Sign o' the Times", "Prince",
                "Sign o' the Times", "Prince", 0, 1, 100, TAGS_FORMAT_FLAC),
      LIB16_ROW("p/9/1.flac", "1999", "Prince", "1999", "Prince", 0, 1, 100,
                TAGS_FORMAT_FLAC),
      LIB16_ROW("p/p/1.flac", "Purple Rain", "Prince", "Purple Rain", "Prince",
                0, 1, 100, TAGS_FORMAT_FLAC),
      LIB16_ROW("p/a/1.flac", "Kiss", "Prince", "parade", "Prince", 0, 1, 100,
                TAGS_FORMAT_FLAC),
  };
  lib = lib16_build_from(other_words, TRACKS, 4);
  browse_library_changed(&b);
  enter(ARTISTS);
  enter(0);
  static const char *const ROWS[] = {"All songs", "1999", "parade",
                                     "Purple Rain", "Sign o' the Times"};
  assert_items(ROWS, 5);
  TEST_ASSERT_EQUAL_STRING("##PPS", groups());
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(0, 1));
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(1, 1));
  TEST_ASSERT_EQUAL_UINT32(4, jump_from(2, 1));
  TEST_ASSERT_EQUAL_UINT32(4, jump_from(4, 1));
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(4, -1));
  TEST_ASSERT_EQUAL_UINT32(2, jump_from(3, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump_from(2, -1));
  TEST_ASSERT_EQUAL_UINT32(0, jump_from(1, -1));
}

// --- No index, a new index, a smaller index ----------------------------------

void test_null_library_leaves_main_and_settings_working(void) {
  lib = NULL;
  browse_refresh(&b, NULL);
  assert_items(MAIN_ITEMS, 5);
  TEST_ASSERT_EQUAL_STRING("Main", name());
  act(NOW_PLAYING, BROWSE_NOW_PLAYING);

  enter(SETTINGS);
  browse_refresh(&b, NULL);
  assert_items(SETTINGS_ITEMS, 3);
  TEST_ASSERT_EQUAL_STRING("Settings", name());
  act(0, BROWSE_RESCAN);
  act(1, BROWSE_KEYTEST);
  act(2, BROWSE_ABOUT);
  TEST_ASSERT_TRUE(browse_back(&b));

  // The library lists can be opened, and are empty.
  static const struct {
    uint32_t row;
    browse_screen_t screen;
    const char *name;
  } LISTS[] = {{ARTISTS, BROWSE_ARTISTS, "Artists"},
               {ALBUMS, BROWSE_ALBUMS, "Albums"},
               {SONGS, BROWSE_SONGS, "Songs"}};
  for (size_t i = 0; i < 3; i++) {
    enter(LISTS[i].row);
    assert_at(LISTS[i].screen, 0, 1);
    assert_items(NULL, 0);
    TEST_ASSERT_EQUAL_STRING(LISTS[i].name, name());
    act(0, BROWSE_NOTHING);
    browse_refresh(&b, NULL);
    assert_items(NULL, 0);
    TEST_ASSERT_EQUAL_CHAR('#', browse_group(&b, NULL, 0));
    TEST_ASSERT_TRUE(browse_back(&b));
  }
  assert_at(BROWSE_MAIN, 0, 0);
}

// Extra: the index goes away while the user is deep in it. Nothing has been
// refreshed yet, so the counts on the stack are stale; every call must still
// be safe and say there is nothing there.
void test_null_library_under_a_level_that_was_open(void) {
  enter(ARTISTS);
  enter(RADIOHEAD);
  for (int pass = 0; pass < 3; pass++) {
    if (pass == 1) {
      enter(1); // Kid A
    } else if (pass == 2) {
      TEST_ASSERT_TRUE(browse_back(&b));
      enter(0); // All songs
    }
    uint32_t count = browse_menu(&b)->count;
    TEST_ASSERT_GREATER_THAN_UINT32(0, count);
    for (uint32_t row = 0; row < count; row++) {
      TEST_ASSERT_EQUAL_STRING("", item_in(NULL, row));
      act_in(NULL, row, BROWSE_NOTHING);
    }
    TEST_ASSERT_EQUAL_STRING("", name_in(NULL));
    browse_t before = b;
    browse_refresh(&b, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->count);
    TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
    act_in(NULL, 0, BROWSE_NOTHING);
    // And back again when the same index returns.
    b = before;
    browse_refresh(&b, lib);
    TEST_ASSERT_EQUAL_UINT32(count, browse_menu(&b)->count);
  }
}

void test_library_changed_returns_to_main(void) {
  browse_menu(&b)->cursor = ARTISTS;
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  browse_menu(&b)->cursor = 2;
  assert_at(BROWSE_ALBUM, KID_A, 3);

  browse_library_changed(&b);
  assert_at(BROWSE_MAIN, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(ARTISTS, browse_menu(&b)->cursor); // kept
  TEST_ASSERT_EQUAL_UINT32(5, browse_menu(&b)->count);
  TEST_ASSERT_FALSE(browse_back(&b));
  assert_items(MAIN_ITEMS, 5);

  // At the main menu already: nothing changes.
  browse_menu(&b)->cursor = SETTINGS;
  browse_library_changed(&b);
  assert_at(BROWSE_MAIN, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(SETTINGS, browse_menu(&b)->cursor);

  // The levels opened next are new ones, not what was left on the stack.
  enter(ARTISTS);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  enter(BJORK);
  assert_at(BROWSE_ARTIST, BJORK, 2);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->count);
}

void test_refresh_after_a_smaller_library_clamps_the_cursor(void) {
  // Radiohead alone: 1 artist, 2 albums, 5 tracks.
  const library_t *small = lib16_build(other_words, LIB16_RADIOHEAD_ONLY);

  enter(SONGS);
  browse_menu(&b)->cursor = 15;
  // Before the refresh the cursor is on an item the new index does not have.
  TEST_ASSERT_EQUAL_STRING("", item_in(small, 15));
  act_in(small, 15, BROWSE_NOTHING);
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(5, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(4, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_STRING("Paranoid Android", item_in(small, 4));
  browse_result_t r = browse_select(&b, small);
  TEST_ASSERT_EQUAL_INT(BROWSE_PLAY, r.action);
  TEST_ASSERT_EQUAL_INT(TRACKLIST_SONGS, r.list.kind);
  TEST_ASSERT_EQUAL_UINT32(4, r.index);
  // A cursor that still fits stays.
  browse_menu(&b)->cursor = 2;
  browse_refresh(&b, lib);
  TEST_ASSERT_EQUAL_UINT32(16, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->cursor);
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(ARTISTS);
  browse_menu(&b)->cursor = VARIOUS;
  act_in(small, VARIOUS, BROWSE_NOTHING);
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(1, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_STRING("Radiohead", item_in(small, 0));
  browse_refresh(&b, lib);
  TEST_ASSERT_TRUE(browse_back(&b));

  enter(ALBUMS);
  browse_menu(&b)->cursor = 6;
  act_in(small, 6, BROWSE_NOTHING);
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(1, browse_menu(&b)->cursor);
  TEST_ASSERT_EQUAL_STRING("OK Computer", item_in(small, 1));
  browse_refresh(&b, lib);
  TEST_ASSERT_TRUE(browse_back(&b));

  // Levels whose own number is no longer in the index become empty: the
  // artist, its songs, and an album.
  enter(ARTISTS);
  enter(VARIOUS);
  browse_menu(&b)->cursor = 1;
  act_in(small, 1, BROWSE_NOTHING);
  act_in(small, 0, BROWSE_NOTHING);
  TEST_ASSERT_EQUAL_STRING("", item_in(small, 0));
  TEST_ASSERT_EQUAL_STRING("", item_in(small, 1));
  TEST_ASSERT_EQUAL_STRING("", name_in(small));
  browse_menu(&b)->cursor = 1;
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  act_in(small, 0, BROWSE_NOTHING);
  browse_refresh(&b, lib);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->count);

  enter(1); // Greatest Hits, album 6: the small index has albums 0 and 1
  browse_menu(&b)->cursor = 2;
  act_in(small, 2, BROWSE_NOTHING);
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  TEST_ASSERT_TRUE(browse_back(&b));

  browse_refresh(&b, lib);
  enter(0);
  browse_menu(&b)->cursor = 2;
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->cursor);
  act_in(small, 0, BROWSE_NOTHING);

  // An album that the smaller index does have, with fewer tracks.
  browse_library_changed(&b);
  enter(ALBUMS);
  enter(0); // Deep Album is album 1: OK Computer (2 tracks) in the small one
  assert_at(BROWSE_ALBUM, DEEP_ALBUM, 2);
  browse_refresh(&b, small);
  TEST_ASSERT_EQUAL_UINT32(2, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_STRING("Airbag", item_in(small, 0));
}

// Extra: a list that the index says is there but that the level has not been
// told about yet (no refresh since it grew) is not acted on past the count
// the user was shown.
void test_select_past_the_count_on_show_does_nothing(void) {
  enter(SONGS);
  browse_menu(&b)->count = 0;
  browse_menu(&b)->cursor = 0;
  act(0, BROWSE_NOTHING);
  browse_menu(&b)->count = 3;
  browse_menu(&b)->cursor = 7;
  browse_result_t r = browse_select(&b, lib);
  TEST_ASSERT_EQUAL_INT(BROWSE_NOTHING, r.action);
  browse_refresh(&b, lib);
  TEST_ASSERT_EQUAL_UINT32(16, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_UINT32(7, browse_menu(&b)->cursor);
  assert_plays(7, TRACKLIST_SONGS, 0, "Everything In Its Right Place");
}

// --- Text out ----------------------------------------------------------------

void test_long_item_text_is_cut_to_the_buffer(void) {
  enter(SONGS);
  // "Everything In Its Right Place" is 29 characters.
  char guarded[40];

  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 7, guarded, 8);
  TEST_ASSERT_EQUAL_STRING("Everyth", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[8]);

  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 7, guarded, 30);
  TEST_ASSERT_EQUAL_STRING("Everything In Its Right Place", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[30]);

  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 7, guarded, 29);
  TEST_ASSERT_EQUAL_STRING("Everything In Its Right Plac", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[29]);

  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 7, guarded, 1);
  TEST_ASSERT_EQUAL_STRING("", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[1]);

  // No room at all: nothing is written.
  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 7, guarded, 0);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[0]);
  browse_item(&b, lib, 7, NULL, 8);

  // An item that is not there clears the buffer too.
  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 16, guarded, 8);
  TEST_ASSERT_EQUAL_STRING("", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[1]);

  // The fixed texts and the names are cut the same way.
  browse_library_changed(&b);
  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 0, guarded, 4);
  TEST_ASSERT_EQUAL_STRING("Now", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[4]);
  memset(guarded, '!', sizeof(guarded));
  browse_name(&b, lib, guarded, 3);
  TEST_ASSERT_EQUAL_STRING("Ma", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[3]);

  enter(ARTISTS);
  enter(RADIOHEAD);
  memset(guarded, '!', sizeof(guarded));
  browse_name(&b, lib, guarded, 5);
  TEST_ASSERT_EQUAL_STRING("Radi", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[5]);
  memset(guarded, '!', sizeof(guarded));
  browse_item(&b, lib, 0, guarded, 5);
  TEST_ASSERT_EQUAL_STRING("All ", guarded);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[5]);
  memset(guarded, '!', sizeof(guarded));
  browse_name(&b, lib, guarded, 0);
  TEST_ASSERT_EQUAL_CHAR('!', guarded[0]);
  browse_name(&b, lib, NULL, 5);
}

// --- The stack ---------------------------------------------------------------

void test_the_deepest_path_fits(void) {
  // Main > Artists > an artist > an album, and > All songs: four levels.
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  TEST_ASSERT_EQUAL_INT(3, b.depth);
  TEST_ASSERT_LESS_THAN_INT(BROWSE_DEPTH_MAX, b.depth);
  // A track is a leaf: selecting it, any number of times, goes no deeper.
  for (int i = 0; i < 10; i++) {
    act((uint32_t)i % 3, BROWSE_PLAY);
  }
  TEST_ASSERT_EQUAL_INT(3, b.depth);
  TEST_ASSERT_TRUE(browse_back(&b));
  enter(0);
  TEST_ASSERT_EQUAL_INT(3, b.depth);
  for (int i = 0; i < 10; i++) {
    act((uint32_t)i % 5, BROWSE_PLAY);
  }
  TEST_ASSERT_EQUAL_INT(3, b.depth);

  // Every other way down is shorter.
  browse_library_changed(&b);
  enter(ALBUMS);
  enter(4);
  TEST_ASSERT_EQUAL_INT(2, b.depth);
  act(0, BROWSE_PLAY);
  browse_library_changed(&b);
  enter(SONGS);
  act(0, BROWSE_PLAY);
  TEST_ASSERT_EQUAL_INT(1, b.depth);
  browse_library_changed(&b);
  enter(SETTINGS);
  act(0, BROWSE_RESCAN);
  TEST_ASSERT_EQUAL_INT(1, b.depth);

  // As many backs as it takes, and one more.
  browse_library_changed(&b);
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_FALSE(browse_back(&b));
  TEST_ASSERT_EQUAL_INT(0, b.depth);
}

// Extra: the tree cannot fill the stack, but if the last level were ever
// reached a level that opens another must refuse rather than write past it.
void test_a_full_stack_refuses_to_go_deeper(void) {
  enter(ARTISTS);
  b.levels[BROWSE_DEPTH_MAX - 1] = b.levels[1];
  b.depth = BROWSE_DEPTH_MAX - 1;
  browse_t before = b;
  browse_menu(&b)->cursor = RADIOHEAD;
  before.levels[BROWSE_DEPTH_MAX - 1].menu.cursor = RADIOHEAD;
  browse_result_t r = browse_select(&b, lib);
  TEST_ASSERT_EQUAL_INT(BROWSE_NOTHING, r.action);
  TEST_ASSERT_EQUAL_MEMORY(&before, &b, sizeof(b));
}

// Extra: a stack that was never set up, or was overwritten, must not send
// any call outside it.
void test_a_damaged_stack_is_harmless(void) {
  static const int DEPTHS[] = {-1, BROWSE_DEPTH_MAX, 1000, INT32_MIN,
                               INT32_MAX};
  char text[32];
  for (size_t i = 0; i < sizeof(DEPTHS) / sizeof(DEPTHS[0]); i++) {
    browse_init(&b);
    enter(ARTISTS);
    b.depth = DEPTHS[i];
    TEST_ASSERT_NOT_NULL(browse_menu(&b));
    browse_name(&b, lib, text, sizeof(text));
    browse_item(&b, lib, 0, text, sizeof(text));
    (void)browse_is_alphabetical(&b);
    (void)browse_group(&b, lib, 0);
    browse_refresh(&b, lib);
    (void)browse_select(&b, lib);
    (void)browse_back(&b);
    TEST_ASSERT_TRUE(b.depth >= 0 && b.depth < BROWSE_DEPTH_MAX);
    browse_library_changed(&b);
    assert_at(BROWSE_MAIN, 0, 0);
    assert_items(MAIN_ITEMS, 5);
  }

  // A screen that is not one of the eight is an empty list with no name.
  browse_init(&b);
  b.levels[0].screen = (browse_screen_t)77;
  browse_refresh(&b, lib);
  TEST_ASSERT_EQUAL_UINT32(0, browse_menu(&b)->count);
  TEST_ASSERT_EQUAL_STRING("", name());
  TEST_ASSERT_EQUAL_STRING("", item(0));
  TEST_ASSERT_FALSE(browse_is_alphabetical(&b));
  TEST_ASSERT_EQUAL_INT(BROWSE_NOTHING, browse_select(&b, lib).action);

  // All zeros, never initialised: the main menu with nothing counted yet.
  memset(&b, 0, sizeof(b));
  TEST_ASSERT_EQUAL_INT(BROWSE_NOTHING, browse_select(&b, lib).action);
  browse_refresh(&b, lib);
  assert_items(MAIN_ITEMS, 5);

  // No stack at all.
  browse_init(NULL);
  browse_refresh(NULL, lib);
  browse_library_changed(NULL);
  TEST_ASSERT_NULL(browse_menu(NULL));
  browse_name(NULL, lib, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("", text);
  browse_item(NULL, lib, 0, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("", text);
  TEST_ASSERT_FALSE(browse_is_alphabetical(NULL));
  TEST_ASSERT_EQUAL_CHAR('#', browse_group(NULL, lib, 0));
  TEST_ASSERT_EQUAL_INT(BROWSE_NOTHING, browse_select(NULL, lib).action);
  TEST_ASSERT_FALSE(browse_back(NULL));
}

// Extra: the stack holds numbers only. Move the whole index to other memory,
// wipe where it was, and everything on the stack still means the same.
void test_the_stack_holds_nothing_that_points_into_the_index(void) {
  enter(ARTISTS);
  enter(RADIOHEAD);
  enter(1);
  browse_menu(&b)->cursor = 2;
  // Use every query once at the old place first.
  TEST_ASSERT_EQUAL_STRING("Kid A", name());
  TEST_ASSERT_EQUAL_STRING("Motion Picture Soundtrack", item(2));

  uint32_t bytes = library_bytes(lib);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(other_words), bytes);
  memcpy(other_words, arena_words, bytes);
  memset(arena_words, 0xDD, sizeof(arena_words));
  lib = (const library_t *)other_words;

  browse_refresh(&b, lib);
  TEST_ASSERT_EQUAL_STRING("Kid A", name());
  assert_items(KID_A_TITLES, 3);
  assert_plays(2, TRACKLIST_ALBUM, KID_A, "Motion Picture Soundtrack");
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_EQUAL_STRING("Radiohead", name());
  TEST_ASSERT_EQUAL_STRING("#KO", groups());
  enter(0);
  assert_items(RADIOHEAD_TITLES, 5);
  TEST_ASSERT_TRUE(browse_back(&b));
  TEST_ASSERT_TRUE(browse_back(&b));
  assert_items(ARTIST_NAMES, 6);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_at_the_main_menu);
  RUN_TEST(test_main_menu_items_and_actions);
  RUN_TEST(test_settings_items_and_actions);
  RUN_TEST(test_artists_are_listed_a_to_z);
  RUN_TEST(test_an_artist_lists_all_songs_then_its_albums);
  RUN_TEST(test_all_songs_of_an_artist);
  RUN_TEST(test_album_tracks_are_in_album_order);
  RUN_TEST(test_albums_and_songs_lists);
  RUN_TEST(test_selecting_a_track_asks_to_play_its_list_from_that_position);
  RUN_TEST(test_back_returns_to_the_same_cursor);
  RUN_TEST(test_back_at_the_main_menu_is_refused);
  RUN_TEST(test_names_for_the_top_row);
  RUN_TEST(test_alphabetical_levels);
  RUN_TEST(test_groups_follow_the_items);
  RUN_TEST(test_levels_that_are_not_alphabetical_have_one_group);
  RUN_TEST(test_an_artists_number_album_shares_the_first_group);
  RUN_TEST(test_null_library_leaves_main_and_settings_working);
  RUN_TEST(test_null_library_under_a_level_that_was_open);
  RUN_TEST(test_library_changed_returns_to_main);
  RUN_TEST(test_refresh_after_a_smaller_library_clamps_the_cursor);
  RUN_TEST(test_select_past_the_count_on_show_does_nothing);
  RUN_TEST(test_long_item_text_is_cut_to_the_buffer);
  RUN_TEST(test_the_deepest_path_fits);
  RUN_TEST(test_a_full_stack_refuses_to_go_deeper);
  RUN_TEST(test_a_damaged_stack_is_harmless);
  RUN_TEST(test_the_stack_holds_nothing_that_points_into_the_index);
  return UNITY_END();
}
