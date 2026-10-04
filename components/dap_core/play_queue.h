#pragma once

#include "library.h"
#include "tracklist.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// The play queue: which tracks will play and in what order. Tracks are held
// as track numbers of one library index. The queue keeps two orders: the
// order tracks were put in, and the order they play in, which is the same
// unless shuffle is on.

#define PLAY_QUEUE_NONE UINT32_MAX

typedef enum {
  PLAY_LOOP_OFF,
  PLAY_LOOP_ALL,
  PLAY_LOOP_ONE,
} play_loop_t;

typedef struct {
  uint16_t *play;     // play order: `count` track numbers
  uint16_t *original; // the order they were added in
  uint32_t capacity;  // entries each array can hold
  uint32_t count;
  uint32_t position;  // in `play`; meaningless when count is 0
  bool shuffle;
  play_loop_t loop;
  uint32_t random;    // generator state; never 0
} play_queue_t;

// An empty queue over two caller-supplied arrays of `capacity` entries each.
// `seed` starts the random generator (any value; 0 is replaced). Shuffle and
// loop start off.
void play_queue_init(play_queue_t *q, uint16_t *play, uint16_t *original,
                     uint32_t capacity, uint32_t seed);

// Empty the queue. Shuffle and loop are settings and stay as they are.
void play_queue_clear(play_queue_t *q);

// Replace the queue with `list`, current at its position `start`. If shuffle
// is on the rest is shuffled and the chosen track is first. Returns how many
// tracks were taken (fewer than the list has if it is longer than the
// capacity; the chosen track is always among them).
//
// Of a list that is too long, the tracks taken are the chosen one and those
// after it; if those do not fill the queue, the ones before it as well, as
// many as fit. A `start` the list does not have is taken as 0. An empty list
// empties the queue.
uint32_t play_queue_load(play_queue_t *q, const library_t *lib,
                         tracklist_t list, uint32_t start);

// Replace the queue with the entries of `tracks` that are below
// `index_tracks` (the others, such as a playlist's missing entries, are left
// out), current at the one at position `start`. With shuffle on the rest is
// shuffled and the chosen track is first. Returns how many were taken; 0,
// and the queue unchanged, if `start` is not below `count` or tracks[start]
// is left out.
uint32_t play_queue_load_tracks(play_queue_t *q, const uint16_t *tracks,
                                uint32_t count, uint32_t start,
                                uint32_t index_tracks);

// Add after the current track / at the end. With an empty queue either makes
// the track current. Return how many were added: all of `list` or none
// (false / 0 if they do not fit).
//
// A track number above 65535 is no track and is not added.
bool play_queue_insert_next(play_queue_t *q, uint32_t track);
bool play_queue_append(play_queue_t *q, uint32_t track);
uint32_t play_queue_append_list(play_queue_t *q, const library_t *lib,
                                tracklist_t list);

// The current track, or PLAY_QUEUE_NONE if the queue is empty.
uint32_t play_queue_current(const play_queue_t *q);

// Make the track at `position` in the play order current. False, and nothing
// changes, if the queue has no such position.
bool play_queue_set_position(play_queue_t *q, uint32_t position);

// Move on. `ended` is true when the current track finished by itself and
// false when the user asked for the next one. Returns the track to play, or
// PLAY_QUEUE_NONE if playback should stop (the position then stays on the
// last track).
//
// Only loop all wraps from the last track to the first: with loop one a
// manual next at the last track stops, as with loop off.
uint32_t play_queue_next(play_queue_t *q, bool ended);

// Go back. `elapsed_ms` is how far into the current track playback is.
// Returns the track to play from its beginning: the current one again, or
// the one before. PLAY_QUEUE_NONE only for an empty queue.
uint32_t play_queue_previous(play_queue_t *q, uint32_t elapsed_ms);

// Setting shuffle to what it already is does nothing: on while on does not
// shuffle again.
void play_queue_set_shuffle(play_queue_t *q, bool on);
void play_queue_set_loop(play_queue_t *q, play_loop_t loop);

// Save to, or restore from, an open file. A restore succeeds only if the file
// is whole, was saved for the index whose checksum and track count are given,
// fits the arrays, and holds only track numbers that index has. On failure
// the queue is left empty. Shuffle and loop are part of what is saved.
//
// A failed restore leaves shuffle and loop as they were. The random
// generator's state is not saved.
bool play_queue_save(const play_queue_t *q, uint32_t index_checksum,
                     FILE *f);
bool play_queue_restore(play_queue_t *q, uint32_t index_checksum,
                        uint32_t index_tracks, FILE *f);
