#include "lib16.h"
#include "play_queue.h"
#include "playlist.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The library of lib16.h. Its lists, as track numbers:
//
//   album Kid A         4 5 6
//   album OK Computer   7 8
//   album Unknown       9 10 11 12
//   album Hits/Various  13 14 15
//   artist Radiohead    4 5 6 7 8
//   songs, by title     7 13 9 14 3 2 10 4 15 0 1 5 6 8 11 12
enum { RADIOHEAD = 3 };
enum { KID_A = 3, OK_COMPUTER = 4, UNKNOWN_ALBUM = 5, HITS_VARIOUS = 6 };
static const uint16_t SONGS[LIB16_TRACKS] = {7,  13, 9, 14, 3, 2, 10, 4,
                                             15, 0,  1, 5,  6, 8, 11, 12};

static uint32_t arena_words[LIB16_ARENA_BYTES / 4];
static const library_t *lib;

// The queue most tests use. Tests about a full queue make their own over
// arrays of exactly the capacity, so that the sanitizer sees a write past
// the end.
#define CAPACITY 32
static uint16_t play_slots[CAPACITY];
static uint16_t original_slots[CAPACITY];
static play_queue_t q;

// The file a test has open. tearDown closes it, so a failed assertion (which
// jumps out of the test) cannot leak it and trip the leak checker.
static FILE *open_file;

void setUp(void) {
  lib = lib16_build(arena_words, LIB16_TRACKS);
  memset(play_slots, 0xEE, sizeof(play_slots));
  memset(original_slots, 0xEE, sizeof(original_slots));
  play_queue_init(&q, play_slots, original_slots, CAPACITY, 1);
  open_file = NULL;
}

void tearDown(void) {
  if (open_file != NULL) {
    fclose(open_file);
    open_file = NULL;
  }
}

static FILE *fresh_tmp(void) {
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = tmpfile();
  TEST_ASSERT_NOT_NULL(open_file);
  return open_file;
}

static tracklist_t album(uint32_t id) {
  tracklist_t list = {.kind = TRACKLIST_ALBUM, .id = id};
  return list;
}

static tracklist_t artist(uint32_t id) {
  tracklist_t list = {.kind = TRACKLIST_ARTIST, .id = id};
  return list;
}

static tracklist_t songs(void) {
  tracklist_t list = {.kind = TRACKLIST_SONGS, .id = 0};
  return list;
}

static void assert_order(const uint16_t *want, uint32_t n,
                         const uint16_t *order, uint32_t count,
                         const char *which) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(n, count, which);
  if (n > 0) {
    TEST_ASSERT_EQUAL_UINT16_ARRAY_MESSAGE(want, order, n, which);
  }
}

#define ASSERT_PLAY(queue, ...)                                                \
  do {                                                                         \
    static const uint16_t want_[] = {__VA_ARGS__};                             \
    assert_order(want_, sizeof(want_) / sizeof(want_[0]), (queue)->play,       \
                 (queue)->count, "play order");                                \
  } while (0)

#define ASSERT_ORIGINAL(queue, ...)                                            \
  do {                                                                         \
    static const uint16_t want_[] = {__VA_ARGS__};                             \
    assert_order(want_, sizeof(want_) / sizeof(want_[0]), (queue)->original,   \
                 (queue)->count, "original order");                            \
  } while (0)

// What must hold of a queue at all times: it is within its arrays, the
// position is on a track, the two orders hold the same tracks the same
// number of times, and they are the same order unless shuffle is on.
static int32_t tally[65536];

static void assert_consistent(const play_queue_t *queue) {
  TEST_ASSERT_TRUE(queue->count <= queue->capacity);
  TEST_ASSERT_NOT_EQUAL(0, queue->random);
  if (queue->count == 0) {
    TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(queue));
    return;
  }
  TEST_ASSERT_LESS_THAN_UINT32(queue->count, queue->position);
  TEST_ASSERT_EQUAL_UINT32(queue->play[queue->position],
                           play_queue_current(queue));
  if (!queue->shuffle) {
    TEST_ASSERT_EQUAL_UINT16_ARRAY(queue->original, queue->play, queue->count);
  }
  for (uint32_t i = 0; i < queue->count; i++) {
    tally[queue->play[i]]++;
    tally[queue->original[i]]--;
  }
  bool same = true;
  for (uint32_t i = 0; i < queue->count; i++) {
    same = same && tally[queue->play[i]] == 0 && tally[queue->original[i]] == 0;
  }
  for (uint32_t i = 0; i < queue->count; i++) {
    tally[queue->play[i]] = 0;
    tally[queue->original[i]] = 0;
  }
  TEST_ASSERT_TRUE_MESSAGE(same, "the two orders hold different tracks");
}

// The queue is exactly the songs of lib16 in title order, however played.
static void assert_original_is_songs(const play_queue_t *queue) {
  assert_order(SONGS, LIB16_TRACKS, queue->original, queue->count,
               "original order");
}

// --- Empty -----------------------------------------------------------------

void test_empty_queue_answers_none(void) {
  static const play_loop_t LOOPS[] = {PLAY_LOOP_OFF, PLAY_LOOP_ALL,
                                      PLAY_LOOP_ONE};
  for (size_t i = 0; i < sizeof(LOOPS) / sizeof(LOOPS[0]); i++) {
    for (int shuffle = 0; shuffle < 2; shuffle++) {
      play_queue_set_loop(&q, LOOPS[i]);
      play_queue_set_shuffle(&q, shuffle != 0);
      TEST_ASSERT_EQUAL_UINT32(0, q.count);
      TEST_ASSERT_EQUAL(LOOPS[i], q.loop);
      TEST_ASSERT_EQUAL(shuffle != 0, q.shuffle);
      TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&q));
      TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, true));
      TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, false));
      TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_previous(&q, 0));
      TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_previous(&q, 9000));
      TEST_ASSERT_FALSE(play_queue_set_position(&q, 0));
      play_queue_clear(&q);
      assert_consistent(&q);
    }
  }
  // A cleared queue is an empty one, and keeps its settings.
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 1));
  play_queue_clear(&q);
  TEST_ASSERT_EQUAL_UINT32(0, q.count);
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, false));
  TEST_ASSERT_TRUE(q.shuffle);
  TEST_ASSERT_EQUAL(PLAY_LOOP_ONE, q.loop);
}

// --- Loading ---------------------------------------------------------------

void test_load_from_an_album_starts_at_the_chosen_track(void) {
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 1));
  ASSERT_PLAY(&q, 4, 5, 6);
  ASSERT_ORIGINAL(&q, 4, 5, 6);
  TEST_ASSERT_EQUAL_UINT32(1, q.position);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_STRING("Kid A", lib16_title(lib, play_queue_current(&q)));

  // A load replaces what was there.
  TEST_ASSERT_EQUAL_UINT32(2, play_queue_load(&q, lib, album(OK_COMPUTER), 0));
  ASSERT_PLAY(&q, 7, 8);
  ASSERT_ORIGINAL(&q, 7, 8);
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&q));

  // The other kinds of list.
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_load(&q, lib, artist(RADIOHEAD), 4));
  ASSERT_PLAY(&q, 4, 5, 6, 7, 8);
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, play_queue_load(&q, lib, songs(), 4));
  assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  assert_original_is_songs(&q);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_current(&q));
  assert_consistent(&q);

  // A position the list does not have starts at its first track.
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 3));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(3,
                           play_queue_load(&q, lib, album(KID_A), UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));

  // A list with nothing in it empties the queue.
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_load(&q, lib, album(99), 0));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 0));
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_load(&q, NULL, album(KID_A), 0));
  TEST_ASSERT_EQUAL_UINT32(0, q.count);
  tracklist_t none = {.kind = TRACKLIST_NONE, .id = 0};
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 0));
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_load(&q, lib, none, 0));
  TEST_ASSERT_EQUAL_UINT32(0, q.count);
}

void test_load_takes_what_fits_and_keeps_the_chosen_track(void) {
  uint16_t play[4], original[4];
  play_queue_t small;
  play_queue_init(&small, play, original, 4, 1);

  // From the chosen track on.
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 2));
  ASSERT_PLAY(&small, 9, 14, 3, 2);
  ASSERT_ORIGINAL(&small, 9, 14, 3, 2);
  TEST_ASSERT_EQUAL_UINT32(0, small.position);
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_current(&small));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 0));
  ASSERT_PLAY(&small, 7, 13, 9, 14);
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&small));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 12));
  ASSERT_PLAY(&small, 6, 8, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(0, small.position);

  // Near the end of the list there are not enough after it: the ones before
  // it fill the queue.
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 14));
  ASSERT_PLAY(&small, 6, 8, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(2, small.position);
  TEST_ASSERT_EQUAL_UINT32(11, play_queue_current(&small));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 15));
  ASSERT_PLAY(&small, 6, 8, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(12, play_queue_current(&small));
  assert_consistent(&small);

  // The same with shuffle on: the chosen track first, the same four tracks.
  play_queue_set_shuffle(&small, true);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&small, lib, songs(), 14));
  ASSERT_ORIGINAL(&small, 6, 8, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(0, small.position);
  TEST_ASSERT_EQUAL_UINT32(11, play_queue_current(&small));
  assert_consistent(&small);
  play_queue_set_shuffle(&small, false);

  // A list of exactly the capacity, and a shorter one.
  TEST_ASSERT_EQUAL_UINT32(
      4, play_queue_load(&small, lib, album(UNKNOWN_ALBUM), 3));
  ASSERT_PLAY(&small, 9, 10, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(12, play_queue_current(&small));
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&small, lib, album(KID_A), 2));
  ASSERT_PLAY(&small, 4, 5, 6);

  // A queue that can hold nothing takes nothing.
  play_queue_t none;
  play_queue_init(&none, play, original, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_load(&none, lib, songs(), 2));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&none));
}

// --- Loading from an array ---------------------------------------------------

#define M PLAYLIST_MISSING

// The queue is exactly `n` tracks in both orders with `current` current.
static void assert_queue(const play_queue_t *queue, const uint16_t *want,
                         uint32_t n, uint32_t position) {
  assert_order(want, n, queue->play, queue->count, "play order");
  assert_order(want, n, queue->original, queue->count, "original order");
  TEST_ASSERT_EQUAL_UINT32(position, queue->position);
  assert_consistent(queue);
}

void test_load_tracks_skips_missing_and_starts_at_the_chosen_one(void) {
  // 99 and 16 are not tracks of a 16-track index; M is a missing entry.
  static const uint16_t ENTRIES[] = {4, M, 7, 99, 5, M, 12, 16};
  static const uint16_t TAKEN[] = {4, 7, 5, 12};
  TEST_ASSERT_EQUAL_UINT32(
      4, play_queue_load_tracks(&q, ENTRIES, 8, 2, LIB16_TRACKS));
  assert_queue(&q, TAKEN, 4, 1);
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(
      4, play_queue_load_tracks(&q, ENTRIES, 8, 0, LIB16_TRACKS));
  assert_queue(&q, TAKEN, 4, 0);
  TEST_ASSERT_EQUAL_UINT32(
      4, play_queue_load_tracks(&q, ENTRIES, 8, 4, LIB16_TRACKS));
  assert_queue(&q, TAKEN, 4, 2);
  TEST_ASSERT_EQUAL_UINT32(
      4, play_queue_load_tracks(&q, ENTRIES, 8, 6, LIB16_TRACKS));
  assert_queue(&q, TAKEN, 4, 3);
  // A smaller index leaves out more.
  static const uint16_t BELOW_6[] = {4, 5};
  TEST_ASSERT_EQUAL_UINT32(2, play_queue_load_tracks(&q, ENTRIES, 8, 4, 6));
  assert_queue(&q, BELOW_6, 2, 1);
  // The same track twice is two entries.
  static const uint16_t TWICE[] = {3, 3, M, 3};
  TEST_ASSERT_EQUAL_UINT32(
      3, play_queue_load_tracks(&q, TWICE, 4, 3, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(2, q.position);

  // A list with nothing left out is loaded as play_queue_load loads the
  // same list, window included, from every start.
  uint16_t play[4], original[4], play2[4], original2[4];
  play_queue_t small, by_list;
  play_queue_init(&small, play, original, 4, 1);
  play_queue_init(&by_list, play2, original2, 4, 1);
  for (uint32_t start = 0; start < LIB16_TRACKS; start++) {
    TEST_ASSERT_EQUAL_UINT32(4,
                             play_queue_load_tracks(&small, SONGS, LIB16_TRACKS,
                                                    start, LIB16_TRACKS));
    TEST_ASSERT_EQUAL_UINT32(4, play_queue_load(&by_list, lib, songs(), start));
    assert_queue(&small, by_list.play, by_list.count, by_list.position);
  }

  // Longer than the capacity with entries left out: the chosen one and those
  // after it; if they do not fill the queue, those before it.
  static const uint16_t LONG[] = {M, 1, 2, M, 3, 4, 5, M};
  uint16_t p3[3], o3[3];
  play_queue_t three;
  play_queue_init(&three, p3, o3, 3, 1);
  static const uint16_t FROM_1[] = {1, 2, 3};
  static const uint16_t FROM_3[] = {3, 4, 5};
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, LONG, 8, 1, 16));
  assert_queue(&three, FROM_1, 3, 0);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, LONG, 8, 2, 16));
  static const uint16_t FROM_2[] = {2, 3, 4};
  assert_queue(&three, FROM_2, 3, 0);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, LONG, 8, 4, 16));
  assert_queue(&three, FROM_3, 3, 0);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, LONG, 8, 5, 16));
  assert_queue(&three, FROM_3, 3, 1);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, LONG, 8, 6, 16));
  assert_queue(&three, FROM_3, 3, 2);
  // Exactly the capacity once the missing are left out.
  static const uint16_t FIT[] = {M, 7, M, 8, 9, M};
  static const uint16_t FIT_TAKEN[] = {7, 8, 9};
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load_tracks(&three, FIT, 6, 4, 16));
  assert_queue(&three, FIT_TAKEN, 3, 2);
  // One place only: the chosen track alone.
  uint16_t p1[1], o1[1];
  play_queue_t one;
  play_queue_init(&one, p1, o1, 1, 1);
  static const uint16_t JUST_4[] = {4};
  TEST_ASSERT_EQUAL_UINT32(1, play_queue_load_tracks(&one, LONG, 8, 5, 16));
  assert_queue(&one, JUST_4, 1, 0);
}

void test_load_tracks_refuses_a_missing_start(void) {
  static const uint16_t ENTRIES[] = {4, M, 7, 99, 5};
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&q, lib, album(KID_A), 1));
  play_queue_t before;
  memcpy(&before, &q, sizeof(q));
  uint16_t play[CAPACITY], original[CAPACITY];
  memcpy(play, play_slots, sizeof(play));
  memcpy(original, original_slots, sizeof(original));

  static const uint32_t STARTS[] = {1, 3, 5, 6, UINT32_MAX};
  for (size_t i = 0; i < sizeof(STARTS) / sizeof(STARTS[0]); i++) {
    TEST_ASSERT_EQUAL_UINT32(
        0, play_queue_load_tracks(&q, ENTRIES, 5, STARTS[i], LIB16_TRACKS));
  }
  // An entry the index has but that is not below `index_tracks`.
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_load_tracks(&q, ENTRIES, 5, 4, 5));
  // No entries at all.
  TEST_ASSERT_EQUAL_UINT32(
      0, play_queue_load_tracks(&q, ENTRIES, 0, 0, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(
      0, play_queue_load_tracks(&q, NULL, 5, 0, LIB16_TRACKS));
  // A queue that can hold nothing.
  play_queue_t none;
  play_queue_init(&none, play, original, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(
      0, play_queue_load_tracks(&none, ENTRIES, 5, 0, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(0, none.count);

  // Unchanged, every byte.
  TEST_ASSERT_EQUAL_MEMORY(&before, &q, sizeof(q));
  TEST_ASSERT_EQUAL_UINT16_ARRAY(play, play_slots, CAPACITY);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(original, original_slots, CAPACITY);
}

void test_load_tracks_while_shuffle_is_on(void) {
  // The songs with missing entries among them.
  uint16_t entries[LIB16_TRACKS + 4];
  uint32_t n = 0;
  for (uint32_t i = 0; i < LIB16_TRACKS; i++) {
    if (i % 5 == 0) {
      entries[n++] = M;
    }
    entries[n++] = SONGS[i];
  }
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS + 4, n);
  uint32_t start = 9; // SONGS[7], after two missing entries
  TEST_ASSERT_EQUAL_UINT16(SONGS[7], entries[start]);

  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_EQUAL_UINT32(
      LIB16_TRACKS,
      play_queue_load_tracks(&q, entries, n, start, LIB16_TRACKS));
  TEST_ASSERT_TRUE(q.shuffle);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_EQUAL_UINT32(SONGS[7], play_queue_current(&q));
  assert_original_is_songs(&q);
  assert_consistent(&q);
  // The rest is shuffled: not the list's order.
  TEST_ASSERT_TRUE(memcmp(q.play + 1, SONGS + 8, 8 * sizeof(uint16_t)) != 0);

  // Shuffle off: the list without its missing entries, at the chosen track.
  play_queue_set_shuffle(&q, false);
  assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  TEST_ASSERT_EQUAL_UINT32(7, q.position);
}

#undef M

// --- Next and previous -----------------------------------------------------

void test_next_walks_the_queue(void) {
  play_queue_load(&q, lib, artist(RADIOHEAD), 0);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(4, q.position);
  // The order is not touched by walking it.
  ASSERT_PLAY(&q, 4, 5, 6, 7, 8);
  ASSERT_ORIGINAL(&q, 4, 5, 6, 7, 8);
}

void test_next_at_the_end_with_loop_off_stops_and_stays(void) {
  play_queue_load(&q, lib, album(KID_A), 2);
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, true));
    TEST_ASSERT_EQUAL_UINT32(6, play_queue_current(&q));
    TEST_ASSERT_EQUAL_UINT32(2, q.position);
    TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, false));
    TEST_ASSERT_EQUAL_UINT32(6, play_queue_current(&q));
  }
  ASSERT_PLAY(&q, 4, 5, 6);

  // A queue of one.
  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_append(&q, 9));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_current(&q));
}

void test_next_at_the_end_with_loop_all_wraps(void) {
  play_queue_set_loop(&q, PLAY_LOOP_ALL);
  play_queue_load(&q, lib, album(KID_A), 2);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));

  // A queue of one wraps to itself.
  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_append(&q, 9));
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
}

void test_loop_one_repeats_when_the_track_ends(void) {
  play_queue_set_loop(&q, PLAY_LOOP_ONE);
  play_queue_load(&q, lib, album(KID_A), 1);
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_UINT32(5, play_queue_next(&q, true));
    TEST_ASSERT_EQUAL_UINT32(1, q.position);
  }
  // At the last track too, and in a queue of one.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 2));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(2, q.position);
  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_append(&q, 9));
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_next(&q, true));
}

void test_loop_one_still_advances_on_a_manual_next(void) {
  play_queue_set_loop(&q, PLAY_LOOP_ONE);
  play_queue_load(&q, lib, album(KID_A), 0);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, false));
  // Only loop all wraps: at the last track a manual next stops and stays.
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, false));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_next(&q, true));
}

void test_previous_restarts_after_three_seconds(void) {
  static const play_loop_t LOOPS[] = {PLAY_LOOP_OFF, PLAY_LOOP_ALL,
                                      PLAY_LOOP_ONE};
  for (size_t i = 0; i < sizeof(LOOPS) / sizeof(LOOPS[0]); i++) {
    play_queue_set_loop(&q, LOOPS[i]);
    play_queue_load(&q, lib, album(KID_A), 2);
    TEST_ASSERT_EQUAL_UINT32(6, play_queue_previous(&q, 3001));
    TEST_ASSERT_EQUAL_UINT32(6, play_queue_previous(&q, 250000));
    TEST_ASSERT_EQUAL_UINT32(6, play_queue_previous(&q, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(2, q.position);
    // At the first track as well, whatever the loop mode.
    TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
    TEST_ASSERT_EQUAL_UINT32(4, play_queue_previous(&q, 3001));
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
  }
}

void test_previous_within_three_seconds_goes_back(void) {
  play_queue_load(&q, lib, artist(RADIOHEAD), 4);
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_previous(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_previous(&q, 2999));
  // Exactly three seconds is not more than three seconds.
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_previous(&q, 3000));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_previous(&q, 1));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  ASSERT_PLAY(&q, 4, 5, 6, 7, 8);
}

void test_previous_at_the_first_track_restarts_without_loop_and_wraps_with_loop_all(
    void) {
  play_queue_load(&q, lib, album(KID_A), 0);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_previous(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  play_queue_set_loop(&q, PLAY_LOOP_ONE);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_previous(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);

  play_queue_set_loop(&q, PLAY_LOOP_ALL);
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_previous(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(2, q.position);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_previous(&q, 0));
  // Far enough into the first track it restarts even with loop all.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_previous(&q, 3001));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);

  // A queue of one goes back to itself.
  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_append(&q, 9));
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_previous(&q, 0));
  play_queue_set_loop(&q, PLAY_LOOP_OFF);
  TEST_ASSERT_EQUAL_UINT32(9, play_queue_previous(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
}

void test_set_position_moves_only_within_the_queue(void) {
  play_queue_load(&q, lib, album(KID_A), 1);
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 2));
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_current(&q));
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));

  static const uint32_t BAD[] = {3, 4, CAPACITY, 65536, UINT32_MAX};
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 1));
  for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
    TEST_ASSERT_FALSE(play_queue_set_position(&q, BAD[i]));
    TEST_ASSERT_EQUAL_UINT32(1, q.position);
    TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  }
  ASSERT_PLAY(&q, 4, 5, 6);

  // The position is one in the play order.
  play_queue_set_shuffle(&q, true);
  for (uint32_t i = 0; i < 3; i++) {
    TEST_ASSERT_TRUE(play_queue_set_position(&q, i));
    TEST_ASSERT_EQUAL_UINT32(q.play[i], play_queue_current(&q));
  }
  TEST_ASSERT_FALSE(play_queue_set_position(&q, 3));
  TEST_ASSERT_EQUAL_UINT32(2, q.position);
}

// --- Adding ----------------------------------------------------------------

void test_insert_next_goes_after_the_current_track(void) {
  play_queue_load(&q, lib, album(KID_A), 1);
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 0));
  ASSERT_PLAY(&q, 4, 5, 0, 6);
  ASSERT_ORIGINAL(&q, 4, 5, 0, 6);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(1, q.position);
  // A second one goes in front of the first: it is the next to play.
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 1));
  ASSERT_PLAY(&q, 4, 5, 1, 0, 6);
  TEST_ASSERT_EQUAL_UINT32(1, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_next(&q, true));

  // After the last track, and after the first.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 4));
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 2));
  ASSERT_PLAY(&q, 4, 5, 1, 0, 6, 2);
  TEST_ASSERT_EQUAL_UINT32(6, play_queue_current(&q));
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 3));
  ASSERT_PLAY(&q, 4, 3, 5, 1, 0, 6, 2);
  ASSERT_ORIGINAL(&q, 4, 3, 5, 1, 0, 6, 2);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));
  assert_consistent(&q);
}

void test_append_goes_last(void) {
  play_queue_load(&q, lib, album(KID_A), 1);
  TEST_ASSERT_TRUE(play_queue_append(&q, 0));
  TEST_ASSERT_TRUE(play_queue_append(&q, 1));
  ASSERT_PLAY(&q, 4, 5, 6, 0, 1);
  ASSERT_ORIGINAL(&q, 4, 5, 6, 0, 1);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(1, q.position);
  TEST_ASSERT_EQUAL_UINT32(2,
                           play_queue_append_list(&q, lib, album(OK_COMPUTER)));
  ASSERT_PLAY(&q, 4, 5, 6, 0, 1, 7, 8);
  ASSERT_ORIGINAL(&q, 4, 5, 6, 0, 1, 7, 8);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  // A queue that had stopped at its end goes on into what was appended.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 6));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&q, true));
  TEST_ASSERT_TRUE(play_queue_append(&q, 2));
  TEST_ASSERT_EQUAL_UINT32(2, play_queue_next(&q, true));
}

void test_insert_and_append_into_an_empty_queue_make_it_current(void) {
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 7));
  ASSERT_PLAY(&q, 7);
  ASSERT_ORIGINAL(&q, 7);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&q));

  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_append(&q, 8));
  ASSERT_PLAY(&q, 8);
  ASSERT_ORIGINAL(&q, 8);
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_current(&q));

  play_queue_clear(&q);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_append_list(&q, lib, album(KID_A)));
  ASSERT_PLAY(&q, 4, 5, 6);
  TEST_ASSERT_EQUAL_UINT32(4, play_queue_current(&q));

  // The same with shuffle on. A position left behind by an earlier queue
  // must not matter.
  play_queue_load(&q, lib, songs(), 9);
  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 9));
  play_queue_clear(&q);
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 7));
  TEST_ASSERT_EQUAL_UINT32(7, play_queue_current(&q));
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 8));
  ASSERT_PLAY(&q, 7, 8);
  ASSERT_ORIGINAL(&q, 7, 8);
  assert_consistent(&q);

  // Numbers that cannot be a track are refused; the largest that can be is
  // not.
  play_queue_clear(&q);
  TEST_ASSERT_FALSE(play_queue_insert_next(&q, PLAY_QUEUE_NONE));
  TEST_ASSERT_FALSE(play_queue_append(&q, PLAY_QUEUE_NONE));
  TEST_ASSERT_FALSE(play_queue_insert_next(&q, 65536));
  TEST_ASSERT_FALSE(play_queue_append(&q, 65536 + 7));
  TEST_ASSERT_EQUAL_UINT32(0, q.count);
  TEST_ASSERT_TRUE(play_queue_append(&q, 65535));
  TEST_ASSERT_EQUAL_UINT32(65535, play_queue_current(&q));
}

void test_append_list_adds_all_or_nothing(void) {
  uint16_t play[8], original[8];
  play_queue_t small;
  play_queue_init(&small, play, original, 8, 1);
  play_queue_load(&small, lib, album(KID_A), 1);

  TEST_ASSERT_EQUAL_UINT32(
      2, play_queue_append_list(&small, lib, album(OK_COMPUTER)));
  ASSERT_PLAY(&small, 4, 5, 6, 7, 8);
  // Four do not fit in the three places left: none is added.
  TEST_ASSERT_EQUAL_UINT32(
      0, play_queue_append_list(&small, lib, album(UNKNOWN_ALBUM)));
  ASSERT_PLAY(&small, 4, 5, 6, 7, 8);
  ASSERT_ORIGINAL(&small, 4, 5, 6, 7, 8);
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_append_list(&small, lib, songs()));
  TEST_ASSERT_EQUAL_UINT32(5, small.count);
  // Three fit exactly.
  TEST_ASSERT_EQUAL_UINT32(
      3, play_queue_append_list(&small, lib, album(HITS_VARIOUS)));
  ASSERT_PLAY(&small, 4, 5, 6, 7, 8, 13, 14, 15);
  ASSERT_ORIGINAL(&small, 4, 5, 6, 7, 8, 13, 14, 15);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&small));
  assert_consistent(&small);

  // A list with nothing in it adds nothing.
  play_queue_load(&small, lib, album(KID_A), 1);
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_append_list(&small, lib, album(99)));
  TEST_ASSERT_EQUAL_UINT32(0,
                           play_queue_append_list(&small, NULL, album(KID_A)));
  ASSERT_PLAY(&small, 4, 5, 6);
}

void test_full_queue_refuses_and_stays_consistent(void) {
  uint16_t play[4], original[4];
  play_queue_t full;
  for (int shuffle = 0; shuffle < 2; shuffle++) {
    play_queue_init(&full, play, original, 4, 1);
    play_queue_set_shuffle(&full, shuffle != 0);
    TEST_ASSERT_EQUAL_UINT32(
        4, play_queue_load(&full, lib, album(UNKNOWN_ALBUM), 1));
    uint16_t play_before[4], original_before[4];
    memcpy(play_before, play, sizeof(play));
    memcpy(original_before, original, sizeof(original));
    uint32_t position = full.position;

    TEST_ASSERT_FALSE(play_queue_insert_next(&full, 0));
    TEST_ASSERT_FALSE(play_queue_append(&full, 0));
    TEST_ASSERT_EQUAL_UINT32(0,
                             play_queue_append_list(&full, lib, album(KID_A)));
    TEST_ASSERT_EQUAL_UINT32(0, play_queue_append_list(&full, lib, songs()));

    TEST_ASSERT_EQUAL_UINT32(4, full.count);
    TEST_ASSERT_EQUAL_UINT32(position, full.position);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(play_before, play, 4);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(original_before, original, 4);
    TEST_ASSERT_EQUAL_UINT32(10, play_queue_current(&full));
    assert_consistent(&full);

    // It still works as a queue.
    play_queue_set_shuffle(&full, !full.shuffle);
    assert_consistent(&full);
    TEST_ASSERT_EQUAL_UINT32(10, play_queue_current(&full));
    play_queue_set_loop(&full, PLAY_LOOP_ALL);
    for (int i = 0; i < 9; i++) {
      TEST_ASSERT_NOT_EQUAL(PLAY_QUEUE_NONE, play_queue_next(&full, true));
    }
    assert_consistent(&full);
  }

  // One place left: one track goes in, a second does not.
  play_queue_init(&full, play, original, 4, 1);
  play_queue_load(&full, lib, album(KID_A), 0);
  TEST_ASSERT_TRUE(play_queue_insert_next(&full, 0));
  TEST_ASSERT_FALSE(play_queue_insert_next(&full, 1));
  TEST_ASSERT_FALSE(play_queue_append(&full, 1));
  ASSERT_PLAY(&full, 4, 0, 5, 6);
  ASSERT_ORIGINAL(&full, 4, 0, 5, 6);

  // A queue that can hold nothing refuses everything.
  play_queue_t none;
  play_queue_init(&none, play, original, 0, 1);
  TEST_ASSERT_FALSE(play_queue_insert_next(&none, 0));
  TEST_ASSERT_FALSE(play_queue_append(&none, 0));
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_append_list(&none, lib, album(KID_A)));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&none));
}

// --- Shuffle ---------------------------------------------------------------

void test_shuffle_keeps_the_current_track_first_and_every_track_once(void) {
  play_queue_load(&q, lib, songs(), 5);
  TEST_ASSERT_EQUAL_UINT32(2, play_queue_current(&q));
  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_TRUE(q.shuffle);
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, q.count);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_EQUAL_UINT32(2, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT16(2, q.play[0]);
  // Every track once: the same tracks as the original order, which is as it
  // was.
  assert_consistent(&q);
  assert_original_is_songs(&q);
  // And it is a different order (with this seed, as with nearly all).
  TEST_ASSERT_TRUE(memcmp(q.play, q.original, sizeof(SONGS)) != 0);
  uint32_t moved = 0;
  for (uint32_t i = 1; i < LIB16_TRACKS; i++) {
    moved += q.play[i] != SONGS[i];
  }
  TEST_ASSERT_GREATER_THAN_UINT32(8, moved);

  // On while on does nothing: no second shuffle, and the position stays.
  uint16_t before[LIB16_TRACKS];
  memcpy(before, q.play, sizeof(before));
  uint32_t random = q.random;
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 3));
  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(before, q.play, LIB16_TRACKS);
  TEST_ASSERT_EQUAL_UINT32(3, q.position);
  TEST_ASSERT_EQUAL_UINT32(random, q.random);
}

void test_shuffle_off_restores_the_original_order_and_the_current_track(void) {
  play_queue_load(&q, lib, songs(), 5);
  play_queue_set_shuffle(&q, true);
  for (int i = 0; i < 6; i++) {
    play_queue_next(&q, true);
  }
  uint32_t current = play_queue_current(&q);
  TEST_ASSERT_NOT_EQUAL(2, current);

  play_queue_set_shuffle(&q, false);
  TEST_ASSERT_FALSE(q.shuffle);
  assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  assert_original_is_songs(&q);
  TEST_ASSERT_EQUAL_UINT32(current, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT16(current, SONGS[q.position]);
  // What follows is what follows it in the list.
  if (q.position + 1 < LIB16_TRACKS) {
    uint16_t following = SONGS[q.position + 1];
    TEST_ASSERT_EQUAL_UINT32(following, play_queue_next(&q, true));
  }

  // Off while off does nothing.
  uint32_t position = q.position;
  uint32_t random = q.random;
  play_queue_set_shuffle(&q, false);
  assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  TEST_ASSERT_EQUAL_UINT32(position, q.position);
  TEST_ASSERT_EQUAL_UINT32(random, q.random);

  // Every position comes back to itself through on and off.
  for (uint32_t i = 0; i < LIB16_TRACKS; i++) {
    TEST_ASSERT_TRUE(play_queue_set_position(&q, i));
    play_queue_set_shuffle(&q, true);
    TEST_ASSERT_EQUAL_UINT32(SONGS[i], play_queue_current(&q));
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
    play_queue_set_shuffle(&q, false);
    TEST_ASSERT_EQUAL_UINT32(i, q.position);
    assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  }
}

void test_shuffle_on_off_on_never_loses_a_track(void) {
  play_queue_load(&q, lib, songs(), 0);
  uint32_t different_orders = 0;
  uint16_t last[LIB16_TRACKS];
  memcpy(last, q.play, sizeof(last));
  for (uint32_t round = 0; round < 200; round++) {
    play_queue_set_shuffle(&q, true);
    assert_consistent(&q);
    assert_original_is_songs(&q);
    different_orders += memcmp(last, q.play, sizeof(last)) != 0;
    memcpy(last, q.play, sizeof(last));
    // Move about, so that each round starts from another track.
    for (uint32_t i = 0; i < round % 7; i++) {
      play_queue_next(&q, true);
    }
    uint32_t current = play_queue_current(&q);
    play_queue_set_shuffle(&q, false);
    assert_consistent(&q);
    assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
    TEST_ASSERT_EQUAL_UINT32(current, play_queue_current(&q));
  }
  // Each shuffle is a new one.
  TEST_ASSERT_EQUAL_UINT32(200, different_orders);
}

// Which of the 24 orders the four numbers 0..3 are in.
static uint32_t order_number(const uint32_t *p) {
  uint32_t number = 0;
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t smaller_after = 0;
    for (uint32_t j = i + 1; j < 4; j++) {
      smaller_after += p[j] < p[i];
    }
    number = number * (4 - i) + smaller_after;
  }
  return number;
}

// A queue of five, the track in the middle current, shuffled `SHUFFLES`
// times: how often each of the other four lands in each of the four places
// after the current track, and how often each of the 24 orders comes up.
//
// THE TOLERANCE. With a fair shuffle a place holds a given track one time in
// four: 6000 of 24000, with a standard deviation of sqrt(24000 * 1/4 * 3/4)
// = 67. An order comes up one time in 24: 1000, standard deviation 31. The
// test allows 5% (300, 4.5 deviations) on the places and 15% (150, 4.8
// deviations) on the orders. The mistakes it is there to catch are far
// outside that: swapping with any place instead of a place not yet settled
// makes some orders half again as likely as others, a range one short never
// leaves a track where it was (a count of 0), and seeds that are small
// numbers fed to the generator raw always pick place 0.
//
// It cannot flake: the generator is deterministic and the seeds are fixed,
// so the counts are the same numbers on every run and every machine. The
// tolerance only decides whether a changed shuffle still passes.
#define SHUFFLES 24000u
#define PLACE_TOLERANCE (SHUFFLES / 4 * 5 / 100)   // 300
#define ORDER_TOLERANCE (SHUFFLES / 24 * 15 / 100) // 150

typedef struct {
  uint32_t place[4][4]; // [track][place]
  uint32_t order[24];
} fairness_t;

static void fairness_count(fairness_t *f, const play_queue_t *queue) {
  static const uint32_t INDEX[5] = {0, 1, 9, 2, 3}; // track -> 0..3
  TEST_ASSERT_EQUAL_UINT32(5, queue->count);
  TEST_ASSERT_EQUAL_UINT32(0, queue->position);
  TEST_ASSERT_EQUAL_UINT16(2, queue->play[0]);
  uint32_t p[4];
  uint32_t seen = 0;
  for (uint32_t i = 0; i < 4; i++) {
    uint16_t track = queue->play[1 + i];
    TEST_ASSERT_TRUE(track < 5 && track != 2);
    p[i] = INDEX[track];
    seen |= 1u << p[i];
    f->place[p[i]][i]++;
  }
  TEST_ASSERT_EQUAL_HEX32(0xF, seen);
  f->order[order_number(p)]++;
}

static void fairness_check(const fairness_t *f, const char *what) {
  uint32_t worst_place = 0, worst_order = 0;
  for (uint32_t t = 0; t < 4; t++) {
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t n = f->place[t][i];
      uint32_t off = n > SHUFFLES / 4 ? n - SHUFFLES / 4 : SHUFFLES / 4 - n;
      worst_place = off > worst_place ? off : worst_place;
    }
  }
  for (uint32_t i = 0; i < 24; i++) {
    uint32_t n = f->order[i];
    uint32_t off = n > SHUFFLES / 24 ? n - SHUFFLES / 24 : SHUFFLES / 24 - n;
    worst_order = off > worst_order ? off : worst_order;
  }
  printf("play_queue fairness, %s: of %u shuffles the worst place is %u from "
         "%u (allowed %u), the worst order %u from %u (allowed %u)\n",
         what, (unsigned)SHUFFLES, (unsigned)worst_place,
         (unsigned)(SHUFFLES / 4), (unsigned)PLACE_TOLERANCE,
         (unsigned)worst_order, (unsigned)(SHUFFLES / 24),
         (unsigned)ORDER_TOLERANCE);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(PLACE_TOLERANCE, worst_place, what);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(ORDER_TOLERANCE, worst_order, what);
}

static void five_tracks_at_the_middle(play_queue_t *queue, uint32_t seed) {
  play_queue_init(queue, play_slots, original_slots, CAPACITY, seed);
  for (uint32_t track = 0; track < 5; track++) {
    TEST_ASSERT_TRUE(play_queue_append(queue, track));
  }
  TEST_ASSERT_TRUE(play_queue_set_position(queue, 2));
}

void test_shuffle_is_fair(void) {
  static fairness_t f;

  // One shuffle from each of the seeds 1, 2, 3, ...: the worst case for a
  // generator, since such seeds differ in a few low bits only.
  memset(&f, 0, sizeof(f));
  for (uint32_t seed = 1; seed <= SHUFFLES; seed++) {
    five_tracks_at_the_middle(&q, seed);
    play_queue_set_shuffle(&q, true);
    fairness_count(&f, &q);
  }
  fairness_check(&f, "one shuffle from each of 24000 seeds");

  // Seeds far apart.
  memset(&f, 0, sizeof(f));
  for (uint32_t i = 0; i < SHUFFLES; i++) {
    five_tracks_at_the_middle(&q, 0x9E3779B9u * i + 0x12345u);
    play_queue_set_shuffle(&q, true);
    fairness_count(&f, &q);
  }
  fairness_check(&f, "24000 seeds far apart");

  // One seed, shuffled again and again, as on the device.
  memset(&f, 0, sizeof(f));
  five_tracks_at_the_middle(&q, 2026);
  for (uint32_t i = 0; i < SHUFFLES; i++) {
    play_queue_set_shuffle(&q, true);
    fairness_count(&f, &q);
    play_queue_set_shuffle(&q, false);
    TEST_ASSERT_EQUAL_UINT32(2, q.position);
  }
  fairness_check(&f, "24000 shuffles in a row from one seed");

  // A longer queue, so that numbers are picked below every n up to 15: each
  // of the 15 other tracks in each of the 15 places 1600 times of 24000,
  // standard deviation 39; allowed 15% (240, 6 deviations: there are 225
  // counts here).
  static uint32_t place[LIB16_TRACKS][LIB16_TRACKS];
  memset(place, 0, sizeof(place));
  for (uint32_t seed = 1; seed <= SHUFFLES; seed++) {
    play_queue_init(&q, play_slots, original_slots, CAPACITY, seed);
    play_queue_set_shuffle(&q, true);
    TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS,
                             play_queue_load(&q, lib, songs(), 9));
    for (uint32_t i = 0; i < LIB16_TRACKS; i++) {
      place[q.play[i]][i]++;
    }
  }
  uint32_t worst = 0;
  for (uint32_t t = 1; t < LIB16_TRACKS; t++) {
    TEST_ASSERT_EQUAL_UINT32(0, place[t][0]);
    for (uint32_t i = 1; i < LIB16_TRACKS; i++) {
      uint32_t n = place[t][i];
      uint32_t off = n > SHUFFLES / 15 ? n - SHUFFLES / 15 : SHUFFLES / 15 - n;
      worst = off > worst ? off : worst;
    }
  }
  printf("play_queue fairness, 16 tracks: the worst place is %u from %u "
         "(allowed %u)\n",
         (unsigned)worst, (unsigned)(SHUFFLES / 15),
         (unsigned)(SHUFFLES / 15 * 15 / 100));
  TEST_ASSERT_EQUAL_UINT32(SHUFFLES, place[0][0]);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(SHUFFLES / 15 * 15 / 100, worst);
}

void test_shuffle_of_one_and_two_tracks(void) {
  TEST_ASSERT_TRUE(play_queue_append(&q, 9));
  for (int i = 0; i < 4; i++) {
    play_queue_set_shuffle(&q, true);
    ASSERT_PLAY(&q, 9);
    ASSERT_ORIGINAL(&q, 9);
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
    TEST_ASSERT_EQUAL_UINT32(9, play_queue_current(&q));
    play_queue_set_shuffle(&q, false);
    ASSERT_PLAY(&q, 9);
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
  }

  // Two: the current one first, so the order is forced.
  TEST_ASSERT_TRUE(play_queue_append(&q, 3));
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_TRUE(play_queue_set_position(&q, 1));
    play_queue_set_shuffle(&q, true);
    ASSERT_PLAY(&q, 3, 9);
    ASSERT_ORIGINAL(&q, 9, 3);
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
    play_queue_set_shuffle(&q, false);
    ASSERT_PLAY(&q, 9, 3);
    TEST_ASSERT_EQUAL_UINT32(1, q.position);

    TEST_ASSERT_TRUE(play_queue_set_position(&q, 0));
    play_queue_set_shuffle(&q, true);
    ASSERT_PLAY(&q, 9, 3);
    TEST_ASSERT_EQUAL_UINT32(0, q.position);
    TEST_ASSERT_EQUAL_UINT32(3, play_queue_next(&q, true));
    play_queue_set_shuffle(&q, false);
    ASSERT_PLAY(&q, 9, 3);
    TEST_ASSERT_EQUAL_UINT32(1, q.position);
  }
}

void test_load_while_shuffle_is_on_shuffles_the_rest(void) {
  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, play_queue_load(&q, lib, songs(), 7));
  TEST_ASSERT_TRUE(q.shuffle);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_EQUAL_UINT32(SONGS[7], play_queue_current(&q));
  assert_original_is_songs(&q);
  assert_consistent(&q);
  // The rest is not in list order.
  uint32_t in_list_order = 0;
  for (uint32_t i = 1; i + 1 < LIB16_TRACKS; i++) {
    uint32_t at = 0;
    while (SONGS[at] != q.play[i]) {
      at++;
    }
    in_list_order += at + 1 < LIB16_TRACKS && SONGS[at + 1] == q.play[i + 1];
  }
  TEST_ASSERT_LESS_THAN_UINT32(6, in_list_order);

  // A second load gives another order.
  uint16_t first[LIB16_TRACKS];
  memcpy(first, q.play, sizeof(first));
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, play_queue_load(&q, lib, songs(), 7));
  TEST_ASSERT_TRUE(memcmp(first, q.play, sizeof(first)) != 0);
  assert_consistent(&q);

  // Shuffle off: the list, at the chosen track.
  play_queue_set_shuffle(&q, false);
  assert_order(SONGS, LIB16_TRACKS, q.play, q.count, "play order");
  TEST_ASSERT_EQUAL_UINT32(7, q.position);
}

void test_edits_while_shuffled_survive_shuffle_off(void) {
  play_queue_load(&q, lib, album(UNKNOWN_ALBUM), 1); // 9 10 11 12, at 10
  play_queue_set_shuffle(&q, true);
  uint16_t a = q.play[1], b = q.play[2], c = q.play[3];

  // Play next: after the current track in both orders.
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 0));
  {
    uint16_t want[] = {10, 0, a, b, c};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, q.play, 5);
  }
  ASSERT_ORIGINAL(&q, 9, 10, 0, 11, 12);
  TEST_ASSERT_EQUAL_UINT32(10, play_queue_current(&q));

  // Add to queue: last in both.
  TEST_ASSERT_TRUE(play_queue_append(&q, 1));
  TEST_ASSERT_EQUAL_UINT32(2,
                           play_queue_append_list(&q, lib, album(OK_COMPUTER)));
  {
    uint16_t want[] = {10, 0, a, b, c, 1, 7, 8};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, q.play, 8);
  }
  ASSERT_ORIGINAL(&q, 9, 10, 0, 11, 12, 1, 7, 8);

  // Play next from further along: after that track, wherever it is in the
  // original order.
  TEST_ASSERT_EQUAL_UINT32(0, play_queue_next(&q, true));
  TEST_ASSERT_EQUAL_UINT32(a, play_queue_next(&q, true));
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 2));
  {
    uint16_t want[] = {10, 0, a, 2, b, c, 1, 7, 8};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, q.play, 9);
  }
  TEST_ASSERT_EQUAL_UINT32(9, q.count);
  uint32_t at = 0;
  while (q.original[at] != a) {
    at++;
  }
  TEST_ASSERT_EQUAL_UINT16(2, q.original[at + 1]);
  TEST_ASSERT_EQUAL_UINT32(a, play_queue_current(&q));
  assert_consistent(&q);

  // After the last track of the play order.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 8));
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_current(&q));
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 3));
  {
    uint16_t want[] = {10, 0, a, 2, b, c, 1, 7, 8, 3};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, q.play, 10);
  }
  assert_consistent(&q);

  // Shuffle off: the order things were put in, with every edit in its place
  // (track 2 after `a`, which is 9, 11 or 12), and the current track still
  // current.
  static const uint16_t WITHOUT_2[] = {9, 10, 0, 11, 12, 1, 7, 8, 3};
  uint16_t want[10];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 9; i++) {
    want[n++] = WITHOUT_2[i];
    if (WITHOUT_2[i] == a) {
      want[n++] = 2;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(10, n);
  play_queue_set_shuffle(&q, false);
  assert_order(want, 10, q.play, q.count, "play order");
  assert_order(want, 10, q.original, q.count, "original order");
  TEST_ASSERT_EQUAL_UINT32(8, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT16(8, want[q.position]);
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_next(&q, true));
}

void test_duplicate_tracks_are_handled(void) {
  // Track 5 twice: 4 5 6 5.
  play_queue_load(&q, lib, album(KID_A), 0);
  TEST_ASSERT_TRUE(play_queue_append(&q, 5));
  ASSERT_PLAY(&q, 4, 5, 6, 5);
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 3));

  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  assert_consistent(&q); // still two 5s, one 4 and one 6
  ASSERT_ORIGINAL(&q, 4, 5, 6, 5);

  // Shuffle off lands on that track's first place in the original order.
  play_queue_set_shuffle(&q, false);
  ASSERT_PLAY(&q, 4, 5, 6, 5);
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&q));
  TEST_ASSERT_EQUAL_UINT32(1, q.position);

  // Play next while shuffled goes after that same first place.
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 3));
  play_queue_set_shuffle(&q, true);
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 0));
  TEST_ASSERT_EQUAL_UINT16(5, q.play[0]);
  TEST_ASSERT_EQUAL_UINT16(0, q.play[1]);
  ASSERT_ORIGINAL(&q, 4, 5, 0, 6, 5);
  assert_consistent(&q);

  // From every position of the shuffled queue, off is on the same track.
  for (uint32_t i = 0; i < 5; i++) {
    play_queue_set_shuffle(&q, true);
    TEST_ASSERT_TRUE(play_queue_set_position(&q, i));
    uint32_t current = play_queue_current(&q);
    play_queue_set_shuffle(&q, false);
    TEST_ASSERT_EQUAL_UINT32(current, play_queue_current(&q));
    ASSERT_PLAY(&q, 4, 5, 0, 6, 5);
    assert_consistent(&q);
  }

  // A queue of one track many times over.
  play_queue_clear(&q);
  for (int i = 0; i < 6; i++) {
    TEST_ASSERT_TRUE(play_queue_append(&q, 7));
  }
  TEST_ASSERT_TRUE(play_queue_set_position(&q, 4));
  play_queue_set_shuffle(&q, true);
  ASSERT_PLAY(&q, 7, 7, 7, 7, 7, 7);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  TEST_ASSERT_TRUE(play_queue_insert_next(&q, 7));
  play_queue_set_shuffle(&q, false);
  ASSERT_PLAY(&q, 7, 7, 7, 7, 7, 7, 7);
  TEST_ASSERT_EQUAL_UINT32(0, q.position);
  assert_consistent(&q);
}

void test_same_seed_same_order(void) {
  static uint16_t play_a[CAPACITY], original_a[CAPACITY];
  static uint16_t play_b[CAPACITY], original_b[CAPACITY];
  play_queue_t a, b;
  play_queue_init(&a, play_a, original_a, CAPACITY, 1234);
  play_queue_init(&b, play_b, original_b, CAPACITY, 1234);
  // Three shuffles each, the two queues taking turns: neither disturbs the
  // other.
  for (int round = 0; round < 3; round++) {
    play_queue_load(&a, lib, songs(), 5);
    play_queue_set_shuffle(&a, true);
    play_queue_load(&b, lib, songs(), 5);
    play_queue_set_shuffle(&b, true);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(play_a, play_b, LIB16_TRACKS);
    TEST_ASSERT_EQUAL_UINT32(a.random, b.random);
    assert_consistent(&a);
    play_queue_set_shuffle(&a, false);
    play_queue_set_shuffle(&b, false);
  }

  // Another seed, another order.
  play_queue_init(&b, play_b, original_b, CAPACITY, 1235);
  play_queue_load(&a, lib, songs(), 5);
  play_queue_set_shuffle(&a, true);
  play_queue_load(&b, lib, songs(), 5);
  play_queue_set_shuffle(&b, true);
  TEST_ASSERT_TRUE(memcmp(play_a, play_b, sizeof(SONGS)) != 0);

  // Seed 0 is a seed like any other.
  play_queue_init(&b, play_b, original_b, CAPACITY, 0);
  TEST_ASSERT_NOT_EQUAL(0, b.random);
  play_queue_load(&b, lib, songs(), 5);
  play_queue_set_shuffle(&b, true);
  assert_consistent(&b);
  TEST_ASSERT_TRUE(memcmp(play_b, original_b, sizeof(SONGS)) != 0);
}

// --- Saving and restoring --------------------------------------------------
//
// The file, as the comment at the top of play_queue.c describes it.

enum {
  F_MAGIC = 0,
  F_VERSION = 4,
  F_CRC = 8,
  F_INDEX = 12,
  F_COUNT = 16,
  F_POSITION = 20,
  F_SHUFFLE = 24,
  F_LOOP = 28,
  F_HEADER = 32,
  F_CRC_FROM = 12, // the checksum covers everything from here to the end
};

#define INDEX_CHECKSUM 0xC0FFEE42u

static uint8_t img[F_HEADER + 4 * CAPACITY + 16];

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd16(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void wr16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

// CRC-32 as zlib, PNG and Ethernet compute it, one bit at a time: written
// independently of the one in play_queue.c.
static uint32_t crc32_bitwise(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

static void fix_checksum(size_t n) {
  wr32(img + F_CRC, crc32_bitwise(img + F_CRC_FROM, n - F_CRC_FROM));
}

// Save `queue` and read the file back into `img`. Returns its length.
static size_t save_image(const play_queue_t *queue) {
  FILE *f = fresh_tmp();
  TEST_ASSERT_TRUE(play_queue_save(queue, INDEX_CHECKSUM, f));
  long end = ftell(f);
  TEST_ASSERT_TRUE(end > 0);
  TEST_ASSERT_TRUE((size_t)end <= sizeof(img));
  rewind(f);
  TEST_ASSERT_EQUAL_size_t((size_t)end, fread(img, 1, (size_t)end, f));
  return (size_t)end;
}

// Restore `queue` from the first `n` bytes of `img`.
static bool restore_image(play_queue_t *queue, size_t n, uint32_t checksum,
                          uint32_t tracks) {
  FILE *f = fresh_tmp();
  if (n > 0) {
    TEST_ASSERT_EQUAL_size_t(n, fwrite(img, 1, n, f));
  }
  rewind(f);
  return play_queue_restore(queue, checksum, tracks, f);
}

// A second queue, holding something else, to restore into.
static uint16_t other_play[CAPACITY];
static uint16_t other_original[CAPACITY];
static play_queue_t other;

static void other_holds_something(void) {
  play_queue_init(&other, other_play, other_original, CAPACITY, 77);
  play_queue_load(&other, lib, album(UNKNOWN_ALBUM), 2);
  play_queue_set_loop(&other, PLAY_LOOP_ALL);
}

static void assert_rejected(size_t n, const char *what) {
  other_holds_something();
  TEST_ASSERT_FALSE_MESSAGE(
      restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, other.count, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(PLAY_QUEUE_NONE, play_queue_current(&other),
                                   what);
}

// The queue the damage tests save: all 16 songs, shuffled, part of the way
// through, with loop one. 32 + 2 * 2 * 16 = 96 bytes.
static size_t make_image(void) {
  play_queue_init(&q, play_slots, original_slots, CAPACITY, 5);
  play_queue_load(&q, lib, songs(), 5);
  play_queue_set_shuffle(&q, true);
  play_queue_set_loop(&q, PLAY_LOOP_ONE);
  for (int i = 0; i < 6; i++) {
    play_queue_next(&q, false);
  }
  size_t n = save_image(&q);
  TEST_ASSERT_EQUAL_size_t(96, n);
  return n;
}

void test_save_and_restore_round_trip(void) {
  size_t n = make_image();
  TEST_ASSERT_EQUAL_UINT32(6, q.position);

  // The file is what the comment in play_queue.c says it is.
  TEST_ASSERT_EQUAL_MEMORY("DAPQ", img + F_MAGIC, 4);
  TEST_ASSERT_EQUAL_UINT32(1, rd32(img + F_VERSION));
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                          crc32_bitwise((const uint8_t *)"123456789", 9));
  TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(img + F_CRC_FROM, n - F_CRC_FROM),
                          rd32(img + F_CRC));
  TEST_ASSERT_EQUAL_HEX32(INDEX_CHECKSUM, rd32(img + F_INDEX));
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, rd32(img + F_COUNT));
  TEST_ASSERT_EQUAL_UINT32(6, rd32(img + F_POSITION));
  TEST_ASSERT_EQUAL_UINT32(1, rd32(img + F_SHUFFLE));
  TEST_ASSERT_EQUAL_UINT32(2, rd32(img + F_LOOP));
  for (uint32_t i = 0; i < LIB16_TRACKS; i++) {
    TEST_ASSERT_EQUAL_UINT32(q.play[i], rd16(img + F_HEADER + 2 * i));
    TEST_ASSERT_EQUAL_UINT32(SONGS[i],
                             rd16(img + F_HEADER + 2 * LIB16_TRACKS + 2 * i));
  }

  // Into another queue that held something else.
  other_holds_something();
  uint32_t random = other.random;
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, other.count);
  TEST_ASSERT_EQUAL_UINT32(6, other.position);
  TEST_ASSERT_TRUE(other.shuffle);
  TEST_ASSERT_EQUAL(PLAY_LOOP_ONE, other.loop);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(q.play, other.play, LIB16_TRACKS);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(q.original, other.original, LIB16_TRACKS);
  TEST_ASSERT_EQUAL_UINT32(play_queue_current(&q), play_queue_current(&other));
  TEST_ASSERT_EQUAL_UINT32(CAPACITY, other.capacity);
  TEST_ASSERT_EQUAL_UINT32(random, other.random);
  assert_consistent(&other);

  // The restored queue behaves as the saved one: the same next track, the
  // same order when shuffle goes off, and the same file when saved again.
  TEST_ASSERT_EQUAL_UINT32(play_queue_next(&q, false),
                           play_queue_next(&other, false));
  static uint8_t first[sizeof(img)];
  TEST_ASSERT_TRUE(play_queue_set_position(&other, 6));
  memcpy(first, img, n);
  TEST_ASSERT_EQUAL_size_t(n, save_image(&other));
  TEST_ASSERT_EQUAL_MEMORY(first, img, n);
  play_queue_set_shuffle(&other, false);
  assert_order(SONGS, LIB16_TRACKS, other.play, other.count, "play order");

  // Shuffle off, loop all, at the last track.
  play_queue_init(&q, play_slots, original_slots, CAPACITY, 5);
  play_queue_load(&q, lib, album(KID_A), 2);
  play_queue_set_loop(&q, PLAY_LOOP_ALL);
  n = save_image(&q);
  TEST_ASSERT_EQUAL_size_t(F_HEADER + 12, n);
  other_holds_something();
  play_queue_set_shuffle(&other, true);
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));
  ASSERT_PLAY(&other, 4, 5, 6);
  ASSERT_ORIGINAL(&other, 4, 5, 6);
  TEST_ASSERT_EQUAL_UINT32(2, other.position);
  TEST_ASSERT_FALSE(other.shuffle);
  TEST_ASSERT_EQUAL(PLAY_LOOP_ALL, other.loop);
  // The index need have no more tracks than the queue uses.
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, 7));
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, 65535));

  // An empty queue is saved and restored as an empty queue, with its
  // settings.
  play_queue_clear(&q);
  play_queue_set_shuffle(&q, true);
  q.position = 12345; // meaningless when empty, and not what is saved
  n = save_image(&q);
  TEST_ASSERT_EQUAL_size_t(F_HEADER, n);
  TEST_ASSERT_EQUAL_UINT32(0, rd32(img + F_POSITION));
  other_holds_something();
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(0, other.count);
  TEST_ASSERT_TRUE(other.shuffle);
  TEST_ASSERT_EQUAL(PLAY_LOOP_ALL, other.loop);
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&other));
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, 0));

  // A file that cannot be written, and no file.
  TEST_ASSERT_FALSE(play_queue_save(&q, INDEX_CHECKSUM, NULL));
  // /dev/full takes the place of the last file, which is closed first;
  // tearDown closes it.
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = fopen("/dev/full", "wb");
  if (open_file == NULL) {
    TEST_IGNORE_MESSAGE("no /dev/full: a save to a full card is not tested");
  }
  play_queue_load(&q, lib, songs(), 0);
  TEST_ASSERT_FALSE(play_queue_save(&q, INDEX_CHECKSUM, open_file));
}

void test_restore_rejects_another_index(void) {
  size_t n = make_image();
  static const uint32_t OTHERS[] = {
      INDEX_CHECKSUM + 1, INDEX_CHECKSUM ^ 0x80000000u, 0, UINT32_MAX};
  for (size_t i = 0; i < sizeof(OTHERS) / sizeof(OTHERS[0]); i++) {
    other_holds_something();
    TEST_ASSERT_FALSE(restore_image(&other, n, OTHERS[i], LIB16_TRACKS));
    TEST_ASSERT_EQUAL_UINT32(0, other.count);
  }
  other_holds_something();
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(LIB16_TRACKS, other.count);

  // An empty queue is for one index too.
  play_queue_clear(&q);
  n = save_image(&q);
  TEST_ASSERT_FALSE(restore_image(&other, n, INDEX_CHECKSUM + 1, LIB16_TRACKS));
}

void test_restore_rejects_a_track_number_the_index_does_not_have(void) {
  // The queue holds tracks 0 to 15: an index of 15 tracks has no track 15.
  size_t n = make_image();
  other_holds_something();
  TEST_ASSERT_FALSE(restore_image(&other, n, INDEX_CHECKSUM, 15));
  TEST_ASSERT_EQUAL_UINT32(0, other.count);
  TEST_ASSERT_FALSE(restore_image(&other, n, INDEX_CHECKSUM, 1));
  TEST_ASSERT_FALSE(restore_image(&other, n, INDEX_CHECKSUM, 0));
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, 16));

  // One bad number, with a checksum that is right, in each place of each
  // order.
  for (uint32_t i = 0; i < 2 * LIB16_TRACKS; i++) {
    n = make_image();
    wr16(img + F_HEADER + 2 * i, 16);
    fix_checksum(n);
    assert_rejected(n, "track 16 of 16");
    wr16(img + F_HEADER + 2 * i, 65535);
    fix_checksum(n);
    assert_rejected(n, "track 65535 of 16");
  }
}

void test_restore_rejects_a_truncated_or_damaged_file(void) {
  size_t n = make_image();

  // Cut short at every length, and with something after it.
  for (size_t cut = 0; cut < n; cut++) {
    assert_rejected(cut, "cut short");
  }
  img[n] = 0;
  assert_rejected(n + 1, "a byte after the end");
  wr16(img + n, 3);
  assert_rejected(n + 2, "a track after the end");

  // One flipped bit, in every bit of the file (768 places).
  n = make_image();
  for (size_t bit = 0; bit < 8 * n; bit++) {
    img[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    assert_rejected(n, "one flipped bit");
    img[bit / 8] ^= (uint8_t)(1u << (bit % 8));
  }
  other_holds_something();
  TEST_ASSERT_TRUE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));

  // Fields that are wrong under a checksum that is right.
  static const struct {
    uint32_t at;
    uint32_t value;
    const char *what;
  } WRONG[] = {
      {F_VERSION, 0, "version 0"},
      {F_VERSION, 2, "version 2"},
      {F_POSITION, 16, "position at the count"},
      {F_POSITION, UINT32_MAX, "position far out"},
      {F_SHUFFLE, 2, "shuffle 2"},
      {F_SHUFFLE, 256, "shuffle 256"},
      {F_LOOP, 3, "loop 3"},
      {F_LOOP, UINT32_MAX, "loop far out"},
      {F_COUNT, 15, "a count below what the file holds"},
      {F_COUNT, 17, "a count above what the file holds"},
      {F_COUNT, 0, "a count of 0 before 16 tracks"},
      {F_COUNT, UINT32_MAX, "a count far out"},
      {F_COUNT, 0x80000010u, "a count that wraps when doubled"},
      {F_COUNT, 0x40000010u, "a count that wraps when multiplied by four"},
  };
  for (size_t i = 0; i < sizeof(WRONG) / sizeof(WRONG[0]); i++) {
    n = make_image();
    wr32(img + WRONG[i].at, WRONG[i].value);
    fix_checksum(n);
    assert_rejected(n, WRONG[i].what);
  }

  // Shuffle off, yet the two orders differ.
  n = make_image();
  wr32(img + F_SHUFFLE, 0);
  fix_checksum(n);
  assert_rejected(n, "shuffle off with two orders");

  // An empty queue with a position.
  play_queue_clear(&q);
  n = save_image(&q);
  wr32(img + F_POSITION, 1);
  fix_checksum(n);
  assert_rejected(n, "a position in an empty queue");
}

void test_restore_rejects_a_queue_larger_than_the_arrays(void) {
  size_t n = make_image(); // 16 tracks
  uint16_t play[15], original[15];
  play_queue_t small;
  play_queue_init(&small, play, original, 15, 1);
  play_queue_load(&small, lib, album(KID_A), 1);
  FILE *f = fresh_tmp();
  TEST_ASSERT_EQUAL_size_t(n, fwrite(img, 1, n, f));
  rewind(f);
  TEST_ASSERT_FALSE(
      play_queue_restore(&small, INDEX_CHECKSUM, LIB16_TRACKS, f));
  TEST_ASSERT_EQUAL_UINT32(0, small.count);
  TEST_ASSERT_EQUAL_UINT32(15, small.capacity);

  // Arrays of exactly the size are enough.
  uint16_t play16[16], original16[16];
  play_queue_init(&small, play16, original16, 16, 1);
  rewind(f);
  TEST_ASSERT_TRUE(play_queue_restore(&small, INDEX_CHECKSUM, LIB16_TRACKS, f));
  TEST_ASSERT_EQUAL_UINT32(16, small.count);
  assert_consistent(&small);

  // A queue that can hold nothing restores an empty queue and no other.
  play_queue_init(&small, play, original, 0, 1);
  rewind(f);
  TEST_ASSERT_FALSE(
      play_queue_restore(&small, INDEX_CHECKSUM, LIB16_TRACKS, f));
}

void test_failed_restore_leaves_the_queue_empty(void) {
  size_t n = make_image();
  img[F_HEADER + 3] ^= 0x10;

  other_holds_something();
  play_queue_set_shuffle(&other, true);
  TEST_ASSERT_EQUAL_UINT32(4, other.count);
  TEST_ASSERT_FALSE(restore_image(&other, n, INDEX_CHECKSUM, LIB16_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(0, other.count);
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_current(&other));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_next(&other, true));
  TEST_ASSERT_EQUAL_UINT32(PLAY_QUEUE_NONE, play_queue_previous(&other, 0));
  TEST_ASSERT_FALSE(play_queue_set_position(&other, 0));
  // Its settings are as they were, not as the file had them.
  TEST_ASSERT_TRUE(other.shuffle);
  TEST_ASSERT_EQUAL(PLAY_LOOP_ALL, other.loop);
  TEST_ASSERT_EQUAL_UINT32(CAPACITY, other.capacity);
  assert_consistent(&other);

  // No file at all.
  other_holds_something();
  TEST_ASSERT_FALSE(
      play_queue_restore(&other, INDEX_CHECKSUM, LIB16_TRACKS, NULL));
  TEST_ASSERT_EQUAL_UINT32(0, other.count);

  // And it is a queue that works.
  TEST_ASSERT_TRUE(play_queue_append(&other, 3));
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_current(&other));
  TEST_ASSERT_EQUAL_UINT32(3, play_queue_load(&other, lib, album(KID_A), 1));
  TEST_ASSERT_EQUAL_UINT32(5, play_queue_current(&other));
  assert_consistent(&other);
}

// --- Scale -----------------------------------------------------------------

#define BIG_TRACKS 5000u
#define BIG_CAPACITY 8192u // the device's queue
#define BIG_ARENA_BYTES 1572864u

static uint32_t big_arena_words[BIG_ARENA_BYTES / 4];
static uint16_t big_play[BIG_CAPACITY];
static uint16_t big_original[BIG_CAPACITY];
static uint16_t big_songs[BIG_TRACKS];
static uint8_t big_seen[BIG_TRACKS];

// 5,000 tracks: 50 artists of 10 albums of 10 tracks, with titles that sort
// in another order than the tracks.
static const library_t *build_big(void) {
  library_builder_t *b =
      library_builder_begin(big_arena_words, BIG_ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  char path[40], title[16], artist_name[16], album_name[16];
  library_track_in_t in = {.path = path,
                           .title = title,
                           .artist = artist_name,
                           .album = album_name,
                           .group_artist = artist_name};
  for (uint32_t i = 0; i < BIG_TRACKS; i++) {
    snprintf(artist_name, sizeof(artist_name), "Artist %02u",
             (unsigned)(i / 100));
    snprintf(album_name, sizeof(album_name), "Album %03u", (unsigned)(i / 10));
    snprintf(path, sizeof(path), "a%02u/b%03u/%02u.flac", (unsigned)(i / 100),
             (unsigned)(i / 10), (unsigned)(i % 10));
    snprintf(title, sizeof(title), "T%04u", (unsigned)(i * 2741u % BIG_TRACKS));
    in.track_no = (uint16_t)(i % 10 + 1);
    TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, &in));
  }
  const library_t *big = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(big);
  TEST_ASSERT_EQUAL_UINT32(BIG_TRACKS, library_track_count(big));
  return big;
}

static unsigned micros_since(clock_t t0) {
  return (unsigned)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC);
}

// Every one of the 5,000 tracks is in `order` exactly once.
static void assert_every_track_once(const uint16_t *order) {
  memset(big_seen, 0, sizeof(big_seen));
  for (uint32_t i = 0; i < BIG_TRACKS; i++) {
    TEST_ASSERT_LESS_THAN_UINT32(BIG_TRACKS, order[i]);
    TEST_ASSERT_EQUAL_UINT8(0, big_seen[order[i]]);
    big_seen[order[i]] = 1;
  }
}

void test_five_thousand_tracks(void) {
  const library_t *big = build_big();
  for (uint32_t i = 0; i < BIG_TRACKS; i++) {
    big_songs[i] = (uint16_t)library_song_by_title(big, i);
  }
  play_queue_t queue;
  play_queue_init(&queue, big_play, big_original, BIG_CAPACITY, 2026);
  const uint32_t start = 1234;

  // Each step is run 20 times and the times added up, so that the clock's
  // grain does not swallow them; what is printed is one run.
  enum { RUNS = 20 };
  clock_t t0 = clock();
  for (int i = 0; i < RUNS; i++) {
    TEST_ASSERT_EQUAL_UINT32(BIG_TRACKS,
                             play_queue_load(&queue, big, songs(), start));
  }
  unsigned load_us = micros_since(t0) / RUNS;
  TEST_ASSERT_EQUAL_UINT16_ARRAY(big_songs, queue.play, BIG_TRACKS);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(big_songs, queue.original, BIG_TRACKS);
  TEST_ASSERT_EQUAL_UINT32(big_songs[start], play_queue_current(&queue));

  unsigned shuffle_us = 0, unshuffle_us = 0;
  for (int i = 0; i < RUNS; i++) {
    t0 = clock();
    play_queue_set_shuffle(&queue, true);
    shuffle_us += micros_since(t0);
    TEST_ASSERT_EQUAL_UINT32(0, queue.position);
    TEST_ASSERT_EQUAL_UINT32(big_songs[start], play_queue_current(&queue));
    assert_every_track_once(queue.play);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(big_songs, queue.original, BIG_TRACKS);
    // Shuffled indeed: few tracks are where they were.
    uint32_t in_place = 0;
    for (uint32_t k = 0; k < BIG_TRACKS; k++) {
      in_place += queue.play[k] == big_songs[k];
    }
    TEST_ASSERT_LESS_THAN_UINT32(20, in_place);
    if (i + 1 == RUNS) {
      break; // stay shuffled for the walk
    }
    t0 = clock();
    play_queue_set_shuffle(&queue, false);
    unshuffle_us += micros_since(t0);
    TEST_ASSERT_EQUAL_UINT32(start, queue.position);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(big_songs, queue.play, BIG_TRACKS);
  }
  shuffle_us /= RUNS;
  unshuffle_us /= RUNS - 1;

  // Walk the shuffled queue to its end: every track plays once.
  memset(big_seen, 0, sizeof(big_seen));
  uint32_t played = 0;
  t0 = clock();
  for (uint32_t track = play_queue_current(&queue); track != PLAY_QUEUE_NONE;
       track = play_queue_next(&queue, true)) {
    if (track >= BIG_TRACKS || big_seen[track] != 0) {
      break;
    }
    big_seen[track] = 1;
    played++;
  }
  unsigned walk_us = micros_since(t0);
  TEST_ASSERT_EQUAL_UINT32(BIG_TRACKS, played);
  TEST_ASSERT_EQUAL_UINT32(BIG_TRACKS - 1, queue.position);

  // The worst insert: after the first track, everything moves; while
  // shuffled, so the current track is searched for as well.
  TEST_ASSERT_TRUE(play_queue_set_position(&queue, BIG_TRACKS - 1));
  uint32_t last = play_queue_current(&queue);
  play_queue_set_shuffle(&queue, false);
  play_queue_set_shuffle(&queue, true);
  TEST_ASSERT_EQUAL_UINT32(last, play_queue_current(&queue));
  t0 = clock();
  for (uint32_t i = 0; i < 100; i++) {
    TEST_ASSERT_TRUE(play_queue_insert_next(&queue, i));
  }
  unsigned insert_us = micros_since(t0) / 100;
  TEST_ASSERT_EQUAL_UINT32(BIG_TRACKS + 100, queue.count);

  // Back to the list order: the 100 are after the track they were put after.
  play_queue_set_shuffle(&queue, false);
  TEST_ASSERT_EQUAL_UINT32(last, play_queue_current(&queue));
  uint32_t at = queue.position;
  TEST_ASSERT_EQUAL_UINT16(last, big_songs[at]);
  for (uint32_t i = 0; i < 100; i++) {
    TEST_ASSERT_EQUAL_UINT16(99 - i, queue.play[at + 1 + i]);
  }
  for (uint32_t i = 0; i < BIG_TRACKS; i++) {
    TEST_ASSERT_EQUAL_UINT16(big_songs[i], queue.play[i <= at ? i : i + 100]);
  }

  // Saved and restored whole.
  FILE *f = fresh_tmp();
  t0 = clock();
  TEST_ASSERT_TRUE(play_queue_save(&queue, library_checksum(big), f));
  unsigned save_us = micros_since(t0);
  TEST_ASSERT_EQUAL_UINT32(F_HEADER + 4 * (BIG_TRACKS + 100),
                           (uint32_t)ftell(f));
  rewind(f);
  static uint16_t again_play[BIG_CAPACITY], again_original[BIG_CAPACITY];
  play_queue_t again;
  play_queue_init(&again, again_play, again_original, BIG_CAPACITY, 1);
  t0 = clock();
  TEST_ASSERT_TRUE(
      play_queue_restore(&again, library_checksum(big), BIG_TRACKS, f));
  unsigned restore_us = micros_since(t0);
  TEST_ASSERT_EQUAL_UINT32(queue.count, again.count);
  TEST_ASSERT_EQUAL_UINT32(queue.position, again.position);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(queue.play, again.play, queue.count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(queue.original, again.original, queue.count);

  printf("play_queue scale, 5000 tracks: load %u us, shuffle %u us, "
         "shuffle off %u us, walk to the end %u us, insert after the first "
         "track while shuffled %u us, save %u us, restore %u us\n",
         load_us, shuffle_us, unshuffle_us, walk_us, insert_us, save_us,
         restore_us);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_queue_answers_none);
  RUN_TEST(test_load_from_an_album_starts_at_the_chosen_track);
  RUN_TEST(test_load_takes_what_fits_and_keeps_the_chosen_track);
  RUN_TEST(test_load_tracks_skips_missing_and_starts_at_the_chosen_one);
  RUN_TEST(test_load_tracks_refuses_a_missing_start);
  RUN_TEST(test_load_tracks_while_shuffle_is_on);
  RUN_TEST(test_next_walks_the_queue);
  RUN_TEST(test_next_at_the_end_with_loop_off_stops_and_stays);
  RUN_TEST(test_next_at_the_end_with_loop_all_wraps);
  RUN_TEST(test_loop_one_repeats_when_the_track_ends);
  RUN_TEST(test_loop_one_still_advances_on_a_manual_next);
  RUN_TEST(test_previous_restarts_after_three_seconds);
  RUN_TEST(test_previous_within_three_seconds_goes_back);
  RUN_TEST(
      test_previous_at_the_first_track_restarts_without_loop_and_wraps_with_loop_all);
  RUN_TEST(test_set_position_moves_only_within_the_queue);
  RUN_TEST(test_insert_next_goes_after_the_current_track);
  RUN_TEST(test_append_goes_last);
  RUN_TEST(test_insert_and_append_into_an_empty_queue_make_it_current);
  RUN_TEST(test_append_list_adds_all_or_nothing);
  RUN_TEST(test_full_queue_refuses_and_stays_consistent);
  RUN_TEST(test_shuffle_keeps_the_current_track_first_and_every_track_once);
  RUN_TEST(test_shuffle_off_restores_the_original_order_and_the_current_track);
  RUN_TEST(test_shuffle_on_off_on_never_loses_a_track);
  RUN_TEST(test_shuffle_is_fair);
  RUN_TEST(test_shuffle_of_one_and_two_tracks);
  RUN_TEST(test_load_while_shuffle_is_on_shuffles_the_rest);
  RUN_TEST(test_edits_while_shuffled_survive_shuffle_off);
  RUN_TEST(test_duplicate_tracks_are_handled);
  RUN_TEST(test_same_seed_same_order);
  RUN_TEST(test_save_and_restore_round_trip);
  RUN_TEST(test_restore_rejects_another_index);
  RUN_TEST(test_restore_rejects_a_track_number_the_index_does_not_have);
  RUN_TEST(test_restore_rejects_a_truncated_or_damaged_file);
  RUN_TEST(test_restore_rejects_a_queue_larger_than_the_arrays);
  RUN_TEST(test_failed_restore_leaves_the_queue_empty);
  RUN_TEST(test_five_thousand_tracks);
  return UNITY_END();
}
