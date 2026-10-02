#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The music library index: every track with its title, artist and album,
// grouped and sorted so that each list the player shows is a contiguous
// range. It is built once by a scan, saved to the card, and loaded whole.
//
// All of it lives in one block of memory (the arena) that the caller
// supplies; nothing here allocates. After a build or a load the arena holds
// the index in the same form as the file, so loading is one read and a
// checksum.

typedef enum {
  LIBRARY_OK,
  LIBRARY_FULL,     // the arena cannot hold any more
  LIBRARY_BAD_FILE, // not an index: wrong magic, version, size or checksum
  LIBRARY_IO,       // reading or writing failed
} library_result_t;

// One track as the scan hands it over. Display text is already converted for
// the LCD; `path` is the file's path as on disk, relative to the scan root,
// with '/' separators.
typedef struct {
  const char *path;
  const char *title;
  const char *artist;       // the track's own artist, for display
  const char *album;
  const char *group_artist; // the artist the album is filed under
  uint16_t track_no;
  uint16_t disc_no;
  uint32_t duration_ms;
  uint8_t format; // tags_format_t
} library_track_in_t;

typedef struct library_builder library_builder_t; // lives inside the arena
typedef struct library library_t;                 // lives inside the arena

// Start building in `arena`. Returns NULL if the arena is too small to
// start.
library_builder_t *library_builder_begin(void *arena, size_t arena_bytes);

// Add one track. LIBRARY_FULL means it was not added and nothing more will
// fit; what was added before is intact and can still be finished.
library_result_t library_builder_add(library_builder_t *b,
                                     const library_track_in_t *track);

// Group, sort and lay the index out in its final form at the start of the
// arena. The builder is finished after this.
const library_t *library_builder_finish(library_builder_t *b);

// Save to an open file, or load one into `arena`. A loaded index is checked
// before it is trusted.
library_result_t library_save(const library_t *lib, FILE *f);
library_result_t library_load(void *arena, size_t arena_bytes, FILE *f,
                              const library_t **out);

uint32_t library_bytes(const library_t *lib); // size of the index
uint32_t library_track_count(const library_t *lib);
uint32_t library_artist_count(const library_t *lib);
uint32_t library_album_count(const library_t *lib);

// Artists are numbered in A to Z order. An artist's albums are a contiguous
// range of album numbers, A to Z within the artist; its tracks are a
// contiguous range of track numbers.
const char *library_artist_name(const library_t *lib, uint32_t artist);
void library_artist_albums(const library_t *lib, uint32_t artist,
                           uint32_t *first_album, uint32_t *count);
void library_artist_tracks(const library_t *lib, uint32_t artist,
                           uint32_t *first_track, uint32_t *count);

// Albums are numbered by artist, then name. An album's tracks are a
// contiguous range, ordered by disc, track number, then file name.
const char *library_album_name(const library_t *lib, uint32_t album);
uint32_t library_album_artist(const library_t *lib, uint32_t album);
void library_album_tracks(const library_t *lib, uint32_t album,
                          uint32_t *first_track, uint32_t *count);

// The n-th album, and the n-th song, in A to Z order over the whole library.
uint32_t library_album_by_name(const library_t *lib, uint32_t n);
uint32_t library_song_by_title(const library_t *lib, uint32_t n);

typedef struct {
  const char *title;
  const char *artist; // the track's own artist
  uint32_t album;
  uint32_t group_artist;
  uint16_t track_no;
  uint16_t disc_no;
  uint32_t duration_ms;
  uint8_t format;
} library_track_t;

// False if `track` is out of range.
bool library_track(const library_t *lib, uint32_t track, library_track_t *out);

// Write the track's path (as given to the builder) into `out`. Returns the
// length it needs, like snprintf; 0 if `track` is out of range.
size_t library_track_path(const library_t *lib, uint32_t track, char *out,
                          size_t out_size);

// Find a track by its path. O(log n).
bool library_find_path(const library_t *lib, const char *path,
                       uint32_t *track);
