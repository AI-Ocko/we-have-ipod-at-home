#include "tracklist.h"

// An album's tracks and an artist's tracks are each a run of consecutive
// track numbers in the index, and Songs is the index's own order table. So
// a list of any length is two numbers here, its length is one query, and
// its n-th track is one query and an addition: nothing is copied out of the
// index and nothing is walked.
//
// The index answers 0 tracks for a number it does not have and for a NULL
// library (library.h), which is what makes every case below safe without
// its own checks.

// The run of track numbers an album or artist list is. False for the other
// kinds.
static bool run_of(const library_t *lib, tracklist_t list, uint32_t *first,
                   uint32_t *count) {
  switch (list.kind) {
  case TRACKLIST_ALBUM:
    library_album_tracks(lib, list.id, first, count);
    return true;
  case TRACKLIST_ARTIST:
    library_artist_tracks(lib, list.id, first, count);
    return true;
  default:
    return false;
  }
}

uint32_t tracklist_count(const library_t *lib, tracklist_t list) {
  uint32_t first, count;
  if (run_of(lib, list, &first, &count)) {
    return count;
  }
  return list.kind == TRACKLIST_SONGS ? library_track_count(lib) : 0;
}

uint32_t tracklist_track(const library_t *lib, tracklist_t list,
                         uint32_t index) {
  uint32_t first, count;
  if (run_of(lib, list, &first, &count)) {
    return index < count ? first + index : UINT32_MAX;
  }
  // UINT32_MAX from the index too if `index` is past the end.
  return list.kind == TRACKLIST_SONGS ? library_song_by_title(lib, index)
                                      : UINT32_MAX;
}
