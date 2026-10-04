#pragma once

#include "library.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Playlists are M3U files: one track per line. This module reads a file into
// the tracks of one library index, and copies a file with one edit applied.
// Both stream through a work area the caller provides, in large blocks; the
// rules for lines and blocks are in the spec, section 8.

#define PLAYLIST_ENTRIES_MAX 1000
#define PLAYLIST_MISSING UINT16_MAX // an entry the index does not have
#define PLAYLIST_LINE_MAX 1024      // longest line, end of line excluded
#define PLAYLIST_PATH_MAX 256       // a path from the card root, terminated

// Scratch space for reading and editing: allocate one (PSRAM on the device)
// and pass it to every call; the module keeps nothing in it between calls.
#define PLAYLIST_WORK_BYTES (16 * 1024)
typedef struct {
  _Alignas(8) unsigned char bytes[PLAYLIST_WORK_BYTES];
} playlist_work_t;

typedef enum {
  PLAYLIST_OK,
  PLAYLIST_TRUNCATED, // read: more than PLAYLIST_ENTRIES_MAX entries; the
                      // first PLAYLIST_ENTRIES_MAX were read
  PLAYLIST_FULL,      // edit: the result would have more than
                      // PLAYLIST_ENTRIES_MAX entries; nothing was written
  PLAYLIST_NO_ENTRY,  // edit: the entry named does not exist, or cannot
                      // move that way; nothing was written
  PLAYLIST_BAD_FILE,  // a line longer than PLAYLIST_LINE_MAX
  PLAYLIST_IO_ERROR,  // a read or a write failed
} playlist_result_t;

// The paths from the card root that an M3U line may mean, most likely first:
// 0 (a comment, an empty line, a URL that is not file://, a path that goes
// above the card root), 1 (an absolute path) or 2 (a relative one: from the
// card root, then from "playlists/"). `line` excludes the end of line.
uint32_t playlist_line_paths(const char *line, size_t len,
                             char paths[2][PLAYLIST_PATH_MAX]);

// Read a playlist: the track of each entry in file order, PLAYLIST_MISSING
// for one that matches nothing in `lib`. `tracks` holds `max` entries
// (normally PLAYLIST_ENTRIES_MAX).
playlist_result_t playlist_read(FILE *in, const library_t *lib,
                                uint16_t *tracks, uint32_t max,
                                uint32_t *count, playlist_work_t *work);

typedef enum {
  PLAYLIST_EDIT_APPEND,    // add `tracks` at the end
  PLAYLIST_EDIT_REMOVE,    // remove entry `entry`
  PLAYLIST_EDIT_MOVE_UP,   // swap entry `entry` with the one before
  PLAYLIST_EDIT_MOVE_DOWN, // swap entry `entry` with the one after
} playlist_edit_kind_t;

typedef struct {
  playlist_edit_kind_t kind;
  uint32_t entry;         // REMOVE, MOVE_UP, MOVE_DOWN: from 0, file order
  const uint16_t *tracks; // APPEND: track numbers of `lib`
  uint32_t count;
} playlist_edit_t;

// Write to `out` the playlist `in` holds with `edit` applied. `in` may be
// NULL: an empty playlist (so APPEND with in == NULL makes a new file).
// Appended tracks are written as "../" and their path in `lib`, one per
// line, LF. On any result but PLAYLIST_OK what was written to `out` must be
// thrown away.
playlist_result_t playlist_edit(FILE *in, FILE *out, const library_t *lib,
                                const playlist_edit_t *edit,
                                playlist_work_t *work);
