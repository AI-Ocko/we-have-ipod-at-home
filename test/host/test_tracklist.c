#include "tracklist.h"
#include "lib16.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

static uint32_t arena_words[LIB16_ARENA_BYTES / 4];
static const library_t *lib;

void setUp(void) { lib = lib16_build(arena_words, LIB16_TRACKS); }
void tearDown(void) {}

static tracklist_t list_of(tracklist_kind_t kind, uint32_t id) {
  tracklist_t list = {.kind = kind, .id = id};
  return list;
}

// The list is exactly these titles in this order, with nothing after them.
static void assert_titles(tracklist_t list, const char *const *titles,
                          uint32_t n) {
  TEST_ASSERT_EQUAL_UINT32(n, tracklist_count(lib, list));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t track = tracklist_track(lib, list, i);
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(LIB16_TRACKS, track, titles[i]);
    TEST_ASSERT_EQUAL_STRING(titles[i], lib16_title(lib, track));
  }
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(lib, list, n));
}

// The numbers the comment in lib16.h gives, checked once here by name so
// that the tests below can use them.
enum {
  BJORK,
  DEEP_ARTIST,
  QUEEN,
  RADIOHEAD,
  UNKNOWN_ARTIST,
  VARIOUS,
  ARTISTS
};
enum {
  HOMOGENIC,
  DEEP_ALBUM,
  HITS_QUEEN,
  KID_A,
  OK_COMPUTER,
  UNKNOWN_ALBUM,
  HITS_VARIOUS,
  ALBUMS
};

void test_the_library_is_numbered_as_expected(void) {
  TEST_ASSERT_EQUAL_UINT32(ARTISTS, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(ALBUMS, library_album_count(lib));
  TEST_ASSERT_EQUAL_STRING("Radiohead", library_artist_name(lib, RADIOHEAD));
  TEST_ASSERT_EQUAL_STRING("Various Artists",
                           library_artist_name(lib, VARIOUS));
  TEST_ASSERT_EQUAL_STRING("Kid A", library_album_name(lib, KID_A));
  TEST_ASSERT_EQUAL_STRING("Greatest Hits",
                           library_album_name(lib, HITS_QUEEN));
  TEST_ASSERT_EQUAL_UINT32(QUEEN, library_album_artist(lib, HITS_QUEEN));
  TEST_ASSERT_EQUAL_STRING("Greatest Hits",
                           library_album_name(lib, HITS_VARIOUS));
  TEST_ASSERT_EQUAL_UINT32(VARIOUS, library_album_artist(lib, HITS_VARIOUS));
  TEST_ASSERT_EQUAL_STRING("Unknown Album",
                           library_album_name(lib, UNKNOWN_ALBUM));
}

void test_none_and_null_are_empty(void) {
  TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(lib, list_of(TRACKLIST_NONE, 0)));
  TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(lib, list_of(TRACKLIST_NONE, 3)));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
                           tracklist_track(lib, list_of(TRACKLIST_NONE, 0), 0));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
                           tracklist_track(lib, list_of(TRACKLIST_NONE, 3), 2));

  static const tracklist_kind_t KINDS[] = {TRACKLIST_NONE, TRACKLIST_ALBUM,
                                           TRACKLIST_ARTIST, TRACKLIST_SONGS};
  for (size_t k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); k++) {
    for (uint32_t id = 0; id < 3; id++) {
      tracklist_t list = list_of(KINDS[k], id);
      TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(NULL, list));
      TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(NULL, list, 0));
      TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(NULL, list, 1));
    }
  }
}

void test_album_tracks_in_album_order(void) {
  // Track 10 after track 2: by number, not by text.
  static const char *const KID_A_TITLES[] = {"Everything In Its Right Place",
                                             "Kid A",
                                             "Motion Picture Soundtrack"};
  assert_titles(list_of(TRACKLIST_ALBUM, KID_A), KID_A_TITLES, 3);
  // Disc before track: the file names are 01 a, 02 b, 01 c.
  static const char *const HITS[] = {"Alpha", "Beta", "Gamma"};
  assert_titles(list_of(TRACKLIST_ALBUM, HITS_VARIOUS), HITS, 3);
  // The other album of the same name is its own list.
  static const char *const QUEEN_HITS[] = {"Bohemian Rhapsody"};
  assert_titles(list_of(TRACKLIST_ALBUM, HITS_QUEEN), QUEEN_HITS, 1);
  // No numbers at all: by file name.
  static const char *const LOOSE[] = {"also untagged", "empty", "text",
                                      "untagged song"};
  assert_titles(list_of(TRACKLIST_ALBUM, UNKNOWN_ALBUM), LOOSE, 4);

  // The positions are consecutive track numbers.
  tracklist_t kid_a = list_of(TRACKLIST_ALBUM, KID_A);
  uint32_t first = tracklist_track(lib, kid_a, 0);
  TEST_ASSERT_EQUAL_UINT32(first + 1, tracklist_track(lib, kid_a, 1));
  TEST_ASSERT_EQUAL_UINT32(first + 2, tracklist_track(lib, kid_a, 2));
}

void test_artist_tracks_are_all_its_albums_in_order(void) {
  // Kid A, then OK Computer, each in its own order.
  static const char *const RADIOHEAD_TITLES[] = {
      "Everything In Its Right Place", "Kid A", "Motion Picture Soundtrack",
      "Airbag", "Paranoid Android"};
  assert_titles(list_of(TRACKLIST_ARTIST, RADIOHEAD), RADIOHEAD_TITLES, 5);
  static const char *const BJORK_TITLES[] = {"Hunter", "Joga"};
  assert_titles(list_of(TRACKLIST_ARTIST, BJORK), BJORK_TITLES, 2);
  // The artist the album is filed under, not the tracks' own artists.
  static const char *const VARIOUS_TITLES[] = {"Alpha", "Beta", "Gamma"};
  assert_titles(list_of(TRACKLIST_ARTIST, VARIOUS), VARIOUS_TITLES, 3);
  static const char *const QUEEN_TITLES[] = {"Bohemian Rhapsody"};
  assert_titles(list_of(TRACKLIST_ARTIST, QUEEN), QUEEN_TITLES, 1);
}

void test_songs_are_every_track_by_title(void) {
  static const char *const TITLES[LIB16_TRACKS] = {
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
  assert_titles(list_of(TRACKLIST_SONGS, 0), TITLES, LIB16_TRACKS);
  // The id means nothing for this kind.
  assert_titles(list_of(TRACKLIST_SONGS, 99), TITLES, LIB16_TRACKS);
}

void test_out_of_range_ids_and_positions(void) {
  static const uint32_t BAD_IDS[] = {ALBUMS, ALBUMS + 1, 65535, 65536,
                                     UINT32_MAX};
  for (size_t i = 0; i < sizeof(BAD_IDS) / sizeof(BAD_IDS[0]); i++) {
    tracklist_t album = list_of(TRACKLIST_ALBUM, BAD_IDS[i]);
    TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(lib, album));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(lib, album, 0));
    tracklist_t artist = list_of(TRACKLIST_ARTIST, BAD_IDS[i]);
    TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(lib, artist));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(lib, artist, 0));
  }
  // The first artist number that is not an artist is still an album number.
  TEST_ASSERT_EQUAL_UINT32(0,
                           tracklist_count(lib, list_of(TRACKLIST_ARTIST, 6)));
  TEST_ASSERT_EQUAL_UINT32(3,
                           tracklist_count(lib, list_of(TRACKLIST_ALBUM, 6)));

  static const uint32_t BAD_POSITIONS[] = {3, 4, 16, 65535, UINT32_MAX - 1,
                                           UINT32_MAX};
  for (size_t i = 0; i < sizeof(BAD_POSITIONS) / sizeof(BAD_POSITIONS[0]);
       i++) {
    uint32_t at = BAD_POSITIONS[i];
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX, tracklist_track(lib, list_of(TRACKLIST_ALBUM, KID_A), at));
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX,
        tracklist_track(lib, list_of(TRACKLIST_ARTIST, VARIOUS), at));
    if (at >= LIB16_TRACKS) {
      TEST_ASSERT_EQUAL_UINT32(
          UINT32_MAX, tracklist_track(lib, list_of(TRACKLIST_SONGS, 0), at));
    }
  }
  // The last album and the last artist end where the tracks end, not at a
  // next row.
  TEST_ASSERT_EQUAL_UINT32(
      15, tracklist_track(lib, list_of(TRACKLIST_ALBUM, HITS_VARIOUS), 2));
  TEST_ASSERT_EQUAL_UINT32(
      15, tracklist_track(lib, list_of(TRACKLIST_ARTIST, VARIOUS), 2));

  // A kind that is not one of the four.
  tracklist_t odd = list_of((tracklist_kind_t)99, 0);
  TEST_ASSERT_EQUAL_UINT32(0, tracklist_count(lib, odd));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(lib, odd, 0));
}

// Extra: count and track agree for every list the library has: exactly
// `count` positions give a track, and every track of the library is in the
// albums once and in the artists once.
void test_every_list_has_exactly_count_tracks(void) {
  uint32_t in_albums = 0;
  for (uint32_t album = 0; album < ALBUMS; album++) {
    tracklist_t list = list_of(TRACKLIST_ALBUM, album);
    uint32_t n = tracklist_count(lib, list);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n);
    for (uint32_t i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_UINT32(in_albums + i, tracklist_track(lib, list, i));
    }
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tracklist_track(lib, list, n));
    in_albums += n;
  }
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, in_albums);

  uint32_t in_artists = 0;
  for (uint32_t artist = 0; artist < ARTISTS; artist++) {
    tracklist_t list = list_of(TRACKLIST_ARTIST, artist);
    uint32_t n = tracklist_count(lib, list);
    for (uint32_t i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_UINT32(in_artists + i, tracklist_track(lib, list, i));
    }
    in_artists += n;
  }
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, in_artists);

  // Songs is each track once.
  uint32_t seen = 0;
  for (uint32_t i = 0; i < LIB16_TRACKS; i++) {
    uint32_t track = tracklist_track(lib, list_of(TRACKLIST_SONGS, 0), i);
    TEST_ASSERT_LESS_THAN_UINT32(LIB16_TRACKS, track);
    TEST_ASSERT_EQUAL_UINT32(0, seen & (1u << track));
    seen |= 1u << track;
  }
  TEST_ASSERT_EQUAL_HEX32(0xFFFFu, seen);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_the_library_is_numbered_as_expected);
  RUN_TEST(test_none_and_null_are_empty);
  RUN_TEST(test_album_tracks_in_album_order);
  RUN_TEST(test_artist_tracks_are_all_its_albums_in_order);
  RUN_TEST(test_songs_are_every_track_by_title);
  RUN_TEST(test_out_of_range_ids_and_positions);
  RUN_TEST(test_every_list_has_exactly_count_tracks);
  return UNITY_END();
}
