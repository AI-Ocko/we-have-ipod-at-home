#pragma once

#include "library.h"
#include <stdint.h>

// A list of tracks to play in order, described by what it is rather than by
// its contents: the index already holds every such list as a range.

typedef enum {
  TRACKLIST_NONE,
  TRACKLIST_ALBUM,  // id = album number: its tracks in album order
  TRACKLIST_ARTIST, // id = artist number: all the artist's tracks
  TRACKLIST_SONGS,  // every track, A to Z by title
} tracklist_kind_t;

typedef struct {
  tracklist_kind_t kind;
  uint32_t id;
} tracklist_t;

// How many tracks the list has in this index. 0 for TRACKLIST_NONE, an id
// out of range, or a NULL library.
uint32_t tracklist_count(const library_t *lib, tracklist_t list);

// The track number at position `index`, or UINT32_MAX if there is none.
uint32_t tracklist_track(const library_t *lib, tracklist_t list,
                         uint32_t index);
