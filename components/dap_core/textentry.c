#include "textentry.h"

#include <string.h>

// Design notes
//
// The pending character is not in `text`: it is `key` and `cycle`, and is
// written out only when committed. So '*' drops it by forgetting the key,
// and the committed text is always a plain terminated string.
//
// There is no timer here. The caller asks textentry_deadline when to wake,
// and calls textentry_tick then. A key press does not commit by time itself
// except for the one case that needs it: the same key again, which must know
// whether it cycles or starts a new character.
//
// Every call is a few instructions; textentry_row copies at most
// TEXTENTRY_ROW characters.

// The characters each digit key cycles through, the digit last.
static const char *const CYCLES[10] = {
    " 0", "-'&.1", "ABC2", "DEF3", "GHI4",
    "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9",
};

static const char *cycle_of(char key) { return CYCLES[key - '0']; }

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Characters a name can have: capitals, digits, space and - ' & .
static bool name_char(char c) {
  return (c >= 'A' && c <= 'Z') || is_digit(c) || c == ' ' || c == '-' ||
         c == '\'' || c == '&' || c == '.';
}

static char pending_char(const textentry_t *t) {
  return cycle_of(t->key)[t->cycle];
}

// Commit the pending character, if any. There is always room: a key is made
// pending only while len < TEXTENTRY_MAX.
static void commit(textentry_t *t) {
  if (t->key != 0) {
    t->text[t->len++] = pending_char(t);
    t->text[t->len] = '\0';
    t->key = 0;
    t->cycle = 0;
  }
}

void textentry_init(textentry_t *t, const char *initial) {
  memset(t, 0, sizeof(*t));
  if (initial == NULL) {
    return;
  }
  for (const char *p = initial; *p != '\0' && t->len < TEXTENTRY_MAX; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (name_char(c)) {
      t->text[t->len++] = c;
    }
  }
  t->text[t->len] = '\0';
}

textentry_result_t textentry_key(textentry_t *t, char key, int64_t now_ms) {
  if (is_digit(key)) {
    if (t->key == key && now_ms - t->since_ms < TEXTENTRY_COMMIT_MS) {
      t->cycle++;
      if (cycle_of(key)[t->cycle] == '\0') {
        t->cycle = 0;
      }
    } else {
      commit(t);
      if (t->len >= TEXTENTRY_MAX) {
        return TEXTENTRY_CONTINUE; // full: the key is ignored
      }
      t->key = key;
      t->cycle = 0;
    }
    t->since_ms = now_ms;
  } else if (key == '*') {
    if (t->key != 0) {
      t->key = 0;
      t->cycle = 0;
    } else if (t->len > 0) {
      t->text[--t->len] = '\0';
    }
  } else if (key == '#') {
    commit(t);
    return TEXTENTRY_DONE;
  } else if (key == 'C') {
    return TEXTENTRY_CANCEL;
  }
  return TEXTENTRY_CONTINUE;
}

bool textentry_tick(textentry_t *t, int64_t now_ms) {
  if (t->key == 0 || now_ms - t->since_ms < TEXTENTRY_COMMIT_MS) {
    return false;
  }
  commit(t);
  return true;
}

int64_t textentry_deadline(const textentry_t *t) {
  return t->key != 0 ? t->since_ms + TEXTENTRY_COMMIT_MS : INT64_MAX;
}

void textentry_text(const textentry_t *t, char *out, size_t size) {
  if (size == 0) {
    return;
  }
  size_t n = t->len < size - 1 ? t->len : size - 1;
  memcpy(out, t->text, n);
  if (t->key != 0 && n < size - 1) {
    out[n++] = pending_char(t);
  }
  out[n] = '\0';
}

void textentry_row(const textentry_t *t, char out[TEXTENTRY_ROW]) {
  // The text and one more character: the pending one, or the cursor.
  char line[TEXTENTRY_MAX + 1];
  size_t n = t->len;
  memcpy(line, t->text, n);
  if (t->key != 0) {
    line[n++] = pending_char(t);
  } else if (n < TEXTENTRY_MAX) {
    line[n++] = '_';
  }
  size_t from = n > TEXTENTRY_ROW ? n - TEXTENTRY_ROW : 0;
  memcpy(out, line + from, n - from);
  memset(out + (n - from), ' ', TEXTENTRY_ROW - (n - from));
}
