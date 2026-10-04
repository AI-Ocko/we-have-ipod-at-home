#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Typing a name on the keypad by multi-tap: each digit key cycles through
// its letters and then the digit; a character is committed when another key
// is pressed or after TEXTENTRY_COMMIT_MS.

#define TEXTENTRY_MAX 16        // characters of text
#define TEXTENTRY_COMMIT_MS 1000
#define TEXTENTRY_ROW 16        // width of the bottom row

typedef enum {
  TEXTENTRY_CONTINUE, // keep typing
  TEXTENTRY_DONE,     // '#': the text is finished (see textentry_text)
  TEXTENTRY_CANCEL,   // 'C': give up
} textentry_result_t;

typedef struct {
  char text[TEXTENTRY_MAX + 1]; // committed characters, terminated
  uint32_t len;
  char key;         // the key whose character is pending, or 0
  uint32_t cycle;   // which of that key's characters is pending
  int64_t since_ms; // when `key` was last pressed
} textentry_t;

// Start with `initial` (may be NULL): letters are made capitals, characters
// a name cannot have are dropped, and it is cut to TEXTENTRY_MAX.
void textentry_init(textentry_t *t, const char *initial);

// A key press at `now_ms`. '0'..'9' type, '*' deletes, '#' finishes, 'C'
// cancels; any other key does nothing. Call textentry_tick first if time
// may have passed.
textentry_result_t textentry_key(textentry_t *t, char key, int64_t now_ms);

// Commit the pending character if TEXTENTRY_COMMIT_MS have passed since its
// key was pressed. True if the text changed.
bool textentry_tick(textentry_t *t, int64_t now_ms);

// When textentry_tick next needs calling, or INT64_MAX if nothing is pending.
int64_t textentry_deadline(const textentry_t *t);

// The text as '#' would finish it: the committed characters and the pending
// one. Always terminated; `size` of at least TEXTENTRY_MAX + 1 always fits.
void textentry_text(const textentry_t *t, char *out, size_t size);

// The bottom row: the text, then the pending character or, if none is
// pending and there is room, a '_' cursor; the last TEXTENTRY_ROW characters
// if that is longer than the row; padded with spaces. No terminator.
void textentry_row(const textentry_t *t, char out[TEXTENTRY_ROW]);
