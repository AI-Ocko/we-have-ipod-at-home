#include "playlist_catalog.h"

#include <string.h>

// Design notes
//
// THE LIST. File names go into one pool, one after another, each with its
// terminator; `offset` says where each starts, and sorting moves only the
// offsets. Nothing is allocated. A listing is: clear, one add per file the
// folder holds, one sort.
//
// ORDER. Names (file names without the ending) in the library's order
// (library.c, ORDER): byte by byte with A-Z folded to a-z, and if that finds
// no difference, by the bytes as they are. So digits and most punctuation
// come before letters and bytes above 127 after them, which is the order of
// menu_group's groups: letter jumps work on the list as they do on the
// library's. The comparison is of names, not file names, so that "Alpha"
// comes before "alpha b" ('.' would sort after ' ').
//
// The sort is a heapsort over the offsets: O(n log n) comparisons whatever
// the order the folder lists its files in, in place, and with no context
// pointer that qsort could not pass to its comparison.
//
// FIND. Because the order is folded first, every name that folds to the same
// text is in one run, and a binary search on the folded comparison finds the
// first of them: O(log n) comparisons.

static inline unsigned char fold(unsigned char c) {
  return (unsigned char)(c - 'A') < 26u ? (unsigned char)(c + ('a' - 'A')) : c;
}

// The length of the ending if `file` (of `len` bytes) ends in .m3u or .m3u8,
// any case; 0 if not.
static size_t ending_of(const char *file, size_t len) {
  static const char M3U[] = ".m3u";
  size_t n = 0;
  if (len >= 5 && file[len - 1] == '8') {
    n = 5;
  } else if (len >= 4) {
    n = 4;
  } else {
    return 0;
  }
  for (size_t i = 0; i < 4; i++) {
    if (fold((unsigned char)file[len - n + i]) != (unsigned char)M3U[i]) {
      return 0;
    }
  }
  return n;
}

// The length of the name of a file the catalog holds (it has an ending).
static size_t name_length(const char *file) {
  size_t len = strlen(file);
  return len - ending_of(file, len);
}

// The library's order (see ORDER) between two names of the given lengths.
// With `folded_only` the second half is left out: 0 if they differ in
// nothing but the case of ASCII letters.
static int name_cmp(const char *a, size_t an, const char *b, size_t bn,
                    bool folded_only) {
  int as_written = 0;
  size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca != cb) {
      unsigned char fa = fold(ca);
      unsigned char fb = fold(cb);
      if (fa != fb) {
        return fa < fb ? -1 : 1;
      }
      if (as_written == 0) {
        as_written = ca < cb ? -1 : 1;
      }
    }
  }
  if (an != bn) {
    return an < bn ? -1 : 1; // the shorter is a prefix of the longer
  }
  return folded_only ? 0 : as_written;
}

static const char *file_at(const playlist_catalog_t *c, uint32_t i) {
  return c->pool + c->offset[i];
}

void playlist_catalog_clear(playlist_catalog_t *c) {
  if (c == NULL) {
    return;
  }
  c->count = 0;
  c->truncated = false;
  c->pool_used = 0;
}

bool playlist_catalog_add(playlist_catalog_t *c, const char *file_name) {
  if (c == NULL || file_name == NULL || file_name[0] == '.') {
    return false;
  }
  size_t len = strlen(file_name);
  size_t ending = ending_of(file_name, len);
  // A name of nothing but the ending starts with '.', refused above. A
  // ".tmp" left by a save does not end in .m3u or .m3u8.
  if (ending == 0) {
    return false;
  }
  if (c->count >= PLAYLIST_CATALOG_MAX ||
      len + 1 > (size_t)PLAYLIST_CATALOG_POOL - c->pool_used) {
    c->truncated = true;
    return false;
  }
  memcpy(c->pool + c->pool_used, file_name, len + 1);
  c->offset[c->count++] = (uint16_t)c->pool_used;
  c->pool_used += (uint32_t)(len + 1);
  return true;
}

// Whether playlist `i` comes before playlist `j` in the order (strictly).
static bool before(const playlist_catalog_t *c, uint16_t i, uint16_t j) {
  const char *a = c->pool + i;
  const char *b = c->pool + j;
  return name_cmp(a, name_length(a), b, name_length(b), false) < 0;
}

// Move the offset at `root` down the max-heap of the first `n` offsets.
static void sift_down(playlist_catalog_t *c, uint32_t root, uint32_t n) {
  uint16_t *o = c->offset;
  uint16_t value = o[root];
  for (;;) {
    uint32_t child = 2 * root + 1;
    if (child >= n) {
      break;
    }
    if (child + 1 < n && before(c, o[child], o[child + 1])) {
      child++;
    }
    if (!before(c, value, o[child])) {
      break;
    }
    o[root] = o[child];
    root = child;
  }
  o[root] = value;
}

void playlist_catalog_sort(playlist_catalog_t *c) {
  if (c == NULL || c->count < 2) {
    return;
  }
  uint32_t n = c->count;
  for (uint32_t i = n / 2; i-- > 0;) {
    sift_down(c, i, n);
  }
  for (uint32_t end = n - 1; end > 0; end--) {
    uint16_t top = c->offset[0];
    c->offset[0] = c->offset[end];
    c->offset[end] = top;
    sift_down(c, 0, end);
  }
}

const char *playlist_catalog_file(const playlist_catalog_t *c, uint32_t i) {
  return c != NULL && i < c->count ? file_at(c, i) : "";
}

void playlist_catalog_name(const playlist_catalog_t *c, uint32_t i, char *out,
                           size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  const char *file = playlist_catalog_file(c, i);
  size_t n = file[0] != '\0' ? name_length(file) : 0;
  if (n > out_size - 1) {
    n = out_size - 1;
  }
  memcpy(out, file, n);
  out[n] = '\0';
}

bool playlist_catalog_find(const playlist_catalog_t *c, const char *name,
                           uint32_t *i) {
  if (c == NULL || name == NULL || name[0] == '\0') {
    return false;
  }
  size_t name_len = strlen(name);
  // The first playlist whose name is not before `name`, folded.
  uint32_t lo = 0, hi = c->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const char *file = file_at(c, mid);
    if (name_cmp(file, name_length(file), name, name_len, true) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == c->count) {
    return false;
  }
  const char *file = file_at(c, lo);
  if (name_cmp(file, name_length(file), name, name_len, true) != 0) {
    return false;
  }
  if (i != NULL) {
    *i = lo;
  }
  return true;
}

static bool allowed(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' ||
         ch == '-' || ch == '\'' || ch == '&' || ch == '.';
}

// The rules are checked in the order of playlist_name_t, and the first one
// broken is the answer. On EMPTY and TOO_LONG `out` is ""; otherwise it is
// the trimmed name, whether or not the name is allowed.
playlist_name_t playlist_name_check(const char *typed, char *out) {
  out[0] = '\0';
  if (typed == NULL) {
    return PLAYLIST_NAME_EMPTY;
  }
  size_t start = 0;
  while (typed[start] == ' ') {
    start++;
  }
  size_t end = start + strlen(typed + start);
  while (end > start && typed[end - 1] == ' ') {
    end--;
  }
  size_t len = end - start;
  if (len == 0) {
    return PLAYLIST_NAME_EMPTY;
  }
  if (len > PLAYLIST_NAME_MAX) {
    return PLAYLIST_NAME_TOO_LONG;
  }
  memcpy(out, typed + start, len);
  out[len] = '\0';
  for (size_t k = 0; k < len; k++) {
    if (!allowed(out[k])) {
      return PLAYLIST_NAME_BAD_CHAR;
    }
  }
  // The card's file system drops a final '.', so "ROCK." would be saved as
  // "ROCK" and then be another playlist's name.
  // Checked before the start, so a name that does both (".", ".A.") is
  // BAD_END.
  if (out[len - 1] == '.') {
    return PLAYLIST_NAME_BAD_END;
  }
  // A file name starting with '.' is hidden: playlist_catalog_add refuses
  // it, so the playlist would never be listed and "Name in use" could not
  // see it.
  if (out[0] == '.') {
    return PLAYLIST_NAME_BAD_START;
  }
  return PLAYLIST_NAME_OK;
}

void playlist_file_for(const char *name, char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  size_t n = 0;
  if (name != NULL) {
    static const char ENDING[] = ".m3u";
    for (const char *p = name; *p != '\0' && n + 1 < out_size; p++) {
      out[n++] = *p;
    }
    for (const char *p = ENDING; *p != '\0' && n + 1 < out_size; p++) {
      out[n++] = *p;
    }
  }
  out[n] = '\0';
}
