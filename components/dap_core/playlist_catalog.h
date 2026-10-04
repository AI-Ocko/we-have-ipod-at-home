#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The playlists in /playlists/ by name, A to Z, and the rules for a name the
// device gives one.

#define PLAYLIST_CATALOG_MAX 200      // playlists listed
#define PLAYLIST_CATALOG_POOL 16384   // bytes of file names
#define PLAYLIST_NAME_MAX 16          // a name typed on the device

typedef struct {
  uint32_t count;
  bool truncated; // some playlists did not fit and are not listed
  uint32_t pool_used;
  uint16_t offset[PLAYLIST_CATALOG_MAX]; // into `pool`, A to Z by name
  char pool[PLAYLIST_CATALOG_POOL];      // file names, each terminated
} playlist_catalog_t;

void playlist_catalog_clear(playlist_catalog_t *c);

// Add a file found in the folder. False, and nothing added, if it is not a
// playlist (no .m3u or .m3u8 ending, any case; a name starting with '.'; a
// .tmp) or there is no room (which sets `truncated`).
bool playlist_catalog_add(playlist_catalog_t *c, const char *file_name);

// Put the names A to Z, ignoring the case of ASCII letters, the way the
// library sorts. Call once after the last add.
void playlist_catalog_sort(playlist_catalog_t *c);

// The file name of playlist `i`, or "" if there is none.
const char *playlist_catalog_file(const playlist_catalog_t *c, uint32_t i);

// The name shown for playlist `i`: the file name without its ending.
// Always terminated.
void playlist_catalog_name(const playlist_catalog_t *c, uint32_t i, char *out,
                           size_t out_size);

// Which playlist has `name` (compared as names are: ignoring ASCII case and
// the ending). O(log n).
bool playlist_catalog_find(const playlist_catalog_t *c, const char *name,
                           uint32_t *i);

typedef enum {
  PLAYLIST_NAME_OK,
  PLAYLIST_NAME_EMPTY,    // nothing left after trimming spaces
  PLAYLIST_NAME_TOO_LONG, // more than PLAYLIST_NAME_MAX after trimming
  PLAYLIST_NAME_BAD_CHAR, // a character that is not allowed
  PLAYLIST_NAME_BAD_END,  // ends with '.'
  PLAYLIST_NAME_BAD_START, // starts with '.'
} playlist_name_t;

// Check a typed name and write the name to use (spaces at both ends
// trimmed) to `out` (at least PLAYLIST_NAME_MAX + 1 bytes). Whether it is in
// use is playlist_catalog_find's business.
playlist_name_t playlist_name_check(const char *typed, char *out);

// "<name>.m3u", the file a new playlist gets. Always terminated.
void playlist_file_for(const char *name, char *out, size_t out_size);
