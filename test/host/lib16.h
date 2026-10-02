#pragma once

// The 16 tracks the scan finds in fixtures/lib (see its README.md), typed in
// as the scan hands them to the index: display text already ASCII, untagged
// files under their file names in "Unknown Artist" / "Unknown Album". Shared
// by the tests of the modules that read the index, which need a library and
// not a file system.
//
// What the index makes of them (test_libscan.c checks the same of the real
// tree):
//
//   artists  0 Bjork  1 Deep Artist  2 Queen  3 Radiohead  4 Unknown Artist
//            5 Various Artists
//   albums   0 Homogenic  1 Deep Album  2 Greatest Hits (Queen)  3 Kid A
//            4 OK Computer  5 Unknown Album  6 Greatest Hits (Various)
//   tracks   0 Hunter  1 Joga  2 Deep Track  3 Bohemian Rhapsody
//            4 Everything In Its Right Place  5 Kid A
//            6 Motion Picture Soundtrack  7 Airbag  8 Paranoid Android
//            9 also untagged  10 empty  11 text  12 untagged song
//            13 Alpha  14 Beta  15 Gamma

#include "library.h"
#include "tags.h"
#include "unity.h"
#include <stddef.h>
#include <stdint.h>

#define LIB16_TRACKS 16
// The first five rows are Radiohead's: a small library of one artist, two
// albums and five tracks.
#define LIB16_RADIOHEAD_ONLY 5

#define LIB16_ARENA_BYTES 65536u

#define LIB16_ROW(path_, title_, artist_, album_, group_, disc_, no_, ms_,     \
                  format_)                                                     \
  {                                                                            \
    .path = (path_), .title = (title_), .artist = (artist_),                   \
    .album = (album_), .group_artist = (group_), .track_no = (no_),            \
    .disc_no = (disc_), .duration_ms = (ms_), .format = (format_)              \
  }

static const library_track_in_t LIB16[LIB16_TRACKS] = {
    LIB16_ROW("Radiohead/Kid A/01 Everything.flac",
              "Everything In Its Right Place", "Radiohead", "Kid A",
              "Radiohead", 1, 1, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Radiohead/Kid A/02 Kid A.flac", "Kid A", "Radiohead", "Kid A",
              "Radiohead", 1, 2, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Radiohead/Kid A/10 Motion.flac", "Motion Picture Soundtrack",
              "Radiohead", "Kid A", "Radiohead", 1, 10, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Radiohead/OK Computer/01 Airbag.mp3", "Airbag", "Radiohead",
              "OK Computer", "Radiohead", 0, 1, 131, TAGS_FORMAT_MP3),
    LIB16_ROW("Radiohead/OK Computer/02 Paranoid.mp3", "Paranoid Android",
              "radiohead", "OK Computer", "radiohead", 0, 2, 131,
              TAGS_FORMAT_MP3),
    LIB16_ROW("Bjork/Homogenic/01 Hunter.flac", "Hunter", "Bjork", "Homogenic",
              "Bjork", 0, 1, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Bjork/Homogenic/02 Joga.flac", "Joga", "Bjork", "Homogenic",
              "Bjork", 0, 2, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Various/Hits/01 a.flac", "Alpha", "The Alphas", "Greatest Hits",
              "Various Artists", 1, 1, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Various/Hits/02 b.flac", "Beta", "The Betas", "Greatest Hits",
              "Various Artists", 1, 2, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Various/Hits/01 c.flac", "Gamma", "The Gammas", "Greatest Hits",
              "Various Artists", 2, 1, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("Queen/Hits/01 Bohemian.mp3", "Bohemian Rhapsody", "Queen",
              "Greatest Hits", "Queen", 0, 1, 131, TAGS_FORMAT_MP3),
    LIB16_ROW("loose/untagged song.flac", "untagged song", "Unknown Artist",
              "Unknown Album", "Unknown Artist", 0, 0, 100, TAGS_FORMAT_FLAC),
    LIB16_ROW("loose/also untagged.mp3", "also untagged", "Unknown Artist",
              "Unknown Album", "Unknown Artist", 0, 0, 131, TAGS_FORMAT_MP3),
    LIB16_ROW("loose/empty.flac", "empty", "Unknown Artist", "Unknown Album",
              "Unknown Artist", 0, 0, 0, TAGS_FORMAT_FLAC),
    LIB16_ROW("loose/text.mp3", "text", "Unknown Artist", "Unknown Album",
              "Unknown Artist", 0, 0, 0, TAGS_FORMAT_MP3),
    LIB16_ROW("deep/a/b/c/d/Deep Track.flac", "Deep Track", "Deep Artist",
              "Deep Album", "Deep Artist", 0, 1, 100, TAGS_FORMAT_FLAC),
};

// Build an index of `n` tracks in `arena` (LIB16_ARENA_BYTES of it, aligned
// for 32-bit words).
static const library_t *lib16_build_from(void *arena,
                                         const library_track_in_t *tracks,
                                         size_t n) {
  library_builder_t *b = library_builder_begin(arena, LIB16_ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, &tracks[i]));
  }
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(n, library_track_count(lib));
  return lib;
}

// The first `n` of the 16.
static const library_t *lib16_build(void *arena, size_t n) {
  return lib16_build_from(arena, LIB16, n);
}

// The title of a track, "" if there is no such track.
static const char *lib16_title(const library_t *lib, uint32_t track) {
  library_track_t t;
  library_track(lib, track, &t);
  return t.title;
}
