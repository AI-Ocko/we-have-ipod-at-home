#include "play_queue.h"

#include "crc32.h"

#include <string.h>

// Design notes
//
// TWO ORDERS. `original` is the order tracks were put in; `play` is the order
// they play in. Both always hold the same `count` tracks. While shuffle is
// off they are the same order, entry for entry, and an edit is made to both
// at the same place; so turning shuffle on needs no copy, and turning it off
// is one copy back. The position is a place in `play`.
//
// WHAT THINGS COST (n is the number of tracks in the queue, at most 8,192 on
// the device, 2 bytes each)
//
//   current, next, previous, set position, append: a few instructions.
//   load, append a list: one pass over the list, one query of the index per
//     track, and for a load one copy of n entries.
//   shuffle on: one pass, one random number and one swap per track.
//   shuffle off: one copy of n entries, and a search of the original order
//     for the current track: the queue does not store where each shuffled
//     track came from (that would be a third array), so it looks. At worst
//     n comparisons of 16-bit numbers.
//   insert next: the entries after the current track move up by one, in both
//     orders: at worst 2n entries, a memmove of 32 KB. That move is what an
//     array costs for an insert. While shuffle is on the same search as for
//     shuffle off comes first, to find the place in the original order.
//   save, restore: three writes or reads (the header and each order whole),
//     and a checksum over all of it.
//
// None of these runs per sample or per frame: each is at most once per key
// press or per track.
//
// A TRACK THAT IS IN THE QUEUE TWICE. The two orders hold track numbers and
// nothing says which entry of one is which entry of the other, so where the
// current track has to be found in the original order (shuffle off, and
// insert next while shuffled) it is found at its FIRST place there. The
// track that is current stays current either way; what differs is only
// which of its places the queue goes on from.
//
// RANDOM NUMBERS. The generator is xorshift32 (Marsaglia's 13, 17, 5): three
// shifts and three exclusive-ors, a period of 2^32 - 1, and a state that is
// never 0. The seed is not used as the state directly, because the first
// numbers xorshift32 gives from a small state are small too; it is put
// through a mixing function first, so that seeds 1, 2, 3 start far apart.
//
// A number below n is the top half of the 64-bit product of a random 32-bit
// number and n: one multiplication, no division and no loop. Each of the n
// results comes from floor(2^32 / n) of the generator's 2^32 - 1 values, give
// or take one, so their chances differ by about n parts in 2^32: two in a
// million for the largest queue, and nothing is thrown away to remove it.
// (Taking the number modulo n would be as fair and cost a division.)
//
// The shuffle is Fisher-Yates over the places after the first: for the last
// place, then the one before, and so on, swap it with a place picked at
// random from itself and those before it. Every order of those tracks is
// equally likely, whatever order they were in.
//
// THE FILE (version 1; all integers little-endian)
//
//   header, 32 bytes, 8 fields of 32 bits:
//      0 magic "DAPQ"      4 version      8 CRC-32 of bytes [12, end)
//     12 checksum of the index the track numbers belong to
//     16 count   20 position   24 shuffle (0 or 1)   28 loop (0, 1 or 2)
//   play order      `count` track numbers, 16 bits each
//   original order  `count` track numbers, 16 bits each
//
// The checksum field comes before everything it covers, so that it covers
// every field that is not checked by its value alone. An empty queue is a
// header with count 0 and position 0. The version must change if anything
// above does.

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the two orders are written to the file as they lie in memory"
#endif
#endif

#define QUEUE_MAGIC 0x51504144u // "DAPQ"
#define QUEUE_VERSION 1u

enum {
  F_MAGIC = 0,
  F_VERSION = 4,
  F_CRC = 8,
  F_INDEX = 12, // the checksum covers everything from here to the end
  F_COUNT = 16,
  F_POSITION = 20,
  F_SHUFFLE = 24,
  F_LOOP = 28,
  HEADER_BYTES = 32,
};

// Track numbers are 16 bits, as in the index.
#define MAX_TRACK UINT16_MAX

// Previous goes back only this far into a track; further in, it restarts.
#define RESTART_AFTER_MS 3000u

// --- Random numbers ----------------------------------------------------------

// The finishing step of MurmurHash3: every bit of the seed reaches every bit
// of the result. It maps 0 to 0 and nothing else to 0.
static uint32_t mix_seed(uint32_t x) {
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x != 0 ? x : 0x9E3779B9u;
}

static inline uint32_t next_random(play_queue_t *q) {
  uint32_t x = q->random;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  q->random = x;
  return x;
}

// A number from 0 to n - 1.
static inline uint32_t random_below(play_queue_t *q, uint32_t n) {
  return (uint32_t)(((uint64_t)next_random(q) * n) >> 32);
}

// --- The two orders ----------------------------------------------------------

// Make the current track the first of the play order and shuffle the rest.
// One pass.
static void shuffle_around_current(play_queue_t *q) {
  if (q->count == 0) {
    return;
  }
  uint16_t *play = q->play;
  uint16_t current = play[q->position];
  play[q->position] = play[0];
  play[0] = current;
  q->position = 0;
  // Places 1 to count - 1. Place `last` is swapped with one of the places 1
  // to `last`, itself included: `last` choices.
  for (uint32_t last = q->count - 1; last > 1; last--) {
    uint32_t other = 1 + random_below(q, last);
    uint16_t t = play[last];
    play[last] = play[other];
    play[other] = t;
  }
}

// The first place `track` has in the original order. It is always there,
// since the two orders hold the same tracks; 0 is the answer if a caller
// has broken that.
static uint32_t place_in_original(const play_queue_t *q, uint16_t track) {
  const uint16_t *original = q->original;
  for (uint32_t i = 0; i < q->count; i++) {
    if (original[i] == track) {
      return i;
    }
  }
  return 0;
}

// Put `track` at place `at` of an order of `count` tracks, moving the ones
// from there on up by one.
static void insert_at(uint16_t *order, uint32_t count, uint32_t at,
                      uint16_t track) {
  memmove(order + at + 1, order + at, (count - at) * sizeof(uint16_t));
  order[at] = track;
}

void play_queue_init(play_queue_t *q, uint16_t *play, uint16_t *original,
                     uint32_t capacity, uint32_t seed) {
  q->play = play;
  q->original = original;
  q->capacity = capacity;
  q->count = 0;
  q->position = 0;
  q->shuffle = false;
  q->loop = PLAY_LOOP_OFF;
  q->random = mix_seed(seed);
}

void play_queue_clear(play_queue_t *q) {
  q->count = 0;
  q->position = 0;
}

uint32_t play_queue_load(play_queue_t *q, const library_t *lib,
                         tracklist_t list, uint32_t start) {
  play_queue_clear(q);
  uint32_t n = tracklist_count(lib, list);
  if (start >= n) {
    start = 0;
  }
  // The part of the list that is taken: all of it, or as much as fits from
  // the chosen track on, moved back if that would run past the list's end.
  uint32_t first = 0;
  uint32_t take = n;
  if (n > q->capacity) {
    take = q->capacity;
    first = start <= n - take ? start : n - take;
  }
  if (take == 0) {
    return 0;
  }
  for (uint32_t i = 0; i < take; i++) {
    uint32_t track = tracklist_track(lib, list, first + i);
    if (track > MAX_TRACK) {
      return 0; // the list is shorter than it said: not with a sound index
    }
    q->original[i] = (uint16_t)track;
  }
  memcpy(q->play, q->original, take * sizeof(uint16_t));
  q->count = take;
  q->position = start - first;
  if (q->shuffle) {
    shuffle_around_current(q);
  }
  return take;
}

bool play_queue_insert_next(play_queue_t *q, uint32_t track) {
  if (q->count == 0) {
    return play_queue_append(q, track);
  }
  if (track > MAX_TRACK || q->count >= q->capacity) {
    return false;
  }
  uint32_t at = q->position + 1;
  // With shuffle off the two orders are the same and so is the place.
  uint32_t original_at =
      q->shuffle ? place_in_original(q, q->play[q->position]) + 1 : at;
  insert_at(q->play, q->count, at, (uint16_t)track);
  insert_at(q->original, q->count, original_at, (uint16_t)track);
  q->count++;
  return true;
}

bool play_queue_append(play_queue_t *q, uint32_t track) {
  if (track > MAX_TRACK || q->count >= q->capacity) {
    return false;
  }
  if (q->count == 0) {
    q->position = 0;
  }
  q->play[q->count] = (uint16_t)track;
  q->original[q->count] = (uint16_t)track;
  q->count++;
  return true;
}

uint32_t play_queue_append_list(play_queue_t *q, const library_t *lib,
                                tracklist_t list) {
  uint32_t n = tracklist_count(lib, list);
  if (n == 0 || n > q->capacity - q->count) {
    return 0;
  }
  // Written past the end of the queue, which grows only once all are there.
  for (uint32_t i = 0; i < n; i++) {
    uint32_t track = tracklist_track(lib, list, i);
    if (track > MAX_TRACK) {
      return 0;
    }
    q->play[q->count + i] = (uint16_t)track;
    q->original[q->count + i] = (uint16_t)track;
  }
  if (q->count == 0) {
    q->position = 0;
  }
  q->count += n;
  return n;
}

// --- Moving ------------------------------------------------------------------

uint32_t play_queue_current(const play_queue_t *q) {
  return q->count > 0 ? q->play[q->position] : PLAY_QUEUE_NONE;
}

bool play_queue_set_position(play_queue_t *q, uint32_t position) {
  if (position >= q->count) {
    return false;
  }
  q->position = position;
  return true;
}

uint32_t play_queue_next(play_queue_t *q, bool ended) {
  if (q->count == 0) {
    return PLAY_QUEUE_NONE;
  }
  if (ended && q->loop == PLAY_LOOP_ONE) {
    return q->play[q->position];
  }
  if (q->position + 1 < q->count) {
    q->position++;
  } else if (q->loop == PLAY_LOOP_ALL) {
    q->position = 0;
  } else {
    return PLAY_QUEUE_NONE;
  }
  return q->play[q->position];
}

uint32_t play_queue_previous(play_queue_t *q, uint32_t elapsed_ms) {
  if (q->count == 0) {
    return PLAY_QUEUE_NONE;
  }
  if (elapsed_ms <= RESTART_AFTER_MS) {
    if (q->position > 0) {
      q->position--;
    } else if (q->loop == PLAY_LOOP_ALL) {
      q->position = q->count - 1;
    }
  }
  return q->play[q->position];
}

void play_queue_set_shuffle(play_queue_t *q, bool on) {
  if (q->shuffle == on) {
    return;
  }
  q->shuffle = on;
  if (q->count == 0) {
    return;
  }
  if (on) {
    shuffle_around_current(q);
    return;
  }
  uint16_t current = q->play[q->position];
  memcpy(q->play, q->original, q->count * sizeof(uint16_t));
  q->position = place_in_original(q, current);
}

void play_queue_set_loop(play_queue_t *q, play_loop_t loop) {
  if (loop == PLAY_LOOP_OFF || loop == PLAY_LOOP_ALL || loop == PLAY_LOOP_ONE) {
    q->loop = loop;
  }
}

// --- Saving and restoring ----------------------------------------------------

static inline uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static inline void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

// The checksum of a file with this header and these two orders, each of
// `bytes` bytes.
static uint32_t file_crc(const uint8_t *header, const uint16_t *play,
                         const uint16_t *original, size_t bytes) {
  uint32_t crc = crc32_update(0, header + F_INDEX, HEADER_BYTES - F_INDEX);
  crc = crc32_update(crc, play, bytes);
  return crc32_update(crc, original, bytes);
}

bool play_queue_save(const play_queue_t *q, uint32_t index_checksum, FILE *f) {
  if (f == NULL) {
    return false;
  }
  size_t bytes = q->count * sizeof(uint16_t);
  uint8_t header[HEADER_BYTES];
  put32(header + F_MAGIC, QUEUE_MAGIC);
  put32(header + F_VERSION, QUEUE_VERSION);
  put32(header + F_INDEX, index_checksum);
  put32(header + F_COUNT, q->count);
  put32(header + F_POSITION, q->count > 0 ? q->position : 0);
  put32(header + F_SHUFFLE, q->shuffle ? 1 : 0);
  put32(header + F_LOOP, (uint32_t)q->loop);
  put32(header + F_CRC, file_crc(header, q->play, q->original, bytes));
  if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
    return false;
  }
  if (bytes > 0 && (fwrite(q->play, 1, bytes, f) != bytes ||
                    fwrite(q->original, 1, bytes, f) != bytes)) {
    return false;
  }
  // The flush is so that a card that is full or gone is reported here and
  // not lost in the caller's fclose.
  return fflush(f) == 0;
}

// A file is restored only if all of this holds, checked in this order:
//
//   1. the header is all there, with the right magic and version;
//   2. it was saved for this index;
//   3. its count fits the arrays, its position is on a track (or is 0 in an
//      empty queue), and shuffle and loop are values they can have;
//   4. the file holds exactly the two orders after the header: no fewer
//      bytes, and none after;
//   5. the CRC-32 of everything after the checksum field matches;
//   6. every track number in both orders is one the index has;
//   7. with shuffle off, the two orders are the same.
//
// Nothing is read into the arrays until 3 has passed, and never more than
// they hold. 1 to 5 catch a file that is damaged, cut short or for another
// index; 3, 6 and 7 are what make a file safe even if it was built wrong
// with a good checksum. That the two orders hold the same tracks while
// shuffle is on is not checked (it would take a table as large as the
// index): if they do not, shuffle off still lands on a track of the queue.
bool play_queue_restore(play_queue_t *q, uint32_t index_checksum,
                        uint32_t index_tracks, FILE *f) {
  play_queue_clear(q);
  if (f == NULL) {
    return false;
  }
  uint8_t header[HEADER_BYTES];
  if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
    return false;
  }
  uint32_t count = get32(header + F_COUNT);
  uint32_t position = get32(header + F_POSITION);
  uint32_t shuffle = get32(header + F_SHUFFLE);
  uint32_t loop = get32(header + F_LOOP);
  if (get32(header + F_MAGIC) != QUEUE_MAGIC ||
      get32(header + F_VERSION) != QUEUE_VERSION ||
      get32(header + F_INDEX) != index_checksum) {
    return false;
  }
  if (count > q->capacity || (count > 0 ? position >= count : position != 0) ||
      shuffle > 1 || loop > PLAY_LOOP_ONE) {
    return false;
  }
  size_t bytes = count * sizeof(uint16_t);
  if (bytes > 0 && (fread(q->play, 1, bytes, f) != bytes ||
                    fread(q->original, 1, bytes, f) != bytes)) {
    return false;
  }
  if (fgetc(f) != EOF || ferror(f)) {
    return false;
  }
  if (file_crc(header, q->play, q->original, bytes) != get32(header + F_CRC)) {
    return false;
  }
  for (uint32_t i = 0; i < count; i++) {
    if (q->play[i] >= index_tracks || q->original[i] >= index_tracks) {
      return false;
    }
  }
  if (shuffle == 0 && bytes > 0 && memcmp(q->play, q->original, bytes) != 0) {
    return false;
  }
  q->count = count;
  q->position = position;
  q->shuffle = shuffle != 0;
  q->loop = (play_loop_t)loop;
  return true;
}
