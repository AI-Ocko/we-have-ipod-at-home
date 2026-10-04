#include "playlist.h"

#include <limits.h>
#include <string.h>

// Design notes
//
// LINES. A line ends at an LF. Its text is the line without the LF, without
// a CR before it, and, for the first line of the file, without a UTF-8
// byte-order mark. A line whose text is longer than PLAYLIST_LINE_MAX makes
// the file a bad one. An entry is a line whose text is not empty and does not
// start with '#'. A last line with no LF after it is a line like any other
// (a lone CR at its end is dropped as well).
//
// THE WORK AREA. The streams this module is handed are, on the device,
// opened unbuffered or with small buffers (internal RAM is scarce), so the
// module does its own blocking: every fread asks for at least 4 KB, and
// every fwrite but the last of a call writes at least 4 KB.
//
//   read:  [0, 512)       the two candidate paths of the line in hand
//          [512, 16 KB)   input, 15.5 KB
//   edit:  [0, 8 KB)      input
//          [8 KB, 16 KB)  output: what is to be written is gathered here
//                         and written when the buffer is full
//
// The input buffer holds a window of the file. A line is handed out where it
// lies in the window; when the window ends inside a line, the part of the
// line already read is moved to the start and the rest of the buffer filled
// with one fread. That part is never more than PLAYLIST_LINE_MAX + 4 bytes
// (beyond that the line is too long whatever its end is), so each fread asks
// for at least the buffer less 1,028 bytes: 14.5 KB when reading, 7 KB when
// editing.
//
// READING is one pass: for each entry playlist_line_paths gives at most two
// candidate paths, and each is looked up with library_find_path, a binary
// search. Nothing else is done per line beyond finding its end with memchr.
//
// EDITING is two passes over the input. The first finds where the blocks the
// edit names start and end, counts the entries for an APPEND, and checks the
// lines, writing nothing; so every refusal (PLAYLIST_FULL, PLAYLIST_NO_ENTRY,
// PLAYLIST_BAD_FILE) comes before the first byte is written. The second
// copies byte ranges of the input to the output in the new order. A file
// that fits in the input buffer is read once: the second pass finds it still
// in the window. A bigger one is read again from its start (fseek), which
// for a 1,000-entry file of about 40 KB is a few more reads. A REMOVE or a
// MOVE stops the first pass at the last block it needs; an APPEND reads to
// the end, as it must count every entry.
//
// The input must therefore be seekable, and it is read from where it stands
// when playlist_edit is called (a byte-order mark is looked for there).
//
// BLOCKS. Entry i's block runs from the end of entry i-1's line (for entry 0,
// from the end of the header: a byte-order mark and a first line #EXTM3U,
// either or both, or nothing) to the end of entry i's line. Lines after the
// last entry belong to no block. REMOVE copies all but the block; MOVE copies
// the two blocks in the other order. When the second of them is the last
// line of the file and has no line end, it takes the first one's (LF or CR
// LF) and the first gives its up: the lines stay lines and the file still
// ends as it did.

#define BLOCK_MIN 4096
#define LINE_BYTES_MAX (PLAYLIST_LINE_MAX + 4) // a BOM before, a CR after

enum {
  READ_PATHS = 2 * PLAYLIST_PATH_MAX,
  READ_IN = PLAYLIST_WORK_BYTES - READ_PATHS,
  EDIT_IN = PLAYLIST_WORK_BYTES / 2,
  EDIT_OUT = PLAYLIST_WORK_BYTES - EDIT_IN,
  // An appended line: "../", the path and its terminator (then the LF).
  APPEND_LINE_MAX = 3 + PLAYLIST_PATH_MAX,
};

_Static_assert(READ_IN - LINE_BYTES_MAX >= BLOCK_MIN,
               "a read must fetch a whole block after keeping a line");
_Static_assert(EDIT_IN - LINE_BYTES_MAX >= BLOCK_MIN,
               "an edit must fetch a whole block after keeping a line");
_Static_assert(EDIT_OUT - APPEND_LINE_MAX >= BLOCK_MIN,
               "an edit must write whole blocks");

static const unsigned char BOM[3] = {0xEF, 0xBB, 0xBF};

static inline size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

static inline unsigned char ascii_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static inline bool is_alpha(unsigned char c) {
  return ascii_lower(c) >= 'a' && ascii_lower(c) <= 'z';
}

// True if the `len` bytes at `p` start with `prefix`, ignoring ASCII case.
static bool starts_with(const unsigned char *p, size_t len,
                        const char *prefix) {
  size_t n = strlen(prefix);
  if (len < n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (ascii_lower(p[i]) != ascii_lower((unsigned char)prefix[i])) {
      return false;
    }
  }
  return true;
}

// --- Paths ---------------------------------------------------------------------

// The bytes of a line's path, one at a time: backslashes come out as '/',
// and in a file:// URL %XX escapes come out decoded.
typedef struct {
  const unsigned char *p;
  size_t i, n;
  bool url;
} source_t;

enum { SOURCE_END = -1, SOURCE_BAD = -2 };

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = ascii_lower(c);
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

static int source_next(source_t *s) {
  if (s->i >= s->n) {
    return SOURCE_END;
  }
  int c = s->p[s->i++];
  if (s->url && c == '%') {
    if (s->n - s->i < 2) {
      return SOURCE_BAD;
    }
    int hi = hex_value(s->p[s->i]);
    int lo = hex_value(s->p[s->i + 1]);
    if (hi < 0 || lo < 0) {
      return SOURCE_BAD;
    }
    s->i += 2;
    c = hi * 16 + lo;
  }
  if (c == 0) {
    return SOURCE_BAD; // no file name holds a NUL
  }
  return c == '\\' ? '/' : c;
}

// The path `s` names, from the folder `base` (base_len bytes, no '/' at
// either end; "" for the card root), into `out`: '/' between parts and none
// at either end; empty parts and "." dropped, ".." taking off the part
// before. False if it goes above the card root, names the root itself, does
// not fit in PLAYLIST_PATH_MAX bytes on the way, or has a bad escape or a
// NUL.
static bool resolve(source_t s, const char *base, size_t base_len,
                    char *out) {
  size_t len = base_len;
  memcpy(out, base, base_len);
  size_t part = 0; // where the part being copied starts
  bool in_part = false;
  for (;;) {
    int c = source_next(&s);
    if (c == SOURCE_BAD) {
      return false;
    }
    if (c == SOURCE_END || c == '/') {
      if (in_part) {
        in_part = false;
        size_t part_len = len - part;
        bool dot = part_len == 1 && out[part] == '.';
        bool dots = part_len == 2 && out[part] == '.' && out[part + 1] == '.';
        if (dot || dots) {
          if (dots && part == 0) {
            return false; // above the card root
          }
          len = part > 0 ? part - 1 : 0; // the part and the '/' before it
          if (dots) {
            while (len > 0 && out[len - 1] != '/') {
              len--;
            }
            if (len > 0) {
              len--;
            }
          }
        }
      }
      if (c == SOURCE_END) {
        break;
      }
      continue;
    }
    if (!in_part) {
      if (len > 0) {
        if (len >= PLAYLIST_PATH_MAX - 1) {
          return false;
        }
        out[len++] = '/';
      }
      part = len;
      in_part = true;
    }
    if (len >= PLAYLIST_PATH_MAX - 1) {
      return false;
    }
    out[len++] = (char)c;
  }
  if (len == 0) {
    return false;
  }
  out[len] = '\0';
  return true;
}

// A URL scheme other than file: two or more letters, digits, '+', '-' or
// '.', the first a letter, then "://". (One letter and a colon is a drive.)
static bool is_other_url(const unsigned char *p, size_t len) {
  if (len == 0 || !is_alpha(p[0])) {
    return false;
  }
  size_t i = 1;
  while (i < len && (is_alpha(p[i]) || (p[i] >= '0' && p[i] <= '9') ||
                     p[i] == '+' || p[i] == '-' || p[i] == '.')) {
    i++;
  }
  return i >= 2 && len - i >= 3 && memcmp(p + i, "://", 3) == 0;
}

uint32_t playlist_line_paths(const char *line, size_t len,
                             char paths[2][PLAYLIST_PATH_MAX]) {
  const unsigned char *p = (const unsigned char *)line;
  if (len == 0 || p[0] == '#') {
    return 0;
  }
  source_t s = {.p = p, .i = 0, .n = len, .url = false};
  bool absolute = false;
  if (starts_with(p, len, "file://")) {
    size_t i = 7;
    s.url = true;
    // "file:///E:/x" has an empty host; "file://host/x" a host, which is
    // passed over; "file://E:/x" is seen too.
    if (!(len - i >= 2 && is_alpha(p[i]) && p[i + 1] == ':')) {
      while (i < len && p[i] != '/' && p[i] != '\\') {
        i++;
      }
      if (len - i >= 3 && is_alpha(p[i + 1]) && p[i + 2] == ':') {
        i += 3; // "/E:"
      }
    } else {
      i += 2;
    }
    s.i = i;
    absolute = true;
  } else if (is_other_url(p, len)) {
    return 0;
  } else if (len >= 2 && is_alpha(p[0]) && p[1] == ':') {
    s.i = 2;
    absolute = true;
  } else if (p[0] == '/' || p[0] == '\\') {
    absolute = true;
  }

  if (absolute) {
    return resolve(s, "", 0, paths[0]) ? 1 : 0;
  }
  // From the card root first: a "../" line (as the device writes) goes
  // above it there and so has only the reading from /playlists/.
  uint32_t n = 0;
  if (resolve(s, "", 0, paths[n])) {
    n++;
  }
  if (resolve(s, "playlists", 9, paths[n])) {
    n++;
  }
  return n;
}

// --- Reading in blocks -----------------------------------------------------------

typedef struct {
  FILE *f; // NULL: an empty file
  unsigned char *buf;
  size_t cap;
  size_t base;  // offset in the file (from `origin`) of buf[0]
  size_t fill;  // bytes in buf
  size_t pos;   // the next byte to hand out; base <= pos <= base + fill
  long origin;  // where in the stream reading began
  bool eof;     // the file holds nothing after base + fill
} reader_t;

static void reader_init(reader_t *r, FILE *f, unsigned char *buf, size_t cap,
                        long origin) {
  r->f = f;
  r->buf = buf;
  r->cap = cap;
  r->base = 0;
  r->fill = 0;
  r->pos = 0;
  r->origin = origin;
  r->eof = (f == NULL);
}

// Keep the bytes from `pos` on, moved to the start of the buffer, and fill
// the rest with one fread. False on a read error.
static bool refill(reader_t *r) {
  size_t keep = r->base + r->fill - r->pos;
  if (keep > 0 && r->pos != r->base) {
    memmove(r->buf, r->buf + (r->pos - r->base), keep);
  }
  r->base = r->pos;
  r->fill = keep;
  if (r->eof) {
    return true;
  }
  size_t want = r->cap - keep;
  size_t got = fread(r->buf + keep, 1, want, r->f);
  r->fill += got;
  if (got < want) {
    if (ferror(r->f)) {
      return false;
    }
    r->eof = true;
  }
  return true;
}

// Go to offset `off`: in the window if it is there, else by fseek (the next
// refill then reads from there).
static bool reader_seek(reader_t *r, size_t off) {
  if (off >= r->base && off <= r->base + r->fill) {
    r->pos = off;
    return true;
  }
  if (r->f == NULL || off > (size_t)(LONG_MAX - r->origin)) {
    return false;
  }
  if (fseek(r->f, r->origin + (long)off, SEEK_SET) != 0) {
    return false;
  }
  r->base = off;
  r->pos = off;
  r->fill = 0;
  r->eof = false;
  return true;
}

typedef struct {
  const unsigned char *p; // the line, without its LF, where it lies in the
  size_t len;             // buffer (good until the next call)
  size_t start;           // offset of its first byte
  size_t end;             // offset just past its LF, or the end of the file
  bool lf;                // false for a last line with no LF
} line_t;

typedef enum { LINE_OK, LINE_END, LINE_TOO_LONG, LINE_IO } line_status_t;

static line_status_t next_line(reader_t *r, line_t *line) {
  size_t scan = r->pos; // bytes before this hold no LF
  for (;;) {
    size_t end = r->base + r->fill;
    const unsigned char *at = r->buf + (r->pos - r->base);
    const unsigned char *lf =
        memchr(r->buf + (scan - r->base), '\n', end - scan);
    if (lf != NULL || (r->eof && end > r->pos)) {
      size_t len = lf != NULL ? (size_t)(lf - at) : end - r->pos;
      line->p = at;
      line->len = len;
      line->start = r->pos;
      line->lf = (lf != NULL);
      r->pos += len + (lf != NULL ? 1 : 0);
      line->end = r->pos;
      return LINE_OK;
    }
    if (end - r->pos > LINE_BYTES_MAX) {
      return LINE_TOO_LONG;
    }
    if (r->eof) {
      return LINE_END;
    }
    scan = end;
    if (!refill(r)) {
      return LINE_IO;
    }
  }
}

// A line's text: without a byte-order mark if it is the first line, and
// without a CR at its end.
static void line_text(const line_t *line, const unsigned char **text,
                      size_t *len) {
  const unsigned char *p = line->p;
  size_t n = line->len;
  if (line->start == 0 && n >= 3 && memcmp(p, BOM, 3) == 0) {
    p += 3;
    n -= 3;
  }
  if (n > 0 && p[n - 1] == '\r') {
    n--;
  }
  *text = p;
  *len = n;
}

static inline bool is_entry(const unsigned char *text, size_t len) {
  return len > 0 && text[0] != '#';
}

playlist_result_t playlist_read(FILE *in, const library_t *lib,
                                uint16_t *tracks, uint32_t max,
                                uint32_t *count, playlist_work_t *work) {
  char(*paths)[PLAYLIST_PATH_MAX] = (char(*)[PLAYLIST_PATH_MAX])work->bytes;
  reader_t r;
  reader_init(&r, in, work->bytes + READ_PATHS, READ_IN, 0);
  const uint32_t n_tracks = library_track_count(lib);
  uint32_t n = 0;
  playlist_result_t result = PLAYLIST_OK;
  for (;;) {
    line_t line;
    line_status_t status = next_line(&r, &line);
    if (status != LINE_OK) {
      result = status == LINE_END        ? PLAYLIST_OK
               : status == LINE_TOO_LONG ? PLAYLIST_BAD_FILE
                                         : PLAYLIST_IO_ERROR;
      break;
    }
    const unsigned char *text;
    size_t len;
    line_text(&line, &text, &len);
    if (len > PLAYLIST_LINE_MAX) {
      result = PLAYLIST_BAD_FILE;
      break;
    }
    if (!is_entry(text, len)) {
      continue;
    }
    if (n == max) {
      result = PLAYLIST_TRUNCATED;
      break;
    }
    uint16_t track = PLAYLIST_MISSING;
    uint32_t k = playlist_line_paths((const char *)text, len, paths);
    for (uint32_t i = 0; i < k; i++) {
      uint32_t found;
      if (library_find_path(lib, paths[i], &found) && found < n_tracks &&
          found < PLAYLIST_MISSING) {
        track = (uint16_t)found;
        break;
      }
    }
    tracks[n++] = track;
  }
  *count = n;
  return result;
}

// --- Writing in blocks -----------------------------------------------------------

typedef struct {
  FILE *f;
  unsigned char *buf;
  size_t cap;
  size_t fill;
  bool error;
} writer_t;

static void flush(writer_t *w) {
  if (w->fill > 0 && !w->error &&
      fwrite(w->buf, 1, w->fill, w->f) != w->fill) {
    w->error = true;
  }
  w->fill = 0;
}

static void put(writer_t *w, const void *p, size_t n) {
  const unsigned char *s = p;
  while (n > 0 && !w->error) {
    size_t k = min_size(n, w->cap - w->fill);
    memcpy(w->buf + w->fill, s, k);
    w->fill += k;
    s += k;
    n -= k;
    if (w->fill == w->cap) {
      flush(w);
    }
  }
}

// "../", the track's path and an LF. The line is made in place in the
// buffer; if there may not be room, what the buffer holds is written first
// (at least EDIT_OUT - APPEND_LINE_MAX bytes, a whole block).
static void put_track(writer_t *w, const library_t *lib, uint32_t track) {
  if (w->cap - w->fill < APPEND_LINE_MAX) {
    flush(w);
  }
  unsigned char *p = w->buf + w->fill;
  memcpy(p, "../", 3);
  size_t len =
      library_track_path(lib, track, (char *)p + 3, PLAYLIST_PATH_MAX);
  p[3 + len] = '\n'; // over the terminator; len < PLAYLIST_PATH_MAX
  w->fill += 4 + len;
  if (w->fill == w->cap) {
    flush(w);
  }
}

static bool finish(writer_t *w) {
  flush(w);
  if (!w->error && (fflush(w->f) != 0 || ferror(w->f))) {
    w->error = true;
  }
  return !w->error;
}

#define TO_END SIZE_MAX

// Copy bytes [from, to) of the input to the output; to TO_END copies to the
// end of the file. False if reading or writing failed, or the file ended
// before `to`.
static bool copy(reader_t *r, writer_t *w, size_t from, size_t to) {
  if (!reader_seek(r, from)) {
    return false;
  }
  while (r->pos < to && !w->error) {
    size_t end = r->base + r->fill;
    if (r->pos == end) {
      if (r->eof) {
        return to == TO_END;
      }
      if (!refill(r)) {
        return false;
      }
      continue;
    }
    size_t n = min_size(end, to) - r->pos;
    put(w, r->buf + (r->pos - r->base), n);
    r->pos += n;
  }
  return !w->error;
}

// --- Editing ---------------------------------------------------------------------

// What the first pass finds.
typedef struct {
  uint32_t entries;     // entries counted (all of them, unless it stopped)
  uint32_t found;       // of the entries wanted
  size_t start[2];      // their blocks
  size_t end[2];
  uint8_t line_end[2];  // bytes after each one's path line: 0, 1 (LF) or 2
  bool ends_cr[2];      // its path line, with no LF, ends with a CR
  size_t bom;           // 3 if the file starts with a byte-order mark
  size_t size;          // the file's length, if it read to the end
  bool ends_lf;         // the file's last line has an LF
} scan_t;

// Read the input up to the end of entry `first + want - 1` (to the end of
// the file if `want` is 0), noting the blocks of entries `first` to `first +
// want - 1`. PLAYLIST_FULL as soon as there are more than `limit` entries.
static playlist_result_t scan(reader_t *r, uint32_t first, uint32_t want,
                              uint32_t limit, scan_t *s) {
  memset(s, 0, sizeof(*s));
  size_t block = 0; // where the next entry's block starts
  for (;;) {
    line_t line;
    line_status_t status = next_line(r, &line);
    if (status == LINE_END) {
      s->size = r->pos;
      return PLAYLIST_OK;
    }
    if (status != LINE_OK) {
      return status == LINE_TOO_LONG ? PLAYLIST_BAD_FILE : PLAYLIST_IO_ERROR;
    }
    const unsigned char *text;
    size_t len;
    line_text(&line, &text, &len);
    if (len > PLAYLIST_LINE_MAX) {
      return PLAYLIST_BAD_FILE;
    }
    s->ends_lf = line.lf;
    if (line.start == 0) {
      s->bom = (size_t)(text - line.p);
      block = starts_with(text, len, "#EXTM3U") ? line.end : s->bom;
    }
    if (!is_entry(text, len)) {
      continue;
    }
    uint32_t i = s->entries++;
    if (s->entries > limit) {
      return PLAYLIST_FULL;
    }
    if (i >= first && i - first < want) {
      uint32_t k = i - first;
      s->start[k] = block;
      s->end[k] = line.end;
      s->line_end[k] =
          line.lf ? (line.len > 0 && line.p[line.len - 1] == '\r' ? 2 : 1) : 0;
      s->ends_cr[k] =
          !line.lf && line.len > 0 && line.p[line.len - 1] == '\r';
      s->found = k + 1;
      if (s->found == want) {
        return PLAYLIST_OK;
      }
    }
    block = line.end;
  }
}

playlist_result_t playlist_edit(FILE *in, FILE *out, const library_t *lib,
                                const playlist_edit_t *edit,
                                playlist_work_t *work) {
  if (edit == NULL) {
    return PLAYLIST_NO_ENTRY;
  }
  uint32_t first = 0;
  uint32_t want = 0;
  uint32_t limit = UINT32_MAX;
  switch (edit->kind) {
  case PLAYLIST_EDIT_APPEND: {
    if (edit->count > 0 && edit->tracks == NULL) {
      return PLAYLIST_NO_ENTRY;
    }
    const uint32_t n_tracks = library_track_count(lib);
    for (uint32_t i = 0; i < edit->count; i++) {
      // A path too long to read back is no track either; the scan never
      // puts one in the index.
      if (edit->tracks[i] >= n_tracks ||
          library_track_path(lib, edit->tracks[i], NULL, 0) >=
              PLAYLIST_PATH_MAX) {
        return PLAYLIST_NO_ENTRY;
      }
    }
    if (edit->count > PLAYLIST_ENTRIES_MAX) {
      return PLAYLIST_FULL;
    }
    limit = PLAYLIST_ENTRIES_MAX - edit->count;
    break;
  }
  case PLAYLIST_EDIT_REMOVE:
    first = edit->entry;
    want = 1;
    break;
  case PLAYLIST_EDIT_MOVE_UP:
    if (edit->entry == 0) {
      return PLAYLIST_NO_ENTRY;
    }
    first = edit->entry - 1;
    want = 2;
    break;
  case PLAYLIST_EDIT_MOVE_DOWN:
    if (edit->entry == UINT32_MAX) {
      return PLAYLIST_NO_ENTRY;
    }
    first = edit->entry;
    want = 2;
    break;
  default:
    return PLAYLIST_NO_ENTRY;
  }

  long origin = 0;
  if (in != NULL) {
    origin = ftell(in);
    if (origin < 0) {
      return PLAYLIST_IO_ERROR;
    }
  }
  reader_t r;
  reader_init(&r, in, work->bytes, EDIT_IN, origin);
  scan_t s;
  playlist_result_t result = scan(&r, first, want, limit, &s);
  if (result != PLAYLIST_OK) {
    return result;
  }
  if (s.found < want) {
    return PLAYLIST_NO_ENTRY;
  }

  writer_t w = {.f = out, .buf = work->bytes + EDIT_IN, .cap = EDIT_OUT};
  bool ok = true;
  if (edit->kind == PLAYLIST_EDIT_APPEND) {
    ok = copy(&r, &w, 0, TO_END);
    if (ok && s.size > s.bom && !s.ends_lf) {
      put(&w, "\n", 1);
    }
    for (uint32_t i = 0; ok && i < edit->count; i++) {
      put_track(&w, lib, edit->tracks[i]);
    }
  } else if (edit->kind == PLAYLIST_EDIT_REMOVE) {
    ok = copy(&r, &w, 0, s.start[0]) && copy(&r, &w, s.end[0], TO_END);
  } else {
    // Blocks a = [start[0], end[0]) and b = [end[0], end[1]), as b, a.
    ok = copy(&r, &w, 0, s.start[0]) && copy(&r, &w, s.end[0], s.end[1]);
    if (ok && s.line_end[1] == 0) {
      // b is the last line and has no line end: it takes a's.
      uint8_t n = s.line_end[0];
      if (s.ends_cr[1]) {
        put(&w, "\n", 1);
      } else {
        put(&w, &"\r\n"[2 - n], n);
      }
      ok = copy(&r, &w, s.start[0], s.end[0] - n);
    } else {
      ok = ok && copy(&r, &w, s.start[0], s.end[0]);
    }
    ok = ok && copy(&r, &w, s.end[1], TO_END);
  }
  if (!ok || !finish(&w)) {
    return PLAYLIST_IO_ERROR;
  }
  return PLAYLIST_OK;
}
