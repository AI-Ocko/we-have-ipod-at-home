#include "marquee.h"

#include <stdbool.h>
#include <string.h>

// The timeline of one cycle, for a text of `len` characters that does not
// fit. The text and its gap make a loop of `period = len + MARQUEE_GAP`
// characters; the offset is the place in that loop shown at the left edge.
//
//   0 .. REST                    offset 0 (the rest)
//   REST                         step 1: offset 1
//   REST + STEP                  step 2: offset 2
//   ...
//   REST + (period - 1) * STEP   step `period`: offset 0 again, and the next
//                                cycle's rest begins at that moment
//
// So the start is at the left for exactly the rest time, every other offset
// for exactly one step time, and a cycle lasts REST + (period - 1) * STEP.

typedef struct {
  size_t len;       // of the text
  size_t offset;    // in the loop; 0 if the text fits
  uint32_t next_ms; // until the offset changes; UINT32_MAX if it never does
} place_t;

// Where the marquee is at `elapsed_ms`. All the arithmetic on time is here,
// once per call; the copy below only counts.
static place_t place_at(const char *text, size_t width, uint32_t elapsed_ms) {
  place_t place = {
      .len = text ? strlen(text) : 0, .offset = 0, .next_ms = UINT32_MAX};
  // Nothing to move if it fits, and nothing to see in no space at all.
  if (place.len <= width || width == 0) {
    return place;
  }

  size_t period = place.len + MARQUEE_GAP;
  uint64_t cycle_ms = MARQUEE_REST_MS + (uint64_t)(period - 1) * MARQUEE_STEP_MS;
  // A text of millions of characters has a cycle longer than `elapsed_ms`
  // can count; it is then still in its first one.
  uint32_t t = cycle_ms > UINT32_MAX ? elapsed_ms
                                     : elapsed_ms % (uint32_t)cycle_ms;
  if (t < MARQUEE_REST_MS) {
    place.next_ms = MARQUEE_REST_MS - t;
    return place;
  }
  uint32_t moving_ms = t - MARQUEE_REST_MS;
  uint32_t steps = moving_ms / MARQUEE_STEP_MS; // whole step times gone by
  // The first step is taken as the rest ends. t is short of a whole cycle, so
  // this is at most period - 1: the step that comes home starts a new cycle.
  place.offset = (size_t)steps + 1;
  place.next_ms = MARQUEE_STEP_MS - (moving_ms - steps * MARQUEE_STEP_MS);
  return place;
}

void marquee_window(const char *text, size_t width, uint32_t elapsed_ms,
                    char *out) {
  place_t place = place_at(text, width, elapsed_ms);
  size_t period = place.len + MARQUEE_GAP;
  bool moves = (place.next_ms != UINT32_MAX);
  // Walk round the loop from the offset: the text, then the gap, then the
  // text again. Text that fits does not come round: what lies past its last
  // character is padding, however wide the space.
  size_t at = place.offset;
  for (size_t col = 0; col < width; col++) {
    out[col] = at < place.len ? text[at] : ' ';
    if (++at == period && moves) {
      at = 0;
    }
  }
}

uint32_t marquee_next_change_ms(const char *text, size_t width,
                                uint32_t elapsed_ms) {
  return place_at(text, width, elapsed_ms).next_ms;
}
