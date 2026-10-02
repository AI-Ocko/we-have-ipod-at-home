#include "tags.h"

#include "flac_header.h"
#include "id3.h"
#include "mp3_xing.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

// Design notes
//
// This runs once per file while a card of thousands of files is scanned, and
// the card is slow: what a file costs is the number of sectors fetched from
// it and the number of seeks, not the number of bytes looked at. So:
//
// - All reading goes through a one-sector window (reader_t). A refill asks
//   for the whole 512-byte sector that holds the wanted byte, which is the
//   unit the card delivers anyway: everything wanted from one sector costs
//   one request, no request ever touches two sectors, and sectors in a row
//   are read without a seek between them.
// - Stepping over something is only arithmetic on an offset. The seek happens
//   when (and if) a byte beyond the window is next wanted, so a picture is
//   never read and a block that turns out not to matter costs nothing.
// - A FLAC file is left as soon as its comment block has been read; the
//   blocks behind it are not walked. (flac_header_read walks them all, to
//   find where the audio starts, which nothing here needs.)
// - An MP3's last sector is visited only if something is still wanted from
//   it: a field an ID3v1 tag could supply, or the file's size for a length
//   estimate. Getting there makes the file system follow the file's whole
//   cluster chain, so a fully tagged file with a Xing/Info header is spared
//   that.
// - Reading stops when every wanted field has been seen.
//
// The files are whatever turned up on the card. Every length and offset
// taken from one is checked against the block or tag it lies in before it is
// used; every loop consumes at least four bytes of the file per pass; and a
// file can make this read at most READ_BUDGET_SECTORS sectors, whatever it
// says about itself.
//
// Nothing here allocates. The stack holds the window (512 bytes) and one
// value's undecoded bytes (256): about 1.2 KB at the deepest point on the
// ESP32 (measured with -fstack-usage at -O2), not counting what fread and
// fseek use beneath.

// --- Reading -----------------------------------------------------------------

#define SECTOR_BYTES 512

// The most sectors read from one file: 64 KB. A file with every wanted field
// behind a picture needs three or four. This is what stops a tag made of a
// million tiny frames from being read from end to end.
#define READ_BUDGET_SECTORS 128

typedef struct {
  FILE *f;
  uint64_t win_at;    // file offset of win[0]
  size_t win_len;     // bytes of win that are file contents
  uint64_t file_at;   // where the stream's position is,
  bool file_at_known; //   if this is true
  unsigned budget;    // sector reads left
  uint8_t win[SECTOR_BYTES];
} reader_t;

static void rd_init(reader_t *r, FILE *f) {
  r->f = f;
  r->win_at = 0;
  r->win_len = 0;
  r->file_at = 0;
  r->file_at_known = false; // the caller may have left it anywhere
  r->budget = READ_BUDGET_SECTORS;
}

// Load the sector that holds offset `at`. False if the file has no byte
// there (or it cannot be had).
static bool rd_fill(reader_t *r, uint64_t at) {
  r->win_len = 0;
  // `long` is 32 bits on the ESP32: an offset from a damaged file must not
  // wrap round to a small, plausible one.
  if (r->budget == 0 || at > (uint64_t)LONG_MAX) {
    return false;
  }
  r->budget--;
  // Sectors in a row are read without a seek between them, also when what
  // was stepped over ended a few bytes into the next one.
  uint64_t start = at - at % SECTOR_BYTES;
  if (!r->file_at_known || r->file_at != start) {
    r->file_at_known = false;
    if (fseek(r->f, (long)start, SEEK_SET) != 0) {
      return false;
    }
  }
  size_t got = fread(r->win, 1, SECTOR_BYTES, r->f);
  r->win_at = start;
  r->win_len = got;
  // After a short read (the end of the file, or an error) the position is
  // not relied on: the next refill seeks.
  r->file_at = start + got;
  r->file_at_known = (got == SECTOR_BYTES);
  return at - start < got;
}

// Copy up to `n` bytes at file offset `at` into `dst`. Returns how many there
// were: fewer than `n` at the end of the file, on a read error, or when the
// file's read budget is spent.
static size_t rd_read(reader_t *r, uint64_t at, uint8_t *dst, size_t n) {
  size_t done = 0;
  while (done < n) {
    if (at < r->win_at || at >= r->win_at + r->win_len) {
      if (!rd_fill(r, at)) {
        break;
      }
    }
    size_t offset = (size_t)(at - r->win_at);
    size_t take = r->win_len - offset;
    if (take > n - done) {
      take = n - done;
    }
    memcpy(dst + done, r->win + offset, take);
    done += take;
    at += take;
  }
  return done;
}

static bool rd_exact(reader_t *r, uint64_t at, uint8_t *dst, size_t n) {
  return rd_read(r, at, dst, n) == n;
}

// Find the file's size and load its last ID3V1_TAG_BYTES bytes into the
// window (win_len stays 0 if the file is shorter than that). With one seek:
// positioning relative to the end and asking where that is gives the size
// without a second trip along the cluster chain.
static bool rd_end(reader_t *r, uint64_t *size) {
  r->win_len = 0;
  r->file_at_known = false;
  if (r->budget == 0) {
    return false;
  }
  r->budget--;
  if (fseek(r->f, -(long)ID3V1_TAG_BYTES, SEEK_END) == 0) {
    long at = ftell(r->f);
    size_t got = fread(r->win, 1, ID3V1_TAG_BYTES, r->f);
    // Only a full read proves the position was what was asked for: on a file
    // shorter than the tag, some file systems (FatFs behind ESP-IDF's VFS
    // among them) report success and stay at the end of the file.
    if (at >= 0 && got == ID3V1_TAG_BYTES) {
      r->win_at = (uint64_t)at;
      r->win_len = got;
      *size = (uint64_t)at + ID3V1_TAG_BYTES;
      return true;
    }
  }
  // Shorter than an ID3v1 tag (or unreadable there): only the size is of
  // interest.
  if (fseek(r->f, 0, SEEK_END) != 0) {
    return false;
  }
  long at = ftell(r->f);
  if (at < 0) {
    return false;
  }
  *size = (uint64_t)at;
  return true;
}

static uint32_t be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | p[0];
}

// --- Text --------------------------------------------------------------------

// The numbers are the ID3v2 encoding bytes.
typedef enum {
  TEXT_LATIN1 = 0,  // ISO-8859-1
  TEXT_UTF16 = 1,   // with a byte order mark
  TEXT_UTF16BE = 2, // big-endian, no mark
  TEXT_UTF8 = 3,
} text_encoding_t;
#define TEXT_ENCODING_MAX TEXT_UTF8

// The most bytes of a stored value that can matter to a field: a field holds
// TAGS_TEXT_MAX - 1 bytes of UTF-8 and every character is at least one of
// them. So ISO-8859-1 needs one byte per character; UTF-16 two, and two more
// for the byte order mark; UTF-8 the field's size and up to three more, so
// that a character that starts inside is seen whole.
#define RAW_MAX (2 * (TAGS_TEXT_MAX - 1) + 2)

static size_t raw_needed(text_encoding_t encoding) {
  switch (encoding) {
  case TEXT_LATIN1:
    return TAGS_TEXT_MAX - 1;
  case TEXT_UTF8:
    return TAGS_TEXT_MAX - 1 + 3;
  default:
    return RAW_MAX;
  }
}

// What a byte that is not text is shown as (U+FFFD).
#define REPLACEMENT 0xFFFD

// Where decoded characters go. Returns false when it wants no more.
typedef bool (*emit_fn)(void *sink, uint32_t cp);

// A text field being filled. `len` never passes `cap - 1`, and a character
// goes in whole or not at all, so the text can be terminated at `len` at any
// time and is valid UTF-8.
typedef struct {
  char *text;
  size_t cap;
  size_t len;
} field_sink_t;

static bool field_emit(void *sink, uint32_t cp) {
  field_sink_t *s = sink;
  if (cp >= 0x110000 || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = REPLACEMENT; // not a character, so not allowed in UTF-8
  }
  size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  if (need > s->cap - 1 - s->len) {
    return false; // full: cut here, at a character boundary
  }
  char *p = &s->text[s->len];
  switch (need) {
  case 1:
    p[0] = (char)cp;
    break;
  case 2:
    p[0] = (char)(0xC0 | (cp >> 6));
    p[1] = (char)(0x80 | (cp & 0x3F));
    break;
  case 3:
    p[0] = (char)(0xE0 | (cp >> 12));
    p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[2] = (char)(0x80 | (cp & 0x3F));
    break;
  default:
    p[0] = (char)(0xF0 | (cp >> 18));
    p[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    p[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[3] = (char)(0x80 | (cp & 0x3F));
    break;
  }
  s->len += need;
  return true;
}

// A track or disc number being read: spaces, digits, then the end, a space
// or the "/" of "3/12".
typedef struct {
  enum { NUMBER_BEFORE, NUMBER_DIGITS, NUMBER_DONE, NUMBER_BAD } state;
  uint32_t value;
} number_sink_t;

static bool number_emit(void *sink, uint32_t cp) {
  number_sink_t *s = sink;
  if (cp >= '0' && cp <= '9') {
    s->state = NUMBER_DIGITS;
    s->value = s->value * 10 + (cp - '0');
    if (s->value > UINT16_MAX) {
      s->state = NUMBER_BAD; // more than the field holds: not a track number
      return false;
    }
    return true;
  }
  if (s->state == NUMBER_BEFORE && cp == ' ') {
    return true;
  }
  s->state = (s->state == NUMBER_DIGITS && (cp == '/' || cp == ' '))
                 ? NUMBER_DONE
                 : NUMBER_BAD;
  return false; // nothing after this changes the answer
}

static void decode_latin1(const uint8_t *p, size_t n, emit_fn emit,
                          void *sink) {
  // ISO-8859-1 is the first 256 code points of Unicode, byte for byte.
  for (size_t i = 0; i < n && p[i] != 0; i++) {
    if (!emit(sink, p[i])) {
      return;
    }
  }
}

// One character from the `n` (at least 1) bytes at `p`. Returns how many
// bytes it took. A byte that cannot begin a character, or begins one that is
// not completed correctly, is taken alone and gives REPLACEMENT; "correctly"
// excludes overlong forms, surrogates and anything above U+10FFFF, so what
// comes out is always something UTF-8 may hold.
static size_t utf8_char(const uint8_t *p, size_t n, uint32_t *cp) {
  uint8_t lead = p[0];
  size_t len;
  uint32_t c;
  // The range allowed for the second byte, which is where the exclusions
  // show: E0 80..9F would be overlong, ED A0..BF a surrogate, and so on.
  uint8_t low = 0x80, high = 0xBF;
  if (lead < 0x80) {
    *cp = lead;
    return 1;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    len = 2;
    c = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    len = 3;
    c = lead & 0x0F;
    if (lead == 0xE0) {
      low = 0xA0;
    } else if (lead == 0xED) {
      high = 0x9F;
    }
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    len = 4;
    c = lead & 0x07;
    if (lead == 0xF0) {
      low = 0x90;
    } else if (lead == 0xF4) {
      high = 0x8F;
    }
  } else {
    *cp = REPLACEMENT; // a continuation byte, C0, C1, or F5 and up
    return 1;
  }
  if (n < len) {
    *cp = REPLACEMENT;
    return 1;
  }
  for (size_t i = 1; i < len; i++) {
    if (p[i] < low || p[i] > high) {
      *cp = REPLACEMENT;
      return 1;
    }
    c = (c << 6) | (p[i] & 0x3F);
    low = 0x80;
    high = 0xBF;
  }
  *cp = c;
  return len;
}

static void decode_utf8(const uint8_t *p, size_t n, emit_fn emit, void *sink) {
  size_t i = 0;
  while (i < n && p[i] != 0) {
    uint32_t cp;
    i += utf8_char(&p[i], n - i, &cp);
    if (!emit(sink, cp)) {
      return;
    }
  }
}

static void decode_utf16(const uint8_t *p, size_t n, bool with_mark,
                         emit_fn emit, void *sink) {
  // Big-endian unless a mark says otherwise: that is what encoding 2 is, and
  // what Unicode says of UTF-16 that has no mark. A mark in front of
  // encoding 2 text is not part of the text either.
  bool big_endian = true;
  size_t i = 0;
  if (n >= 2) {
    if (p[0] == 0xFE && p[1] == 0xFF) {
      i = 2;
    } else if (with_mark && p[0] == 0xFF && p[1] == 0xFE) {
      big_endian = false;
      i = 2;
    }
  }
  // An odd byte left over at the end is not looked at.
  while (n - i >= 2) {
    uint32_t cp = big_endian ? ((uint32_t)p[i] << 8) | p[i + 1]
                             : ((uint32_t)p[i + 1] << 8) | p[i];
    i += 2;
    if (cp == 0) {
      return;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF && n - i >= 2) {
      // The first half of a surrogate pair, if the second half follows. A
      // half without its other half is passed on as it is: it is not a
      // character, and field_emit shows it as REPLACEMENT.
      uint32_t second = big_endian ? ((uint32_t)p[i] << 8) | p[i + 1]
                                   : ((uint32_t)p[i + 1] << 8) | p[i];
      if (second >= 0xDC00 && second <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (second - 0xDC00);
        i += 2;
      }
    }
    if (!emit(sink, cp)) {
      return;
    }
  }
}

static void decode(text_encoding_t encoding, const uint8_t *p, size_t n,
                   emit_fn emit, void *sink) {
  switch (encoding) {
  case TEXT_LATIN1:
    decode_latin1(p, n, emit, sink);
    break;
  case TEXT_UTF16:
    decode_utf16(p, n, true, emit, sink);
    break;
  case TEXT_UTF16BE:
    decode_utf16(p, n, false, emit, sink);
    break;
  case TEXT_UTF8:
    decode_utf8(p, n, emit, sink);
    break;
  }
}

// --- Fields ------------------------------------------------------------------

typedef enum {
  FIELD_TITLE,
  FIELD_ARTIST,
  FIELD_ALBUM,
  FIELD_ALBUM_ARTIST,
  FIELD_TRACK,
  FIELD_DISC,
  FIELD_COUNT,
} field_t;

#define ALL_FIELDS ((1u << FIELD_COUNT) - 1)

// Put a value into a field of `out`, replacing what was there: `n` bytes of
// text in `encoding`, up to the first terminator. Text is cut to what the
// field holds; a number that is not one becomes 0. `cut` says that the
// stored value goes on beyond these bytes.
static void set_field(tags_t *out, field_t field, text_encoding_t encoding,
                      const uint8_t *p, size_t n, bool cut) {
  if (field == FIELD_TRACK || field == FIELD_DISC) {
    number_sink_t sink = {NUMBER_BEFORE, 0};
    decode(encoding, p, n, number_emit, &sink);
    // Digits that run up to where the bytes were cut may not be all of the
    // number; no number at all is better than part of one.
    bool whole =
        sink.state == NUMBER_DONE || (sink.state == NUMBER_DIGITS && !cut);
    uint16_t value = whole ? (uint16_t)sink.value : 0;
    if (field == FIELD_TRACK) {
      out->track_no = value;
    } else {
      out->disc_no = value;
    }
    return;
  }
  field_sink_t sink = {NULL, TAGS_TEXT_MAX, 0};
  switch (field) {
  case FIELD_TITLE:
    sink.text = out->title;
    break;
  case FIELD_ARTIST:
    sink.text = out->artist;
    break;
  case FIELD_ALBUM:
    sink.text = out->album;
    break;
  default:
    sink.text = out->album_artist;
    break;
  }
  decode(encoding, p, n, field_emit, &sink);
  sink.text[sink.len] = '\0';
}

// The same for a value still in the file: `n` bytes at offset `at`. Only as
// much of it is read as the field can use. A value the file ends in the
// middle of is not used.
static void read_field(reader_t *r, uint64_t at, uint32_t n, tags_t *out,
                       field_t field, text_encoding_t encoding) {
  uint8_t raw[RAW_MAX];
  size_t want = raw_needed(encoding);
  bool cut = n > want;
  if (!cut) {
    want = n;
  }
  if (rd_exact(r, at, raw, want)) {
    set_field(out, field, encoding, raw, want, cut);
  }
}

// --- FLAC --------------------------------------------------------------------

#define FLAC_BLOCK_HEADER_BYTES 4
#define FLAC_BLOCK_LAST 0x80
#define FLAC_BLOCK_TYPE_MASK 0x7F
#define FLAC_BLOCK_STREAMINFO 0
#define FLAC_BLOCK_VORBIS_COMMENT 4

static const struct {
  const char *key;
  field_t field;
} VORBIS_KEYS[] = {
    {"TITLE", FIELD_TITLE},
    {"ARTIST", FIELD_ARTIST},
    {"ALBUM", FIELD_ALBUM},
    {"ALBUMARTIST", FIELD_ALBUM_ARTIST},
    {"ALBUM ARTIST", FIELD_ALBUM_ARTIST},
    {"TRACKNUMBER", FIELD_TRACK},
    {"DISCNUMBER", FIELD_DISC},
};

// Enough of a comment to see any of those keys and the "=" after it.
#define VORBIS_KEY_PROBE_BYTES 13

static char ascii_upper(uint8_t c) {
  return (char)((c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c);
}

// Which field the comment that starts with these bytes is for; FIELD_COUNT
// if none. `*key_len` is then the length of its key.
static field_t vorbis_field(const uint8_t *p, size_t n, size_t *key_len) {
  const uint8_t *equals = memchr(p, '=', n);
  if (equals == NULL) {
    return FIELD_COUNT; // no key, or a longer one than any wanted
  }
  size_t len = (size_t)(equals - p);
  for (size_t k = 0; k < sizeof(VORBIS_KEYS) / sizeof(VORBIS_KEYS[0]); k++) {
    const char *key = VORBIS_KEYS[k].key;
    if (strlen(key) != len) {
      continue;
    }
    size_t i = 0;
    while (i < len && ascii_upper(p[i]) == key[i]) {
      i++;
    }
    if (i == len) {
      *key_len = len;
      return VORBIS_KEYS[k].field;
    }
  }
  return FIELD_COUNT;
}

// A little-endian length at `*at`, which is stepped past it. False if the
// block, which ends at `end`, has no room for one or it cannot be read.
static bool vorbis_length(reader_t *r, uint64_t *at, uint64_t end,
                          uint32_t *length) {
  uint8_t b[4];
  if (end - *at < sizeof(b) || !rd_exact(r, *at, b, sizeof(b))) {
    return false;
  }
  *at += sizeof(b);
  *length = le32(b);
  return true;
}

// The comment block whose body is the `block_len` bytes at `at`: the
// encoder's name, a count, then that many "KEY=value" comments, each with
// its length in front. Stops at the first thing that does not fit inside the
// block, keeping what came before.
static void read_vorbis_comments(reader_t *r, uint64_t at, uint32_t block_len,
                                 tags_t *out) {
  const uint64_t end = at + block_len; // `at` never passes this
  uint32_t length, count;
  if (!vorbis_length(r, &at, end, &length) || length > end - at) {
    return;
  }
  at += length; // the encoder's name
  if (!vorbis_length(r, &at, end, &count)) {
    return;
  }
  // Each pass uses up four bytes of the block or returns, so a count that is
  // too large ends with the block; and when every wanted key has been seen
  // the rest is left unread.
  unsigned seen = 0;
  for (uint32_t i = 0; i < count && seen != ALL_FIELDS; i++) {
    if (!vorbis_length(r, &at, end, &length) || length > end - at) {
      return;
    }
    uint8_t probe[VORBIS_KEY_PROBE_BYTES];
    size_t probe_len = length < sizeof(probe) ? length : sizeof(probe);
    if (!rd_exact(r, at, probe, probe_len)) {
      return;
    }
    size_t key_len = 0;
    field_t field = vorbis_field(probe, probe_len, &key_len);
    // The first of a repeated key wins, even if it is empty.
    if (field != FIELD_COUNT && !(seen & (1u << field))) {
      seen |= 1u << field;
      read_field(r, at + key_len + 1, length - (uint32_t)key_len - 1, out,
                 field, TEXT_UTF8);
    }
    at += length;
  }
}

static bool read_flac(reader_t *r, tags_t *out) {
  uint8_t b[FLAC_STREAMINFO_BYTES];
  _Static_assert(sizeof(b) >= ID3V2_HEADER_BYTES, "also holds a tag header");
  uint64_t at = 0;

  // The marker, which some taggers put an ID3v2 tag in front of.
  if (!rd_exact(r, at, b, 4)) {
    return false;
  }
  if (memcmp(b, "ID3", 3) == 0) {
    if (!rd_exact(r, at, b, ID3V2_HEADER_BYTES)) {
      return false;
    }
    // (For a header that is not a tag's this is 0, and what is read again
    // is "ID3" and not the marker.)
    at = id3v2_tag_bytes(b);
    if (!rd_exact(r, at, b, 4)) {
      return false;
    }
  }
  if (memcmp(b, "fLaC", 4) != 0) {
    return false;
  }
  at += 4;

  // STREAMINFO is always the first block and always this size; a file
  // without it is not one a decoder would accept (flac_header_read applies
  // the same rule).
  if (!rd_exact(r, at, b, FLAC_BLOCK_HEADER_BYTES)) {
    return false;
  }
  bool last = (b[0] & FLAC_BLOCK_LAST) != 0;
  if ((b[0] & FLAC_BLOCK_TYPE_MASK) != FLAC_BLOCK_STREAMINFO ||
      be24(&b[1]) != FLAC_STREAMINFO_BYTES) {
    return false;
  }
  at += FLAC_BLOCK_HEADER_BYTES;
  if (!rd_exact(r, at, b, FLAC_STREAMINFO_BYTES)) {
    return false;
  }
  at += FLAC_STREAMINFO_BYTES;
  flac_header_t header = {0};
  flac_header_unpack_streaminfo(b, &header);
  out->duration_ms = flac_header_duration_ms(&header);

  // From here on the file is FLAC and has a length: whatever goes wrong
  // with the other blocks only means there are no tags to be had. Every pass
  // uses up four bytes of the file, so the walk ends where the file does at
  // the latest; a block length that points past the end shows up as a
  // header that cannot be read.
  while (!last) {
    if (!rd_exact(r, at, b, FLAC_BLOCK_HEADER_BYTES)) {
      break;
    }
    last = (b[0] & FLAC_BLOCK_LAST) != 0;
    uint32_t length = be24(&b[1]);
    at += FLAC_BLOCK_HEADER_BYTES;
    if ((b[0] & FLAC_BLOCK_TYPE_MASK) == FLAC_BLOCK_VORBIS_COMMENT) {
      read_vorbis_comments(r, at, length, out);
      break; // a stream has one comment block; nothing behind it is wanted
    }
    at += length;
  }
  return true;
}

// --- MP3 ---------------------------------------------------------------------

#define ID3V2_FLAG_UNSYNCHRONISED 0x80
#define ID3V2_FLAG_EXTENDED_HEADER 0x40
#define ID3V2_FRAME_HEADER_BYTES 10
// Frame flags that say the body is not stored as it stands (compressed,
// encrypted, unsynchronised, or with extra bytes in front): the second flag
// byte, whose layout differs between the versions.
#define ID3V23_FRAME_FORMAT_FLAGS 0xE0
#define ID3V24_FRAME_FORMAT_FLAGS 0x4F

static const struct {
  char id[5];
  field_t field;
} ID3_FRAMES[] = {
    {"TIT2", FIELD_TITLE},        {"TPE1", FIELD_ARTIST}, {"TALB", FIELD_ALBUM},
    {"TPE2", FIELD_ALBUM_ARTIST}, {"TRCK", FIELD_TRACK},  {"TPOS", FIELD_DISC},
};

static field_t id3_field(const uint8_t *id) {
  for (size_t k = 0; k < sizeof(ID3_FRAMES) / sizeof(ID3_FRAMES[0]); k++) {
    if (memcmp(id, ID3_FRAMES[k].id, 4) == 0) {
      return ID3_FRAMES[k].field;
    }
  }
  return FIELD_COUNT;
}

// Four 7-bit bytes, as ID3v2 writes sizes that must not look like a frame
// sync. False if a top bit is set, which a real one never has.
static bool syncsafe32(const uint8_t *p, uint32_t *value) {
  if (((p[0] | p[1] | p[2] | p[3]) & 0x80) != 0) {
    return false;
  }
  *value = ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) |
           ((uint32_t)p[2] << 7) | p[3];
  return true;
}

// Read the frames of an ID3v2.3 or v2.4 tag whose ten-byte header has been
// read and whose body ends at `end`. Stops at padding, at the end of the tag,
// when every wanted frame has been seen, or at anything that is not a frame
// that fits in the tag, keeping what came before.
//
// Returns 0, or the offset the walk stopped at if what is there is the start
// of an MPEG audio frame: the tag claimed more than it has, and the audio
// begins here and not where the tag says it ends.
static uint64_t read_id3v2_frames(reader_t *r, unsigned version, uint8_t flags,
                                  uint64_t end, tags_t *out) {
  uint8_t h[ID3V2_FRAME_HEADER_BYTES];
  uint64_t at = ID3V2_HEADER_BYTES; // never passes `end`
  uint32_t size;

  if (flags & ID3V2_FLAG_EXTENDED_HEADER) {
    // v2.3: a plain size that does not count its own four bytes. v2.4: a
    // sync-safe size that does.
    if (end - at < 4 || !rd_exact(r, at, h, 4)) {
      return 0;
    }
    uint64_t skip;
    if (version == 4) {
      if (!syncsafe32(h, &size)) {
        return 0;
      }
      skip = size;
    } else {
      skip = (uint64_t)be32(h) + 4;
    }
    if (skip < 4 || skip > end - at) {
      return 0;
    }
    at += skip;
  }

  // Every pass moves on by at least a frame header, so this ends with the
  // tag whatever the sizes say.
  unsigned seen = 0;
  while (seen != ALL_FIELDS && end - at >= ID3V2_FRAME_HEADER_BYTES) {
    if (!rd_exact(r, at, h, sizeof(h))) {
      return 0;
    }
    // A frame's name is four capitals or digits. A zero is padding, which
    // ends the frames.
    for (int i = 0; i < 4; i++) {
      if (!((h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9'))) {
        uint32_t bitrate;
        return mp3_frame_header(h, sizeof(h), &bitrate) ? at : 0;
      }
    }
    // The size: plain in v2.3, sync-safe in v2.4.
    if (version == 4) {
      if (!syncsafe32(&h[4], &size)) {
        return 0;
      }
    } else {
      size = be32(&h[4]);
    }
    at += sizeof(h);
    if (size > end - at) {
      return 0; // runs past the end of the tag
    }
    field_t field = id3_field(h);
    uint8_t format_flags = h[9] & (version == 4 ? ID3V24_FRAME_FORMAT_FLAGS
                                                : ID3V23_FRAME_FORMAT_FLAGS);
    // A text frame is an encoding byte and the text. The first of a repeated
    // frame wins.
    if (field != FIELD_COUNT && !(seen & (1u << field)) && size >= 1 &&
        format_flags == 0) {
      uint8_t encoding;
      if (!rd_exact(r, at, &encoding, 1)) {
        return 0;
      }
      if (encoding <= TEXT_ENCODING_MAX) {
        seen |= 1u << field;
        read_field(r, at + 1, size - 1, out, field, (text_encoding_t)encoding);
      }
    }
    at += size; // only arithmetic: a picture is never read
  }
  return 0;
}

// One fixed-width ID3v1 field into `text`, unless it already has something:
// ISO-8859-1, ended by a zero or by the field's end, and padded with spaces
// by some taggers.
static void id3v1_text(const uint8_t *p, size_t width, tags_t *out,
                       field_t field, const char *text) {
  if (text[0] != '\0') {
    return;
  }
  size_t n = 0;
  while (n < width && p[n] != 0) {
    n++;
  }
  while (n > 0 && p[n - 1] == ' ') {
    n--;
  }
  set_field(out, field, TEXT_LATIN1, p, n, false);
}

// Fill what is still empty from an ID3v1 tag: "TAG", title, artist and album
// (30 bytes each), year (4), comment (30), genre (1).
static void read_id3v1(const uint8_t tag[ID3V1_TAG_BYTES], tags_t *out) {
  id3v1_text(&tag[3], 30, out, FIELD_TITLE, out->title);
  id3v1_text(&tag[33], 30, out, FIELD_ARTIST, out->artist);
  id3v1_text(&tag[63], 30, out, FIELD_ALBUM, out->album);
  // ID3v1.1 took the comment's last two bytes for a zero and a track number.
  // In ID3v1.0 they are comment text (or both zero).
  if (out->track_no == 0 && tag[125] == 0) {
    out->track_no = tag[126];
  }
}

static bool read_mp3(reader_t *r, tags_t *out) {
  uint8_t b[MP3_XING_PROBE_BYTES];
  _Static_assert(sizeof(b) >= ID3V2_HEADER_BYTES, "also holds a tag header");

  // An ID3v2 tag at the start. Whatever its version, the audio is after it.
  size_t got = rd_read(r, 0, b, ID3V2_HEADER_BYTES);
  if (got == 0) {
    return false; // empty, or cannot be read
  }
  uint64_t audio = (got == ID3V2_HEADER_BYTES) ? id3v2_tag_bytes(b) : 0;
  const bool has_id3v2 = audio > 0;
  uint32_t body;
  if (has_id3v2 && (b[3] == 3 || b[3] == 4) &&
      !(b[5] & ID3V2_FLAG_UNSYNCHRONISED) && syncsafe32(&b[6], &body)) {
    uint64_t stopped = read_id3v2_frames(
        r, b[3], b[5], (uint64_t)ID3V2_HEADER_BYTES + body, out);
    if (stopped != 0) {
      audio = stopped;
    }
  }

  // The first frame: the length, if the encoder left a Xing/Info header.
  // Otherwise its bit rate, to estimate the length from.
  got = rd_read(r, audio, b, sizeof(b));
  uint32_t bitrate;
  const bool has_frame = mp3_frame_header(b, got, &bitrate);
  out->duration_ms = mp3_xing_duration_ms(b, got);
  const bool estimate = has_frame && out->duration_ms == 0 && bitrate > 0;

  // The end of the file is somewhere else on the card, and getting there
  // makes the file system follow the whole cluster chain. Go only for a
  // reason: a field an ID3v1 tag could fill, or the size for the estimate.
  // (Without an ID3v2 tag the fields are empty, so the file's end is always
  // looked at before it is called not an MP3.)
  const bool wanting = out->title[0] == '\0' || out->artist[0] == '\0' ||
                       out->album[0] == '\0' || out->track_no == 0;
  if (!wanting && !estimate) {
    return true;
  }
  uint64_t size;
  if (!rd_end(r, &size)) {
    return has_id3v2 || has_frame;
  }
  // rd_end leaves the last ID3V1_TAG_BYTES bytes in the window. A tag there
  // must lie behind the ID3v2 tag, not be bytes of it.
  const bool has_id3v1 = r->win_len == ID3V1_TAG_BYTES && r->win_at >= audio &&
                         id3v1_tag_begins(r->win);
  if (has_id3v1) {
    read_id3v1(r->win, out);
  }
  if (estimate) {
    // Constant bit rate assumed: audio bytes x 8 / bits per second.
    uint64_t audio_end = size - (has_id3v1 ? ID3V1_TAG_BYTES : 0);
    if (audio_end > audio) {
      uint64_t ms = (audio_end - audio) * 8000 / bitrate; // below 2^47
      out->duration_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    }
  }
  // With no tag of either kind and no frame where the audio should start,
  // there is nothing to say this is an MP3.
  return has_id3v2 || has_frame || has_id3v1;
}

// --- The interface -----------------------------------------------------------

bool tags_read(FILE *f, tags_format_t format, tags_t *out) {
  reader_t r;
  rd_init(&r, f);
  // Every field is empty and terminated from here on, whatever happens.
  // Neither reader puts anything in a field before it has seen what makes
  // the file its format, so a file that is refused leaves them all empty.
  memset(out, 0, sizeof(*out));
  switch (format) {
  case TAGS_FORMAT_FLAC:
    return read_flac(&r, out);
  case TAGS_FORMAT_MP3:
    return read_mp3(&r, out);
  }
  return false;
}

static bool equals_ignoring_case(const char *a, const char *upper) {
  size_t i = 0;
  while (a[i] != '\0' && ascii_upper((uint8_t)a[i]) == upper[i]) {
    i++;
  }
  return a[i] == '\0' && upper[i] == '\0';
}

bool tags_format_from_name(const char *name, tags_format_t *format) {
  // The extension is what follows the last dot of the last path component. A
  // name that starts with its only dot (".flac") is a hidden file with no
  // extension.
  const char *slash = strrchr(name, '/');
  const char *base = slash ? slash + 1 : name;
  const char *dot = strrchr(base, '.');
  if (dot == NULL || dot == base) {
    return false;
  }
  if (equals_ignoring_case(dot, ".FLAC")) {
    *format = TAGS_FORMAT_FLAC;
    return true;
  }
  if (equals_ignoring_case(dot, ".MP3")) {
    *format = TAGS_FORMAT_MP3;
    return true;
  }
  return false;
}
