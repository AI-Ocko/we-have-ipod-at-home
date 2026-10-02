#include "tags.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Expectations for the fixture files come from fixtures/tags/README.md, which
// lists what each one holds byte by byte. Cases with no fixture are built
// here, in `img`, and written to a temporary file.

// --- Files -------------------------------------------------------------------

typedef struct cost cost_t;

// The file a test has open. tearDown closes it, so a failed assertion (which
// jumps out of the test) cannot leak it and trip the leak checker.
static FILE *open_file;
// Where the reads and seeks made on that file are being counted, if anywhere.
static cost_t *counting;

static void reset_seek_behaviour(void);

void setUp(void) {
  open_file = NULL;
  counting = NULL;
  reset_seek_behaviour();
}

void tearDown(void) {
  counting = NULL;
  reset_seek_behaviour();
  if (open_file != NULL) {
    fclose(open_file);
    open_file = NULL;
  }
}

static FILE *open_fixture(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(open_file, path);
  return open_file;
}

// A temporary file holding `n` bytes, positioned at its start.
static FILE *file_of(const uint8_t *bytes, size_t n) {
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = tmpfile();
  TEST_ASSERT_NOT_NULL(open_file);
  if (n > 0) {
    TEST_ASSERT_EQUAL_size_t(n, fwrite(bytes, 1, n, open_file));
  }
  rewind(open_file);
  return open_file;
}

static long size_of(FILE *f) {
  TEST_ASSERT_EQUAL(0, fseek(f, 0, SEEK_END));
  long size = ftell(f);
  rewind(f);
  return size;
}

// --- What a read costs on a slow card ----------------------------------------

// The linker is told to send every fread and fseek call in this program to
// the two functions below (--wrap, see CMakeLists.txt), so they see exactly
// what tags_read asks of the C library: on the device each fread here is a
// request for part of one sector and each fseek a change of position.
struct cost {
  unsigned long bytes; // bytes the reader was given
  unsigned reads;      // fread calls
  unsigned seeks;      // fseek calls
  long furthest;       // the highest offset a read or a seek was made at
  // Reads that asked for bytes of two 512-byte sectors at once, other than
  // the one for the last 128 bytes of a file, which lie where they lie.
  unsigned straddles;
  bool at_tail; // the last seek was made relative to the end
};

size_t __real_fread(void *p, size_t size, size_t n, FILE *f);
int __real_fseek(FILE *f, long offset, int whence);

size_t __wrap_fread(void *p, size_t size, size_t n, FILE *f) {
  cost_t *c = counting;
  if (c != NULL) {
    long at = ftell(f);
    if (at > c->furthest) {
      c->furthest = at;
    }
    if (!c->at_tail && at % 512 + (long)(size * n) > 512) {
      c->straddles++;
    }
    c->at_tail = false;
  }
  size_t got = __real_fread(p, size, n, f);
  if (c != NULL) {
    c->bytes += got * size;
    c->reads++;
  }
  return got;
}

// Set to make seeks behave as they do on the device, where FatFs (behind
// ESP-IDF's VFS) does not refuse a position outside a file opened for
// reading: it reports success and goes to the end of the file.
static bool seeks_are_clamped;

int __wrap_fseek(FILE *f, long offset, int whence) {
  const bool from_end = (whence == SEEK_END);
  if (seeks_are_clamped) {
    long was = ftell(f);
    __real_fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long to = whence == SEEK_SET ? offset : size + offset;
    if (whence == SEEK_CUR) {
      to = was + offset;
    }
    offset = (to < 0 || to > size) ? size : to;
    whence = SEEK_SET;
  }
  int result = __real_fseek(f, offset, whence);
  cost_t *c = counting;
  if (c != NULL) {
    c->seeks++;
    c->at_tail = from_end;
    long at = ftell(f);
    if (result == 0 && at > c->furthest) {
      c->furthest = at;
    }
  }
  return result;
}

static void reset_seek_behaviour(void) { seeks_are_clamped = false; }

// tags_read on `f`, counting what it asks for.
static bool read_counted(FILE *f, tags_format_t format, tags_t *t, cost_t *c) {
  memset(c, 0, sizeof(*c));
  counting = c;
  bool ok = tags_read(f, format, t);
  counting = NULL;
  return ok;
}

// --- Checking the result -----------------------------------------------------

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
static bool utf8_valid(const char *text) {
  const uint8_t *s = (const uint8_t *)text;
  while (*s) {
    unsigned more;
    uint32_t cp, least;
    if (s[0] < 0x80) {
      s++;
      continue;
    } else if ((s[0] & 0xE0) == 0xC0) {
      more = 1, cp = s[0] & 0x1F, least = 0x80;
    } else if ((s[0] & 0xF0) == 0xE0) {
      more = 2, cp = s[0] & 0x0F, least = 0x800;
    } else if ((s[0] & 0xF8) == 0xF0) {
      more = 3, cp = s[0] & 0x07, least = 0x10000;
    } else {
      return false;
    }
    for (unsigned i = 1; i <= more; i++) {
      if ((s[i] & 0xC0) != 0x80) {
        return false; // also catches a string that ends inside a character
      }
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    s += more + 1;
  }
  return true;
}

static void assert_field_is_text(const char *field, const char *name) {
  TEST_ASSERT_NOT_NULL_MESSAGE(memchr(field, '\0', TAGS_TEXT_MAX), name);
  TEST_ASSERT_TRUE_MESSAGE(utf8_valid(field), name);
}

// Whatever was read: every field is terminated and valid UTF-8.
static void assert_all_text(const tags_t *t) {
  assert_field_is_text(t->title, "title");
  assert_field_is_text(t->artist, "artist");
  assert_field_is_text(t->album, "album");
  assert_field_is_text(t->album_artist, "album artist");
}

static void assert_tags(const tags_t *t, const char *title, const char *artist,
                        const char *album, const char *album_artist,
                        unsigned track, unsigned disc) {
  assert_all_text(t);
  TEST_ASSERT_EQUAL_STRING(title, t->title);
  TEST_ASSERT_EQUAL_STRING(artist, t->artist);
  TEST_ASSERT_EQUAL_STRING(album, t->album);
  TEST_ASSERT_EQUAL_STRING(album_artist, t->album_artist);
  TEST_ASSERT_EQUAL_UINT16(track, t->track_no);
  TEST_ASSERT_EQUAL_UINT16(disc, t->disc_no);
}

// tags_read into a struct filled with rubbish first, so that a field the
// reader forgets to set is caught.
static bool read_tags(FILE *f, tags_format_t format, tags_t *t) {
  memset(t, 0xAA, sizeof(*t));
  return tags_read(f, format, t);
}

static void read_fixture(const char *name, tags_format_t format, tags_t *t) {
  TEST_ASSERT_TRUE_MESSAGE(read_tags(open_fixture(name), format, t), name);
}

// --- Building files by hand --------------------------------------------------

static uint8_t img[8192];
static size_t img_len;

static void img_reset(void) {
  memset(img, 0, sizeof(img));
  img_len = 0;
}

static void img_bytes(const void *p, size_t n) {
  TEST_ASSERT_TRUE(img_len + n <= sizeof(img));
  memcpy(&img[img_len], p, n);
  img_len += n;
}

static void img_zeros(size_t n) {
  TEST_ASSERT_TRUE(img_len + n <= sizeof(img));
  img_len += n; // img_reset already zeroed it
}

static void img_text(const char *s) { img_bytes(s, strlen(s)); }

static void img_be32(uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                  (uint8_t)v};
  img_bytes(b, 4);
}

static void img_le32(uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  img_bytes(b, 4);
}

static void put_le32(size_t at, uint32_t v) {
  img[at] = (uint8_t)v;
  img[at + 1] = (uint8_t)(v >> 8);
  img[at + 2] = (uint8_t)(v >> 16);
  img[at + 3] = (uint8_t)(v >> 24);
}

static void put_syncsafe(size_t at, uint32_t v) {
  img[at] = (uint8_t)((v >> 21) & 0x7F);
  img[at + 1] = (uint8_t)((v >> 14) & 0x7F);
  img[at + 2] = (uint8_t)((v >> 7) & 0x7F);
  img[at + 3] = (uint8_t)(v & 0x7F);
}

static FILE *img_file(void) { return file_of(img, img_len); }

// FLAC ........................................................................

static void flac_block_header(int last, int type, uint32_t length) {
  uint8_t h[4] = {(uint8_t)((last ? 0x80 : 0) | type), (uint8_t)(length >> 16),
                  (uint8_t)(length >> 8), (uint8_t)length};
  img_bytes(h, 4);
}

// The marker and a STREAMINFO block: 44.1 kHz, stereo, 16 bits, 4410 samples
// (100 ms).
static void flac_start(int streaminfo_is_last) {
  static const uint8_t body[34] = {
      0x10, 0x00, 0x10, 0x00, 0x00, 0x0E, 0x0F, 0x00, 0x09,
      0xBB, 0x0A, 0xC4, 0x42, 0xF0, 0x00, 0x00, 0x11, 0x3A, // 44100/2/16, 4410
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  };
  img_reset();
  img_text("fLaC");
  flac_block_header(streaminfo_is_last, 0, 34);
  img_bytes(body, 34);
}

static size_t vc_at; // where the comment block's header is
static uint32_t vc_count;

static void vc_open(void) {
  vc_at = img_len;
  vc_count = 0;
  flac_block_header(0, 4, 0);
  img_le32(4);
  img_text("test"); // the vendor string
  img_le32(0);      // the count, filled in by vc_close
}

static void vc_add_bytes(const void *comment, size_t n) {
  img_le32((uint32_t)n);
  img_bytes(comment, n);
  vc_count++;
}

static void vc_add(const char *comment) {
  vc_add_bytes(comment, strlen(comment));
}

// Ends the comment block and, if it is the last block, adds two bytes of
// "audio" (a FLAC frame's sync) after it.
static void vc_close(int last) {
  uint32_t length = (uint32_t)(img_len - vc_at - 4);
  img[vc_at] = (uint8_t)((last ? 0x80 : 0) | 4);
  img[vc_at + 1] = (uint8_t)(length >> 16);
  img[vc_at + 2] = (uint8_t)(length >> 8);
  img[vc_at + 3] = (uint8_t)length;
  put_le32(vc_at + 4 + 4 + 4, vc_count);
  if (last) {
    img_bytes("\xFF\xF8", 2);
  }
}

// A whole FLAC file with these comments.
static FILE *flac_with(const char *const *comments, size_t n) {
  flac_start(0);
  vc_open();
  for (size_t i = 0; i < n; i++) {
    vc_add(comments[i]);
  }
  vc_close(1);
  return img_file();
}

// MP3 .........................................................................

static unsigned id3_version; // of the tag being built: 3 or 4
static size_t id3_at;        // where its header is

static void id3_open(unsigned version, uint8_t flags) {
  uint8_t h[10] = {'I', 'D', '3', (uint8_t)version, 0, flags, 0, 0, 0, 0};
  id3_version = version;
  id3_at = img_len;
  img_bytes(h, 10);
}

// Fills in the tag's size: everything after the header up to here.
static void id3_close(void) {
  put_syncsafe(id3_at + 6, (uint32_t)(img_len - id3_at - 10));
}

static void id3_frame_header(const char *id, uint32_t size, uint8_t flags1,
                             uint8_t flags2) {
  img_bytes(id, 4);
  if (id3_version == 4) {
    img_zeros(4);
    put_syncsafe(img_len - 4, size);
  } else {
    img_be32(size);
  }
  img_bytes(&flags1, 1);
  img_bytes(&flags2, 1);
}

static void id3_frame(const char *id, const void *body, size_t n) {
  id3_frame_header(id, (uint32_t)n, 0, 0);
  img_bytes(body, n);
}

// A text frame: the encoding byte, then `n` bytes of text.
static void id3_text_bytes(const char *id, uint8_t encoding, const void *text,
                           size_t n) {
  id3_frame_header(id, (uint32_t)(n + 1), 0, 0);
  img_bytes(&encoding, 1);
  img_bytes(text, n);
}

static void id3_text(const char *id, uint8_t encoding, const char *text) {
  id3_text_bytes(id, encoding, text, strlen(text));
}

#define FRAME_BYTES 417 // one 128 kbps frame at 44.1 kHz, without padding

// A frame of silence with this header, `bytes` long in all.
static void mp3_frame(uint8_t h1, uint8_t h2, uint8_t h3, size_t bytes) {
  uint8_t h[4] = {0xFF, h1, h2, h3};
  img_bytes(h, 4);
  img_zeros(bytes - 4);
}

// MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo.
static void mp3_audio_frames(unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    mp3_frame(0xFB, 0x90, 0x00, FRAME_BYTES);
  }
}

// The frame an encoder puts first, saying that `frames` frames follow.
// 5 frames are 130.612 ms, as in the fixtures.
static void mp3_info_frame(uint32_t frames) {
  size_t at = img_len;
  mp3_frame(0xFB, 0x90, 0x00, FRAME_BYTES);
  memcpy(&img[at + 36], "Info", 4);
  img[at + 43] = 0x01; // flags: the frame count is present
  img[at + 44] = (uint8_t)(frames >> 24);
  img[at + 45] = (uint8_t)(frames >> 16);
  img[at + 46] = (uint8_t)(frames >> 8);
  img[at + 47] = (uint8_t)frames;
}

// What the fixtures have after their tag: an Info frame and five frames.
static void mp3_audio(void) {
  mp3_info_frame(5);
  mp3_audio_frames(5);
}

static void id3v1_field(const char *text, size_t width, char pad) {
  size_t n = strlen(text);
  TEST_ASSERT_TRUE(n <= width);
  img_bytes(text, n);
  for (size_t i = n; i < width; i++) {
    img_bytes(&pad, 1);
  }
}

// An ID3v1 tag, its fields padded with `pad`. `track` < 0 makes it ID3v1.0:
// thirty bytes of comment and no track number.
static void id3v1(const char *title, const char *artist, const char *album,
                  char pad, int track) {
  img_text("TAG");
  id3v1_field(title, 30, pad);
  id3v1_field(artist, 30, pad);
  id3v1_field(album, 30, pad);
  img_text("1999");
  if (track < 0) {
    // The last character would be track 33 to a reader that does not look
    // for the zero byte ID3v1.1 puts before the track number.
    id3v1_field("thirty characters of comment!!", 30, pad);
  } else {
    uint8_t tail[2] = {0, (uint8_t)track};
    id3v1_field("", 28, pad);
    img_bytes(tail, 2);
  }
  img_bytes("\xFF", 1); // genre
}

// --- FLAC fixtures -----------------------------------------------------------

static void test_flac_full(void) {
  tags_t t;
  read_fixture("flac_full.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t, "Idioteque", "Radiohead", "Kid A", "Radiohead", 8, 1);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_keys_are_case_insensitive(void) {
  tags_t t;
  read_fixture("flac_lowercase_keys.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t, "Idioteque", "Radiohead", "Kid A", "Radiohead", 8, 1);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_track_of_total(void) {
  tags_t t;
  read_fixture("flac_track_of_total.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t, "", "", "", "", 3, 2);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_utf8_is_kept_as_utf8(void) {
  tags_t t;
  read_fixture("flac_utf8.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t,
              "Sigur R\xc3\xb3s \xe2\x80\x93 Hopp\xc3\xadpolla", // 26 bytes
              "Bj\xc3\xb6rk", "\xe4\xbd\xa0\xe5\xa5\xbd", "", 0, 0);
  TEST_ASSERT_EQUAL_size_t(26, strlen(t.title));
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_without_tags_gives_empty_fields_and_a_duration(void) {
  tags_t t;
  read_fixture("flac_no_tags.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_empty_values(void) {
  tags_t t;
  read_fixture("flac_empty_values.flac", TAGS_FORMAT_FLAC, &t);
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_picture_is_stepped_over(void) {
  tags_t t;
  cost_t c;
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(open_fixture("flac_picture_first.flac"),
                                TAGS_FORMAT_FLAC, &t, &c));
  assert_tags(&t, "Picture First", "The Block Order", "Cover Before Comments",
              "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
  // The picture is 206940 bytes of a 209841-byte file.
  TEST_ASSERT_TRUE(c.bytes < 2048);
  // In sectors, which is what the card deals in: the one with the start of
  // the file and the one with the comments.
  TEST_ASSERT_TRUE(c.bytes <= 2 * 512);
  TEST_ASSERT_TRUE(c.reads <= 2);
}

static void test_flac_long_title_is_cut_at_a_character_boundary(void) {
  char whole[601];
  for (int group = 0; group < 60; group++) {
    snprintf(&whole[group * 10], 11, "%03d-abcdef", group);
  }
  tags_t t;
  read_fixture("flac_long_title.flac", TAGS_FORMAT_FLAC, &t);
  assert_all_text(&t);
  size_t n = strlen(t.title);
  TEST_ASSERT_TRUE(n <= TAGS_TEXT_MAX - 1);
  TEST_ASSERT_EQUAL_MEMORY(whole, t.title, n); // a prefix of the original
  // The title is ASCII, where every byte is a character boundary, so all
  // 127 bytes are used.
  TEST_ASSERT_EQUAL_size_t(TAGS_TEXT_MAX - 1, n);
  TEST_ASSERT_EQUAL_STRING("", t.artist);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

// --- MP3 fixtures ------------------------------------------------------------

// 5 frames x 1152 samples at 44.1 kHz are 130.612 ms; the Xing/Info reader
// rounds down.
#define FIXTURE_MS 130

static void test_mp3_id3v23_latin1(void) {
  tags_t t;
  read_fixture("mp3_id3v23_latin1.mp3", TAGS_FORMAT_MP3, &t);
  assert_tags(&t, "Caf\xc3\xa9 del Mar", "Mot\xc3\xb6rhead",
              "D\xc3\xa9j\xc3\xa0 Vu", "Se\xc3\xb1or Coconut", 5, 1);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v23_utf16(void) {
  tags_t t;
  read_fixture("mp3_id3v23_utf16.mp3", TAGS_FORMAT_MP3, &t);
  assert_tags(&t,
              "\xd0\x92\xd0\xbb\xd0\xb0\xd0\xb4\xd0\xb8\xd0\xb2\xd0\xbe\xd1\x81"
              "\xd1\x82\xd0\xbe\xd0\xba 2000",
              "Мумий Тролль",
              "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c",
              "Na\xc3\xafve \xf0\x9d\x84\x9e Trio", 2, 1);
  TEST_ASSERT_EQUAL_size_t(27, strlen(t.title));
  TEST_ASSERT_EQUAL_size_t(23, strlen(t.artist)); // the terminator is not text
  TEST_ASSERT_EQUAL_size_t(12, strlen(t.album));
  TEST_ASSERT_EQUAL_size_t(16, strlen(t.album_artist));
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v24_utf8(void) {
  tags_t t;
  read_fixture("mp3_id3v24_utf8.mp3", TAGS_FORMAT_MP3, &t);
  // Everything after the TXXX frame is lost if its sync-safe size is taken
  // for a plain number.
  assert_tags(&t, "Leyndarm\xc3\xa1l \xe2\x80\x93 \xe7\xa7\x98\xe5\xaf\x86",
              "\xc3\x81sgeir", "Dýrð í dauðaþögn", "Ýmsir flytjendur", 4, 1);
  TEST_ASSERT_EQUAL_size_t(7, strlen(t.artist));
  TEST_ASSERT_EQUAL_size_t(22, strlen(t.album));
  TEST_ASSERT_EQUAL_size_t(17, strlen(t.album_artist));
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v24_utf16be(void) {
  tags_t t;
  read_fixture("mp3_id3v24_utf16be.mp3", TAGS_FORMAT_MP3, &t);
  assert_tags(&t, "Тёмная ночь", "Марк Бернес", "Два бойца – 二人",
              "Разные исполнители", 9, 2);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v1_only(void) {
  tags_t t;
  read_fixture("mp3_id3v1_only.mp3", TAGS_FORMAT_MP3, &t);
  // The album fills all 30 bytes: nothing terminates it but the field's end.
  assert_tags(&t, "F\xc3\xbcr Elise", "Ludwig van Beethoven",
              "Bagatelles and Piano Favorites", "", 7, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v2_wins_over_v1(void) {
  tags_t t;
  read_fixture("mp3_both.mp3", TAGS_FORMAT_MP3, &t);
  // Only the album comes from the ID3v1 tag, whose track is 9.
  assert_tags(&t, "Title From V2", "Artist From V2", "Album Only In V1", "", 3,
              0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_without_tags_has_a_duration_from_xing(void) {
  tags_t t;
  read_fixture("mp3_no_tags.mp3", TAGS_FORMAT_MP3, &t);
  assert_tags(&t, "", "", "", "", 0, 0);
  // Its first frame is 64 kbps and the file 2006 bytes: size and bit rate
  // would say 250 ms.
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_cbr_duration_without_xing(void) {
  tags_t t;
  read_fixture("mp3_no_xing_cbr.mp3", TAGS_FORMAT_MP3, &t);
  assert_tags(&t, "", "", "", "", 0, 0);
  // Within 2 % of the true 130.612 ms.
  TEST_ASSERT_UINT32_WITHIN(2, 130, t.duration_ms);
  // Exactly: 2089 bytes x 8 / 128000 bit/s = 130.56 ms, rounded down.
  TEST_ASSERT_EQUAL_UINT32(130, t.duration_ms);
}

static void test_mp3_apic_is_stepped_over(void) {
  tags_t t;
  cost_t c;
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(
      read_counted(open_fixture("mp3_apic.mp3"), TAGS_FORMAT_MP3, &t, &c));
  assert_tags(&t, "After The Picture", "The Cover Arts", "Front Cover",
              "Picture People", 12, 1);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
  // The picture is 206899 bytes of a 209691-byte file.
  TEST_ASSERT_TRUE(c.bytes < 4096);
  // The sector with the start of the tag, and the one that happens to hold
  // both the text frames and the first audio frame.
  TEST_ASSERT_TRUE(c.bytes <= 2 * 512);
  TEST_ASSERT_TRUE(c.reads <= 2);
}

static void test_mp3_bad_tag_size_does_not_read_out_of_bounds(void) {
  tags_t t;
  cost_t c;
  memset(&t, 0xAA, sizeof(t));
  FILE *f = open_fixture("mp3_bad_size.mp3");
  long size = size_of(f);
  TEST_ASSERT_EQUAL_INT64(2535, size);
  TEST_ASSERT_TRUE(read_counted(f, TAGS_FORMAT_MP3, &t, &c));
  // The one well-formed frame is kept. The walk then stops at something that
  // is not a frame but is the start of the audio, and the length is read
  // there rather than at offset 65546, where the tag claims to end.
  assert_tags(&t, "Bad Size", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
  TEST_ASSERT_TRUE(c.furthest <= size);
  TEST_ASSERT_TRUE(c.bytes <= (unsigned long)size);
}

// --- Not the stated format ---------------------------------------------------

static const uint8_t SOME_TEXT[] =
    "These are the notes that came with the album. They are not audio, "
    "whatever the name of the file says, and a scan must get past them.\n";

static void assert_rejected(FILE *f, tags_format_t format) {
  tags_t t;
  TEST_ASSERT_FALSE(read_tags(f, format, &t));
  // Even then nothing is left unset.
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);
}

static void test_wrong_format_returns_false(void) {
  // A FLAC file holds no MP3 frame (and this one no ID3 tag).
  assert_rejected(open_fixture("flac_full.flac"), TAGS_FORMAT_MP3);
  assert_rejected(open_fixture("flac_picture_first.flac"), TAGS_FORMAT_MP3);
  assert_rejected(file_of(SOME_TEXT, sizeof(SOME_TEXT) - 1), TAGS_FORMAT_FLAC);
  assert_rejected(file_of(SOME_TEXT, sizeof(SOME_TEXT) - 1), TAGS_FORMAT_MP3);
  assert_rejected(open_fixture("mp3_id3v1_only.mp3"), TAGS_FORMAT_FLAC);
  assert_rejected(open_fixture("mp3_id3v24_utf8.mp3"), TAGS_FORMAT_FLAC);
}

static void test_empty_file_returns_false(void) {
  assert_rejected(file_of(NULL, 0), TAGS_FORMAT_FLAC);
  assert_rejected(file_of(NULL, 0), TAGS_FORMAT_MP3);
}

static void test_format_from_name(void) {
  tags_format_t format = (tags_format_t)99;
  TEST_ASSERT_TRUE(tags_format_from_name("a.flac", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_FLAC, format);
  format = (tags_format_t)99;
  TEST_ASSERT_TRUE(tags_format_from_name("A.FLAC", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_FLAC, format);
  format = (tags_format_t)99;
  TEST_ASSERT_TRUE(tags_format_from_name("b.Mp3", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_MP3, format);

  format = (tags_format_t)99;
  TEST_ASSERT_FALSE(tags_format_from_name("c.txt", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("noext", &format));
  // A name that is nothing but ".flac" is a hidden file called "flac" with
  // no extension.
  TEST_ASSERT_FALSE(tags_format_from_name(".flac", &format));
  TEST_ASSERT_FALSE(tags_format_from_name(".mp3", &format));
  TEST_ASSERT_EQUAL(99, format); // left alone when the answer is no

  // Further cases.
  TEST_ASSERT_FALSE(tags_format_from_name("", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("flac", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("a.flac.txt", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("a.flacs", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("a.mp", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("a.", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("album.mp3/cover", &format));
  TEST_ASSERT_FALSE(tags_format_from_name("album/.flac", &format));
  TEST_ASSERT_TRUE(tags_format_from_name("01. Song.v2.fLaC", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_FLAC, format);
  TEST_ASSERT_TRUE(tags_format_from_name("my.album/02 song.MP3", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_MP3, format);
  TEST_ASSERT_TRUE(tags_format_from_name("._resource.mp3", &format));
  TEST_ASSERT_EQUAL(TAGS_FORMAT_MP3, format);
}

// --- ID3 cases with no fixture (coordinator ruling 2)
// -------------------------

static void test_mp3_id3v1_space_padded(void) {
  tags_t t;
  img_reset();
  mp3_audio();
  id3v1("Spaced Out", "The Padders", "Thirty Spaces", ' ', 12);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Spaced Out", "The Padders", "Thirty Spaces", "", 12, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // Spaces and then zeros; a space inside the text is kept, and a field of
  // nothing but padding is empty.
  img_reset();
  mp3_audio();
  img_text("TAG");
  img_bytes("Two Words   \0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 30);
  img_bytes("                              ", 30);
  img_bytes(" Leading space kept           ", 30);
  img_text("1999");
  img_zeros(28);
  img_bytes("\x00\x05\xFF", 3);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Two Words", "", " Leading space kept", "", 5, 0);
}

static void test_mp3_id3v1_0_has_no_track(void) {
  tags_t t;
  img_reset();
  mp3_audio();
  id3v1("Old Tag", "Version One", "Point Zero", ' ', -1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Old Tag", "Version One", "Point Zero", "", 0, 0);

  img_reset();
  mp3_audio();
  id3v1("Old Tag", "Version One", "Point Zero", '\0', -1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Old Tag", "Version One", "Point Zero", "", 0, 0);
}

static void test_mp3_unsynchronised_tag_is_ignored(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0x80);
  id3_text("TIT2", 3, "Ignored Title");
  id3_text("TPE2", 3, "Ignored Album Artist");
  id3_text("TRCK", 3, "8");
  id3_close();
  mp3_audio();
  const size_t without_v1 = img_len;
  id3v1("From V1", "V1 Artist", "V1 Album", '\0', 4);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "From V1", "V1 Artist", "V1 Album", "", 4, 0);
  // The tag is still stepped over to find the audio.
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  img_len = without_v1;
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_extended_header_is_skipped_v23(void) {
  tags_t t;
  // ID3v2.3: a size that does not count itself (6), flags, padding size.
  img_reset();
  id3_open(3, 0x40);
  img_bytes("\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00", 10);
  id3_text("TIT2", 0, "After The Header");
  id3_text("TRCK", 0, "6");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "After The Header", "", "", "", 6, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // With the CRC flag: size 10, four more bytes. The CRC here spells a frame
  // name, which must not be taken for one.
  img_reset();
  id3_open(3, 0x40);
  img_bytes("\x00\x00\x00\x0A\x80\x00\x00\x00\x00\x00TPE1", 14);
  id3_text("TALB", 0, "After The Checksum");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "After The Checksum", "", 0, 0);
}

static void test_mp3_extended_header_is_skipped_v24(void) {
  tags_t t;
  // ID3v2.4: a sync-safe size that counts itself (6), one flag byte, flags.
  img_reset();
  id3_open(4, 0x40);
  img_bytes("\x00\x00\x00\x06\x01\x00", 6);
  id3_text("TIT2", 3, "After The Header");
  id3_text("TPOS", 3, "2");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "After The Header", "", "", "", 0, 2);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // With the CRC flag: size 12, a length byte and five bytes of CRC.
  img_reset();
  id3_open(4, 0x40);
  img_bytes("\x00\x00\x00\x0C\x01\x20\x05TPE1\x00", 12);
  id3_text("TALB", 3, "After The Checksum");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "After The Checksum", "", 0, 0);

  // An extended header that claims more than the tag holds: no frames are
  // read, and nothing else goes wrong.
  img_reset();
  id3_open(4, 0x40);
  img_bytes("\x00\x00\x7F\x7F\x01\x00", 6);
  id3_text("TIT2", 3, "Unreachable");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_id3v22_is_ignored(void) {
  tags_t t;
  img_reset();
  id3_open(2, 0);
  // ID3v2.2 frames: a three-letter name and a three-byte size.
  img_bytes("TT2\x00\x00\x08\x00Ignored", 14);
  img_bytes("TP1\x00\x00\x08\x00Ignored", 14);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  // The tag is still stepped over to find the audio.
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

// --- Hostile ID3v2 tags (coordinator ruling 3)
// --------------------------------

static void test_mp3_frame_running_past_the_tag_end_is_not_read(void) {
  tags_t t;
  for (unsigned version = 3; version <= 4; version++) {
    img_reset();
    id3_open(version, 0);
    id3_text("TIT2", 0, "Kept");
    id3_frame_header("TPE1", 5000, 0, 0); // the tag has 5 bytes left
    img_bytes("\x00Lost", 5);
    id3_close();
    mp3_audio();
    TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
    assert_tags(&t, "Kept", "", "", "", 0, 0);
    TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
  }

  // One byte too long is too long.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Kept");
  id3_frame_header("TPE1", 6, 0, 0);
  img_bytes("\x00Lost", 5);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Kept", "", "", "", 0, 0);

  // The largest size the field can hold, in both versions' notations.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Kept");
  img_bytes("TPE1\xFF\xFF\xFF\xFF\x00\x00\x00Lost", 15);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Kept", "", "", "", 0, 0);

  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 0, "Kept");
  img_bytes("TPE1\x7F\x7F\x7F\x7F\x00\x00\x00Lost", 15);
  id3_text("TALB", 0, "Beyond");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Kept", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // A tag cut off in the middle of a frame header.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Kept");
  img_bytes("TPE1\x00\x00", 6);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Kept", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_frame_of_size_zero_is_stepped_over(void) {
  tags_t t;
  for (unsigned version = 3; version <= 4; version++) {
    img_reset();
    id3_open(version, 0);
    id3_frame("TIT2", "", 0);
    id3_frame("COMM", "", 0);
    id3_text("TPE1", 0, "Still Read");
    id3_close();
    mp3_audio();
    TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
    assert_tags(&t, "", "Still Read", "", "", 0, 0);
    TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
  }
}

static void test_mp3_text_frame_with_only_an_encoding_byte_is_empty(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "");
  id3_text("TPE1", 1, "");
  id3_text("TRCK", 2, "");
  id3_text("TALB", 0, "Not Empty");
  id3_close();
  mp3_audio();
  const size_t without_v1 = img_len;
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "Not Empty", "", 0, 0);

  // An empty field is one the ID3v1 tag may still fill.
  img_len = without_v1;
  id3v1("V1 Title", "V1 Artist", "V1 Album", '\0', 2);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "V1 Title", "V1 Artist", "Not Empty", "", 2, 0);
}

static void test_mp3_utf16_with_an_odd_number_of_bytes(void) {
  tags_t t;
  img_reset();
  id3_open(3, 0);
  id3_text_bytes("TIT2", 1,
                 "\xFF\xFE"
                 "A\0B\0C",
                 7); // half a "C"
  id3_text_bytes("TPE1", 2, "\0D\0E\0", 5);
  id3_text_bytes("TALB", 1, "\xFF", 1); // half a byte order mark
  id3_text_bytes("TPE2", 1, "\xFE\xFF\x4F\x60\x59", 5); // 你 and half of 好
  id3_text_bytes("TRCK", 1,
                 "\xFF\xFE"
                 "7\0"
                 "1",
                 5);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "AB", "DE", "", "\xe4\xbd\xa0", 7, 0);
}

static void test_mp3_utf16_unpaired_surrogate_is_replaced(void) {
  tags_t t;
  img_reset();
  id3_open(3, 0);
  // A high surrogate (D834) with no low one after it.
  id3_text_bytes("TIT2", 1,
                 "\xFF\xFE"
                 "A\0\x34\xD8"
                 "B\0",
                 8);
  // A low surrogate (DD1E) on its own.
  id3_text_bytes("TPE1", 1,
                 "\xFF\xFE"
                 "A\0\x1E\xDD"
                 "B\0",
                 8);
  // A high surrogate as the very last thing in the frame.
  id3_text_bytes("TALB", 2, "\0A\xD8\x34", 4);
  // Two high surrogates, then a proper pair.
  id3_text_bytes("TPE2", 2, "\xD8\x34\xD8\x34\xD8\x34\xDD\x1E", 8);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  // U+FFFD, the replacement character, is EF BF BD.
  assert_tags(&t,
              "A\xEF\xBF\xBD"
              "B",
              "A\xEF\xBF\xBD"
              "B",
              "A\xEF\xBF\xBD", "\xEF\xBF\xBD\xEF\xBF\xBD\xF0\x9D\x84\x9E", 0,
              0);
}

static void test_mp3_latin1_high_bytes_become_two_byte_utf8(void) {
  uint8_t all[128];
  for (unsigned i = 0; i < 128; i++) {
    all[i] = (uint8_t)(0x80 + i);
  }
  tags_t t;
  img_reset();
  id3_open(3, 0);
  id3_text_bytes("TIT2", 0, &all[0], 60);       // 80..BB
  id3_text_bytes("TPE1", 0, &all[60], 63);      // BC..FA
  id3_text_bytes("TALB", 0, &all[123], 5);      // FB..FF
  id3_text_bytes("TPE2", 0, "\xA0\x7F\x80", 3); // around the boundary
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_all_text(&t);

  // U+0080..U+00FF are C2 80..C2 BF and C3 80..C3 BF.
  char want[2 * 128 + 1];
  for (unsigned i = 0; i < 128; i++) {
    want[2 * i] = (char)(i < 64 ? 0xC2 : 0xC3);
    want[2 * i + 1] = (char)(0x80 + (i & 0x3F));
  }
  want[256] = '\0';
  TEST_ASSERT_EQUAL_size_t(120, strlen(t.title));
  TEST_ASSERT_EQUAL_MEMORY(&want[0], t.title, 120);
  TEST_ASSERT_EQUAL_size_t(126, strlen(t.artist));
  TEST_ASSERT_EQUAL_MEMORY(&want[120], t.artist, 126);
  TEST_ASSERT_EQUAL_STRING(&want[246], t.album);
  TEST_ASSERT_EQUAL_STRING("\xC2\xA0\x7F\xC2\x80", t.album_artist);
}

// --- Hostile Vorbis comments (coordinator ruling 3)
// ---------------------------

static void test_flac_comment_length_past_the_block_end_is_not_read(void) {
  tags_t t;
  flac_start(0);
  vc_open();
  vc_add("TITLE=Good");
  img_le32(1000); // claims 1000 bytes; the block holds 14 more
  img_text("ARTIST=Outside");
  vc_count++;
  vc_close(1);
  img_zeros(2000); // "audio": the 1000 bytes are there in the file to read
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "Good", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);

  // The largest length the field can hold, and one that is one byte too long.
  static const uint32_t LENGTHS[] = {0xFFFFFFFFu, 0x80000000u, 15};
  for (size_t i = 0; i < sizeof(LENGTHS) / sizeof(LENGTHS[0]); i++) {
    flac_start(0);
    vc_open();
    vc_add("TITLE=Good");
    img_le32(LENGTHS[i]);
    img_text("ARTIST=Outside");
    vc_count++;
    vc_close(1);
    TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
    assert_tags(&t, "Good", "", "", "", 0, 0);
  }

  // A vendor string that claims to run a little past the block, to where
  // the next block holds what looks like the rest of a comment block.
  flac_start(0);
  vc_open();
  vc_close(0);
  put_le32(vc_at + 4, 4 + 4 + 4); // the block has 8 bytes after this field
  flac_block_header(1, 1, 18);
  img_le32(1);
  img_le32(10);
  img_text("TITLE=Fake");
  img_bytes("\xFF\xF8", 2);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);

  // A vendor string that claims to run far past the block.
  flac_start(0);
  vc_open();
  vc_add("TITLE=Unreachable");
  vc_close(1);
  put_le32(vc_at + 4, 0xFFFFFFF0u);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_comment_count_larger_than_the_block_holds(void) {
  static const uint32_t COUNTS[] = {3, 1000, 0xFFFFFFFFu};
  for (size_t i = 0; i < sizeof(COUNTS) / sizeof(COUNTS[0]); i++) {
    tags_t t;
    flac_start(0);
    vc_open();
    vc_add("TITLE=One");
    vc_add("ARTIST=Two");
    vc_close(0);
    put_le32(vc_at + 4 + 4 + 4, COUNTS[i]);
    // What follows the block looks just like a third comment, but is the
    // next block (padding) and must not be read as one.
    flac_block_header(1, 1, 15);
    img_le32(11);
    img_text("ALBUM=Wrong");
    img_bytes("\xFF\xF8", 2);
    TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
    assert_tags(&t, "One", "Two", "", "", 0, 0);
    TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
  }

  // And a count smaller than what is there: the count is believed.
  tags_t t;
  flac_start(0);
  vc_open();
  vc_add("TITLE=One");
  vc_add("ARTIST=Two");
  vc_close(1);
  put_le32(vc_at + 4 + 4 + 4, 1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "One", "", "", "", 0, 0);
}

static void test_flac_comment_without_equals_is_stepped_over(void) {
  static const char *const COMMENTS[] = {
      "JUSTAKEY",
      "TITLE",
      "",
      "ARTIST",
      "A comment that is "
      "much longer than any key this reader wants and has no equals sign",
      "ARTIST=Found",
      "=no key at all",
      "ALBUM=After",
  };
  tags_t t;
  TEST_ASSERT_TRUE(read_tags(flac_with(COMMENTS, 8), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "Found", "After", "", 0, 0);
}

static void test_flac_comments_after_a_block_past_the_end_of_the_file(void) {
  tags_t t;
  flac_start(0);
  flac_block_header(0, 1, 0xFFFFFF); // 16 MB of padding, it says
  vc_open();
  vc_add("TITLE=Unreachable");
  vc_close(1);
  cost_t c;
  FILE *f = img_file();
  long size = size_of(f);
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(f, TAGS_FORMAT_FLAC, &t, &c));
  // The stream format was read, so the file is FLAC and has a length; what
  // the damaged block hides is simply not there.
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
  TEST_ASSERT_TRUE(c.bytes <= (unsigned long)size);

  // The same when the sum of the lengths would not fit a 32-bit offset.
  flac_start(0);
  for (int i = 0; i < 200; i++) {
    flac_block_header(0, 1, 0xFFFFFF);
  }
  vc_open();
  vc_add("TITLE=Unreachable");
  vc_close(1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

// --- Further FLAC cases ------------------------------------------------------

static void test_flac_first_of_repeated_keys_wins(void) {
  static const char *const COMMENTS[] = {
      "TITLE=First",         "title=Second",       "ARTIST=",
      "ARTIST=Later",        "TRACKNUMBER=4",      "TRACKNUMBER=5",
      "ALBUM ARTIST=Spaced", "ALBUMARTIST=Joined",
  };
  tags_t t;
  TEST_ASSERT_TRUE(read_tags(flac_with(COMMENTS, 8), TAGS_FORMAT_FLAC, &t));
  // An empty first value is still the first.
  assert_tags(&t, "First", "", "", "Spaced", 4, 0);
}

static void test_flac_album_artist_is_spelt_two_ways(void) {
  static const char *const JOINED[] = {"AlbumArtist=Various Artists"};
  static const char *const SPACED[] = {"album artist=Various Artists"};
  tags_t t;
  TEST_ASSERT_TRUE(read_tags(flac_with(JOINED, 1), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "Various Artists", 0, 0);
  TEST_ASSERT_TRUE(read_tags(flac_with(SPACED, 1), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "Various Artists", 0, 0);
}

static void test_flac_keys_that_only_resemble_wanted_ones(void) {
  static const char *const COMMENTS[] = {
      "TITLES=a",          "TITL=b",          "ARTISTS=c",     "ARTISTSORT=d",
      "ALBUMARTISTSORT=e", "ALBUM_ARTIST=f",  "ALBUMS=g",      "TRACKNUMBERS=3",
      "TRACK=4",           "DISC=5",          "DISCNUMBER =6", " TITLE=h",
      "TRACKTOTAL=12",     "ALBUM  ARTIST=i",
  };
  tags_t t;
  TEST_ASSERT_TRUE(read_tags(flac_with(COMMENTS, 14), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
}

static void test_flac_value_is_kept_exactly(void) {
  static const char *const COMMENTS[] = {
      "TITLE=  two spaces each side  ",
      "ARTIST=a=b=c",
      "ALBUM==",
  };
  tags_t t;
  TEST_ASSERT_TRUE(read_tags(flac_with(COMMENTS, 3), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "  two spaces each side  ", "a=b=c", "=", "", 0, 0);

  // A zero byte ends the text, as it would end the C string.
  flac_start(0);
  vc_open();
  vc_add_bytes("TITLE=ab\0cd", 11);
  vc_close(1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "ab", "", "", "", 0, 0);
}

static void test_numbers(void) {
  static const struct {
    const char *text;
    unsigned want;
  } CASES[] = {
      {"8", 8},
      {"08", 8},
      {"008", 8},
      {"3/12", 3},
      {"03/12", 3},
      {" 7", 7},
      {"  07 ", 7},
      {"7 / 9", 7},
      {"12/", 12},
      {"65535", 65535},
      {"0", 0},
      {"", 0},
      {" ", 0},
      {"/12", 0},
      {"abc", 0},
      {"A1", 0},
      {"-1", 0},
      {"+1", 0},
      {"3a", 0},
      {"1.5", 0},
      {"65536", 0},
      {"99999999999999999999", 0},
      {"0x10", 0},
      {"00000000000000000000000000000000000000000000000000000000000000012", 12},
  };
  for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    char track[128], disc[128];
    snprintf(track, sizeof(track), "TRACKNUMBER=%s", CASES[i].text);
    snprintf(disc, sizeof(disc), "DISCNUMBER=%s", CASES[i].text);
    const char *const comments[] = {track, disc};
    tags_t t;
    TEST_ASSERT_TRUE(read_tags(flac_with(comments, 2), TAGS_FORMAT_FLAC, &t));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(CASES[i].want, t.track_no, CASES[i].text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(CASES[i].want, t.disc_no, CASES[i].text);
  }
}

// A number whose digits run up to where the reader stops reading a value may
// not be all there: it is not used, rather than used in part.
static void test_a_number_too_long_to_read_whole_is_not_used(void) {
  static char comment[512];
  tags_t t;
  // 128 zeros and then 1234: the 130 bytes read end after the "12".
  strcpy(comment, "TRACKNUMBER=");
  memset(comment + 12, '0', 128);
  strcpy(comment + 12 + 128, "1234");
  const char *const comments[] = {comment, "DISCNUMBER=2"};
  TEST_ASSERT_TRUE(read_tags(flac_with(comments, 2), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 2);

  // But a number that is complete before that point is.
  strcpy(comment, "TRACKNUMBER=12/");
  memset(comment + 15, '9', 300);
  comment[315] = '\0';
  TEST_ASSERT_TRUE(read_tags(flac_with(comments, 2), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 12, 2);

  // The same in an ID3v2 frame: 125 zeros and 1234 in ISO-8859-1, of which
  // 127 bytes are read.
  img_reset();
  id3_open(3, 0);
  memset(comment, '0', 125);
  strcpy(comment + 125, "1234");
  id3_text("TRCK", 0, comment);
  id3_text("TPOS", 0, "2");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 2);
}

static void test_flac_invalid_utf8_is_made_valid(void) {
  tags_t t;
  flac_start(0);
  vc_open();
  vc_add("TITLE=Bj\xF6rk"); // ISO-8859-1, not UTF-8
  vc_add("ARTIST=a\xC0\x80"
         "b\xED\xA0\x80"
         "c");                          // overlong; a surrogate
  vc_add("ALBUM=\xE4\xBD\xA0\xE5\xA5"); // 你, then two thirds of 好
  vc_add("ALBUMARTIST=\xF5\x80\x80\x80\xF4\x90\x80\x80z"); // past U+10FFFF
  vc_close(1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  // Each byte that cannot be part of a character becomes U+FFFD.
#define BAD "\xEF\xBF\xBD"
  TEST_ASSERT_EQUAL_STRING("Bj" BAD "rk", t.title);
  TEST_ASSERT_EQUAL_STRING("a" BAD BAD "b" BAD BAD BAD "c", t.artist);
  TEST_ASSERT_EQUAL_STRING("\xE4\xBD\xA0" BAD BAD, t.album);
  TEST_ASSERT_EQUAL_STRING(BAD BAD BAD BAD BAD BAD BAD BAD "z", t.album_artist);

  // Longer overlong forms; and the first and last character of each length,
  // which are valid and must come through untouched.
  flac_start(0);
  vc_open();
  vc_add("TITLE=\xE0\x80\xAF");
  vc_add("ARTIST=\xF0\x80\x80\xAF");
  vc_add(
      "ALBUM=\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF"
      "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
  vc_close(1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_STRING(BAD BAD BAD, t.title);
  TEST_ASSERT_EQUAL_STRING(BAD BAD BAD BAD, t.artist);
  TEST_ASSERT_EQUAL_STRING(
      "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF"
      "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF",
      t.album);
#undef BAD
}

static void test_flac_cut_keeps_whole_characters(void) {
  tags_t t;
  static char comment[1024];

  // 100 three-byte characters: 42 of them fit (126 bytes).
  strcpy(comment, "TITLE=");
  for (int i = 0; i < 100; i++) {
    strcat(comment, "\xe4\xbd\xa0");
  }
  const char *const three[] = {comment};
  TEST_ASSERT_TRUE(read_tags(flac_with(three, 1), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_size_t(126, strlen(t.title));
  TEST_ASSERT_EQUAL_MEMORY(comment + 6, t.title, 126);

  // 50 four-byte characters: 31 fit (124 bytes).
  strcpy(comment, "TITLE=");
  for (int i = 0; i < 50; i++) {
    strcat(comment, "\xf0\x9d\x84\x9e");
  }
  const char *const four[] = {comment};
  TEST_ASSERT_TRUE(read_tags(flac_with(four, 1), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_size_t(124, strlen(t.title));
  TEST_ASSERT_EQUAL_MEMORY(comment + 6, t.title, 124);

  // One byte, then two-byte characters: 1 + 63 x 2 = 127, every byte used.
  strcpy(comment, "TITLE=a");
  for (int i = 0; i < 100; i++) {
    strcat(comment, "\xc3\xa9");
  }
  const char *const two[] = {comment};
  TEST_ASSERT_TRUE(read_tags(flac_with(two, 1), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_size_t(127, strlen(t.title));
  TEST_ASSERT_EQUAL_MEMORY(comment + 6, t.title, 127);

  // A value of exactly 127 bytes is not cut, and one of 128 loses a byte.
  strcpy(comment, "TITLE=");
  memset(comment + 6, 'x', 128);
  comment[6 + 127] = '\0';
  const char *const fits[] = {comment};
  TEST_ASSERT_TRUE(read_tags(flac_with(fits, 1), TAGS_FORMAT_FLAC, &t));
  TEST_ASSERT_EQUAL_STRING(comment + 6, t.title);
  comment[6 + 127] = 'x';
  comment[6 + 128] = '\0';
  TEST_ASSERT_TRUE(read_tags(flac_with(fits, 1), TAGS_FORMAT_FLAC, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_size_t(127, strlen(t.title));
}

static void test_flac_behind_an_id3v2_tag(void) {
  tags_t t;
  flac_start(0);
  vc_open();
  vc_add("TITLE=Behind The Tag");
  vc_close(1);
  static uint8_t flac[256];
  size_t flac_len = img_len;
  TEST_ASSERT_TRUE(flac_len <= sizeof(flac));
  memcpy(flac, img, flac_len);

  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "The ID3 Title Is Not Used");
  img_zeros(700); // padding, so the stream starts in the file's third sector
  id3_close();
  img_bytes(flac, flac_len);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "Behind The Tag", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_other_blocks_before_the_comments(void) {
  tags_t t;
  flac_start(0);
  flac_block_header(0, 3, 1800); // a seek table
  img_zeros(1800);
  flac_block_header(0, 2, 0); // an empty application block
  flac_block_header(0, 1, 3); // padding
  img_zeros(3);
  vc_open();
  vc_add("TITLE=Fourth Block");
  vc_add("DISCNUMBER=2");
  vc_close(0);
  flac_block_header(1, 6, 0xFFFFFF); // a picture that is cut off: not needed
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "Fourth Block", "", "", "", 0, 2);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

static void test_flac_that_is_not_quite_flac_returns_false(void) {
  // The marker and nothing else.
  assert_rejected(file_of((const uint8_t *)"fLaC", 4), TAGS_FORMAT_FLAC);
  // The first block is not STREAMINFO.
  flac_start(0);
  img[4] = 4;
  assert_rejected(img_file(), TAGS_FORMAT_FLAC);
  // STREAMINFO of the wrong length.
  flac_start(0);
  img[7] = 33;
  assert_rejected(img_file(), TAGS_FORMAT_FLAC);
  // STREAMINFO cut short.
  flac_start(1);
  img_len -= 1;
  assert_rejected(img_file(), TAGS_FORMAT_FLAC);
  // An ID3v2 tag with something else behind it.
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "An MP3, Really");
  id3_close();
  mp3_audio();
  assert_rejected(img_file(), TAGS_FORMAT_FLAC);
  // An ID3v2 tag that claims to be larger than the file.
  flac_start(1);
  static uint8_t flac[64];
  memcpy(flac, img, 42);
  img_reset();
  id3_open(4, 0);
  put_syncsafe(6, 0x0FFFFFFF);
  img_bytes(flac, 42);
  assert_rejected(img_file(), TAGS_FORMAT_FLAC);
}

static void test_flac_with_only_streaminfo_is_enough(void) {
  tags_t t;
  flac_start(1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);

  // Not marked as the last block, and then the file ends.
  flac_start(0);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

// --- Further MP3 cases -------------------------------------------------------

static void test_mp3_estimate_leaves_both_tags_out(void) {
  tags_t t;
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Constant Bit Rate");
  img_zeros(300); // padding
  id3_close();
  mp3_audio_frames(10); // 4170 bytes at 128 kbps: 260.6 ms
  const size_t without_v1 = img_len;
  id3v1("V1 Title", "V1 Artist", "V1 Album", ' ', 1);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Constant Bit Rate", "V1 Artist", "V1 Album", "", 1, 0);
  TEST_ASSERT_EQUAL_UINT32(260, t.duration_ms);

  img_len = without_v1;
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  TEST_ASSERT_EQUAL_UINT32(260, t.duration_ms);
}

static void test_mp3_estimate_for_mpeg2_and_free_format(void) {
  tags_t t;
  // MPEG-2 Layer III, 64 kbps, 22.05 kHz: 2000 bytes are 250 ms.
  img_reset();
  mp3_frame(0xF3, 0x80, 0x00, 2000);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  TEST_ASSERT_EQUAL_UINT32(250, t.duration_ms);

  // MPEG-2.5 Layer III, 8 kbps, 11.025 kHz: 2000 bytes are 2 s.
  img_reset();
  mp3_frame(0xE3, 0x10, 0x00, 2000);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  TEST_ASSERT_EQUAL_UINT32(2000, t.duration_ms);

  // A "free format" frame states no bit rate: an MP3, of unknown length.
  img_reset();
  mp3_frame(0xFB, 0x00, 0x00, 2000);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);

  // Layer II is not something this reader estimates, or takes for an MP3.
  img_reset();
  mp3_frame(0xFD, 0x90, 0x00, 2000);
  assert_rejected(img_file(), TAGS_FORMAT_MP3);
}

static void test_mp3_tag_and_no_audio(void) {
  tags_t t;
  // A file that ends where its tag does: the tags are still its tags.
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "No Audio");
  id3_close();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "No Audio", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);

  // The last 128 bytes of that file are part of its ID3v2 tag (padding, say,
  // or a frame's contents). Even if they begin with "TAG" they are not an
  // ID3v1 tag.
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "No Audio");
  img_zeros(60);
  id3v1("Not A Tag", "Just Padding", "", '\0', 9);
  id3_close();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "No Audio", "", "", "", 0, 0);

  // Nothing but an ID3v1 tag.
  img_reset();
  id3v1("Only A Tag", "", "", '\0', 0);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Only A Tag", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);

  // A file shorter than an ID3v1 tag that ends in "TAG".
  assert_rejected(file_of((const uint8_t *)"TAG", 3), TAGS_FORMAT_MP3);
  // One audio frame header and nothing more.
  TEST_ASSERT_TRUE(read_tags(file_of((const uint8_t *)"\xFF\xFB\x90\x00", 4),
                             TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "", "", 0, 0);
}

static void test_mp3_frame_with_format_flags_is_stepped_over(void) {
  tags_t t;
  // Compressed, encrypted or otherwise transformed frames are not text that
  // can be read as it stands.
  img_reset();
  id3_open(4, 0);
  id3_frame_header("TIT2", 9, 0x00, 0x08); // compressed
  img_bytes("\x03"
            "Squeezed",
            9);
  id3_frame_header("TPE1", 5, 0x00, 0x02); // unsynchronised
  img_bytes("\x03"
            "Sync",
            5);
  id3_frame_header("TALB", 6, 0x40, 0x00); // a status flag: harmless
  img_bytes("\x03"
            "Plain",
            6);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "Plain", "", 0, 0);

  img_reset();
  id3_open(3, 0);
  id3_frame_header("TIT2", 9, 0x00, 0x80); // compressed, in ID3v2.3
  img_bytes("\x00"
            "Squeezed",
            9);
  id3_frame_header("TALB", 6, 0x80, 0x00);
  img_bytes("\x00"
            "Plain",
            6);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "", "Plain", "", 0, 0);
}

static void test_mp3_text_ends_at_the_first_terminator(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0);
  // ID3v2.4 allows several values with a terminator between them.
  id3_text_bytes("TPE1", 3, "First\0Second", 12);
  id3_text_bytes("TIT2", 1,
                 "\xFF\xFE"
                 "A\0\0\0B\0",
                 8);
  id3_text_bytes("TALB", 0, "\0Hidden", 7);
  id3_text_bytes("TRCK", 0,
                 "9\0"
                 "10",
                 4);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "A", "First", "", "", 9, 0);
}

static void test_mp3_utf16_byte_order(void) {
  tags_t t;
  img_reset();
  id3_open(3, 0);
  // Encoding 1 without a byte order mark: read as big-endian.
  id3_text_bytes("TIT2", 1, "\0A\0B", 4);
  // Encoding 2 with a (redundant) byte order mark: the mark is not text.
  id3_text_bytes("TPE1", 2, "\xFE\xFF\0C", 4);
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "AB", "C", "", "", 0, 0);
}

static void test_mp3_unknown_encoding_leaves_the_field_empty(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 4, "What Encoding");
  id3_text("TPE1", 0xFF, "What Encoding");
  id3_text("TALB", 3, "Known");
  id3_close();
  mp3_audio();
  id3v1("V1 Title", "", "", '\0', 0);
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "V1 Title", "", "Known", "", 0, 0);
}

static void test_mp3_invalid_utf8_is_made_valid(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "Bj\xF6rk");
  id3_text("TPE1", 3, "\xE4\xBD");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Bj\xEF\xBF\xBDrk", "\xEF\xBF\xBD\xEF\xBF\xBD", "", "", 0, 0);
}

static void test_mp3_long_text_is_cut_at_a_character_boundary(void) {
  static uint8_t text[1024];
  tags_t t;
  img_reset();
  id3_open(3, 0);
  // 300 x U+0416 in UTF-16LE: two bytes each in UTF-8, so 63 fit.
  text[0] = 0xFF, text[1] = 0xFE;
  for (int i = 0; i < 300; i++) {
    text[2 + 2 * i] = 0x16, text[3 + 2 * i] = 0x04;
  }
  id3_text_bytes("TIT2", 1, text, 602);
  // 100 x U+1D11E in UTF-16BE: four bytes each, so 31 fit.
  for (int i = 0; i < 100; i++) {
    memcpy(&text[4 * i], "\xD8\x34\xDD\x1E", 4);
  }
  id3_text_bytes("TPE1", 2, text, 400);
  // 300 x "A" in UTF-16BE: all 127 bytes used.
  for (int i = 0; i < 300; i++) {
    text[2 * i] = 0, text[2 * i + 1] = 'A';
  }
  id3_text_bytes("TALB", 2, text, 600);
  // 300 x E9 in ISO-8859-1: two bytes each, so 63 fit.
  memset(text, 0xE9, 300);
  id3_text_bytes("TPE2", 0, text, 300);
  id3_text("TRCK", 0, "11");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_all_text(&t);
  TEST_ASSERT_EQUAL_size_t(126, strlen(t.title));
  TEST_ASSERT_EQUAL_MEMORY("\xD0\x96\xD0\x96", &t.title[122], 4);
  TEST_ASSERT_EQUAL_size_t(124, strlen(t.artist));
  TEST_ASSERT_EQUAL_MEMORY("\xF0\x9D\x84\x9E", &t.artist[120], 4);
  TEST_ASSERT_EQUAL_size_t(127, strlen(t.album));
  TEST_ASSERT_EQUAL_size_t(126, strlen(t.album_artist));
  TEST_ASSERT_EQUAL_MEMORY("\xC3\xA9\xC3\xA9", &t.album_artist[122], 4);
  // The frames after the long ones are still found.
  TEST_ASSERT_EQUAL_UINT16(11, t.track_no);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_first_of_repeated_frames_wins(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0);
  id3_text("TIT2", 3, "First");
  id3_text("TIT2", 3, "Second");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "First", "", "", "", 0, 0);
}

static void test_mp3_what_follows_the_frames_is_not_read_as_frames(void) {
  tags_t t;
  // Padding ends the frames, even if something that looks like a frame
  // comes after it.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Before The Padding");
  img_zeros(20);
  id3_text("TPE1", 0, "In The Padding");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Before The Padding", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // A name that is not a frame name does the same.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Before The Rubbish");
  id3_text("tpe1", 0, "Lower Case");
  id3_text("TALB", 0, "After The Rubbish");
  id3_close();
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Before The Rubbish", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

static void test_mp3_footer_is_stepped_over(void) {
  tags_t t;
  img_reset();
  id3_open(4, 0x10);
  id3_text("TIT2", 3, "With A Footer");
  id3_close();
  img_bytes("3DI\x04\x00\x10\x00\x00\x00\x18", 10);
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "With A Footer", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);

  // The footer is not part of the tag's body: a frame that claims to reach
  // into it runs past the end.
  img_reset();
  id3_open(4, 0x10);
  id3_text("TIT2", 3, "With A Footer");
  id3_frame_header("TPE1", 5 + 4, 0, 0);
  img_bytes("\x03"
            "Into",
            5);
  id3_close();
  img_bytes("3DI\x04\x00\x10\x00\x00\x00\x27", 10);
  mp3_audio();
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "With A Footer", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

// On the device a seek to before the start or past the end of a file does not
// fail (see seeks_are_clamped). Neither may be taken for a position reached.
static void test_seeks_that_do_not_fail_outside_the_file(void) {
  tags_t t;
  // 100 bytes of a 128 kbps frame: 6.25 ms. Looking for an ID3v1 tag 128
  // bytes before the end lands at the end instead; taking that for "size -
  // 128" would make the file 228 bytes and 14 ms.
  img_reset();
  mp3_frame(0xFB, 0x90, 0x00, 100);
  FILE *f = img_file();
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(6, t.duration_ms);

  // Exactly an ID3v1 tag's length, and one byte less.
  img_reset();
  id3v1("Only A Tag", "", "", ' ', 3);
  f = img_file();
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "Only A Tag", "", "", "", 3, 0);
  f = file_of(img + 1, 127);
  seeks_are_clamped = true;
  bool ok = read_tags(f, TAGS_FORMAT_MP3, &t);
  seeks_are_clamped = false;
  TEST_ASSERT_FALSE(ok);

  // A tag that claims to end past the end of the file, with padding where
  // the frames stop: the audio is looked for at the end of the file, where
  // there is none.
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "Kept");
  img_zeros(40);
  put_syncsafe(6, 100000);
  mp3_audio();
  f = img_file();
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "Kept", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);

  // A FLAC block that claims to end past the end of the file.
  flac_start(0);
  flac_block_header(0, 1, 0xFFFFFF);
  vc_open();
  vc_add("TITLE=Unreachable");
  vc_close(1);
  f = img_file();
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_FLAC, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);

  // And the fixtures read the same.
  f = open_fixture("mp3_both.mp3");
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "Title From V2", "Artist From V2", "Album Only In V1", "", 3,
              0);
  f = open_fixture("mp3_bad_size.mp3");
  seeks_are_clamped = true;
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  seeks_are_clamped = false;
  assert_tags(&t, "Bad Size", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
}

// --- Any file, any position --------------------------------------------------

static void test_reads_from_the_start_whatever_the_position(void) {
  tags_t t;
  FILE *f = open_fixture("flac_full.flac");
  TEST_ASSERT_EQUAL(0, fseek(f, 1000, SEEK_SET));
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "Idioteque", "Radiohead", "Kid A", "Radiohead", 8, 1);

  f = open_fixture("mp3_both.mp3");
  TEST_ASSERT_EQUAL(0, fseek(f, 0, SEEK_END));
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Title From V2", "Artist From V2", "Album Only In V1", "", 3,
              0);
  // And a second time on the same stream.
  TEST_ASSERT_TRUE(read_tags(f, TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "Title From V2", "Artist From V2", "Album Only In V1", "", 3,
              0);
}

// A value that the file ends in the middle of is not used in part: half a
// title is worse than the file's name, and half a number is another number.
static void test_a_value_the_file_ends_inside_is_not_used(void) {
  tags_t t;
  flac_start(0);
  vc_open();
  vc_add("ARTIST=Whole");
  vc_add("TRACKNUMBER=12");
  vc_close(1);
  img_len -= 2 + 1; // the "audio", and the "2" of "12"
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "Whole", "", "", 0, 0);

  flac_start(0);
  vc_open();
  vc_add("ARTIST=Whole");
  vc_add("TITLE=Cut Short");
  vc_close(1);
  img_len -= 2 + 5;
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_FLAC, &t));
  assert_tags(&t, "", "Whole", "", "", 0, 0);

  img_reset();
  id3_open(3, 0);
  id3_text("TPE1", 0, "Whole");
  id3_text("TIT2", 0, "Cut Short");
  id3_close();
  img_len -= 5;
  TEST_ASSERT_TRUE(read_tags(img_file(), TAGS_FORMAT_MP3, &t));
  assert_tags(&t, "", "Whole", "", "", 0, 0);
}

static uint8_t whole[4096];

static size_t load_fixture(const char *name) {
  FILE *f = open_fixture(name);
  size_t n = fread(whole, 1, sizeof(whole), f);
  TEST_ASSERT_TRUE_MESSAGE(n > 0 && feof(f), name); // all of it fits
  return n;
}

static const struct {
  const char *name;
  tags_format_t format;
} SMALL[] = {
    {"flac_full.flac", TAGS_FORMAT_FLAC},
    {"flac_utf8.flac", TAGS_FORMAT_FLAC},
    {"flac_long_title.flac", TAGS_FORMAT_FLAC},
    {"mp3_id3v23_latin1.mp3", TAGS_FORMAT_MP3},
    {"mp3_id3v23_utf16.mp3", TAGS_FORMAT_MP3},
    {"mp3_id3v24_utf8.mp3", TAGS_FORMAT_MP3},
    {"mp3_id3v24_utf16be.mp3", TAGS_FORMAT_MP3},
    {"mp3_both.mp3", TAGS_FORMAT_MP3},
    {"mp3_id3v1_only.mp3", TAGS_FORMAT_MP3},
};
#define SMALL_COUNT (sizeof(SMALL) / sizeof(SMALL[0]))

// A file cut off anywhere in its tags gives what was whole, and never
// anything that is not text.
static void test_a_file_cut_at_any_length_is_survived(void) {
  for (size_t i = 0; i < SMALL_COUNT; i++) {
    size_t size = load_fixture(SMALL[i].name);
    size_t most = size < 700 ? size : 700;
    for (size_t n = 0; n <= most; n++) {
      tags_t t;
      memset(&t, 0xAA, sizeof(t));
      bool ok = tags_read(file_of(whole, n), SMALL[i].format, &t);
      assert_all_text(&t);
      if (!ok) {
        assert_tags(&t, "", "", "", "", 0, 0);
      }
    }
  }
}

static uint32_t rng_state = 0x2545F491u;

static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

// Each fixture with a few bytes of its tags changed at random, read as either
// format: whatever comes out is terminated, valid UTF-8, and the sanitizers
// see nothing.
static void test_damaged_tags_are_survived(void) {
  for (size_t i = 0; i < SMALL_COUNT; i++) {
    size_t size = load_fixture(SMALL[i].name);
    size_t span = size < 450 ? size : 450; // where the tags are
    for (int round = 0; round < 400; round++) {
      static uint8_t damaged[sizeof(whole)];
      memcpy(damaged, whole, size);
      unsigned changes = 1 + rng() % 4;
      for (unsigned c = 0; c < changes; c++) {
        size_t at = rng() % span;
        // Half the time an extreme value, which is what breaks length fields.
        damaged[at] =
            (rng() & 1) ? (uint8_t)rng() : (uint8_t)(0xFF * (rng() & 1));
      }
      size_t n = (rng() % 8 == 0) ? rng() % size : size;
      tags_t t;
      memset(&t, 0xAA, sizeof(t));
      tags_format_t format =
          (rng() % 8 == 0) ? (tags_format_t)!SMALL[i].format : SMALL[i].format;
      tags_read(file_of(damaged, n), format, &t);
      assert_all_text(&t);
    }
  }
}

// --- A hostile file cannot make the reader read on and on --------------------

// The file `img` followed by `count` copies of `unit`.
static FILE *img_then_repeated(const void *unit, size_t unit_len,
                               unsigned count) {
  FILE *f = img_file();
  TEST_ASSERT_EQUAL(0, fseek(f, 0, SEEK_END));
  for (unsigned i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_size_t(unit_len, fwrite(unit, 1, unit_len, f));
  }
  rewind(f);
  return f;
}

static void test_a_megabyte_of_tiny_comments_is_not_all_read(void) {
  tags_t t;
  cost_t c;
  // A comment block of 250000 comments of length 0 (1 MB), none wanted.
  flac_start(0);
  flac_block_header(1, 4, 12 + 1000000);
  img_le32(4);
  img_text("test");
  img_le32(250000);
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(img_then_repeated("\0\0\0\0", 4, 250000),
                                TAGS_FORMAT_FLAC, &t, &c));
  assert_tags(&t, "", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
  printf("1 MB of empty comments: %lu bytes read, %u reads, %u seeks\n",
         c.bytes, c.reads, c.seeks);
  TEST_ASSERT_TRUE(c.bytes <= 64 * 1024);
}

static void test_a_megabyte_of_tiny_frames_is_not_all_read(void) {
  tags_t t;
  cost_t c;
  // An ID3v2.3 tag of 90000 one-byte frames that are not wanted (990 kB).
  img_reset();
  id3_open(3, 0);
  put_syncsafe(6, 90000 * 11);
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(
      read_counted(img_then_repeated("TXXX\0\0\0\1\0\0\0", 11, 90000),
                   TAGS_FORMAT_MP3, &t, &c));
  assert_tags(&t, "", "", "", "", 0, 0);
  printf("1 MB of one-byte frames: %lu bytes read, %u reads, %u seeks\n",
         c.bytes, c.reads, c.seeks);
  TEST_ASSERT_TRUE(c.bytes <= 64 * 1024);
}

// --- Reading no more than is needed
// -------------------------------------------

static void test_flac_reading_stops_when_every_field_has_been_seen(void) {
  tags_t t;
  cost_t c;
  flac_start(0);
  vc_open();
  vc_add("TITLE=t");
  vc_add("ARTIST=a");
  vc_add("ALBUM=b");
  vc_add("ALBUMARTIST=c");
  vc_add("TRACKNUMBER=1");
  vc_add("DISCNUMBER=2");
  // Three sectors of other comments, and other blocks, behind them.
  for (int i = 0; i < 60; i++) {
    vc_add("COMMENT=nothing that this reader wants");
  }
  vc_close(0);
  flac_block_header(1, 1, 1000);
  img_zeros(1000);
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(img_file(), TAGS_FORMAT_FLAC, &t, &c));
  assert_tags(&t, "t", "a", "b", "c", 1, 2);
  TEST_ASSERT_EQUAL_UINT(1, c.reads);
  TEST_ASSERT_EQUAL_UINT(512, c.bytes);
}

static void test_sectors_in_a_row_are_read_without_seeking(void) {
  tags_t t;
  cost_t c;
  flac_start(0);
  vc_open();
  // Small comments that are not wanted, over three sectors, and then one
  // that is: every sector on the way has to be read, but they follow one
  // another, which the card does fastest when it is not told where to go.
  for (int i = 0; i < 60; i++) {
    vc_add("COMMENT=nothing that this reader wants");
  }
  vc_add("TITLE=At The End");
  vc_close(1);
  TEST_ASSERT_TRUE(img_len > 5 * 512 && img_len < 6 * 512);
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(img_file(), TAGS_FORMAT_FLAC, &t, &c));
  assert_tags(&t, "At The End", "", "", "", 0, 0);
  TEST_ASSERT_EQUAL_UINT(6, c.reads);
  TEST_ASSERT_EQUAL_UINT(1, c.seeks); // to the start of the file
  TEST_ASSERT_EQUAL_UINT(0, c.straddles);
}

static void test_mp3_reading_stops_when_every_frame_has_been_seen(void) {
  tags_t t;
  cost_t c;
  img_reset();
  id3_open(3, 0);
  id3_text("TIT2", 0, "t");
  id3_text("TPE1", 0, "a");
  id3_text("TALB", 0, "b");
  id3_text("TPE2", 0, "c");
  id3_text("TRCK", 0, "1");
  id3_text("TPOS", 0, "2");
  // Two large frames behind them: the header of the second is in a sector
  // of its own, which nothing needs.
  id3_frame_header("PRIV", 2000, 0, 0);
  img_zeros(2000);
  id3_frame_header("GEOB", 2000, 0, 0);
  img_zeros(2000);
  id3_close();
  mp3_audio();
  memset(&t, 0xAA, sizeof(t));
  TEST_ASSERT_TRUE(read_counted(img_file(), TAGS_FORMAT_MP3, &t, &c));
  assert_tags(&t, "t", "a", "b", "c", 1, 2);
  TEST_ASSERT_EQUAL_UINT32(FIXTURE_MS, t.duration_ms);
  // The start of the tag and the start of the audio. Nothing is wanted from
  // the end of the file either.
  TEST_ASSERT_EQUAL_UINT(2, c.reads);
  TEST_ASSERT_EQUAL_UINT(2, c.seeks);
}

// --- What each fixture costs to read -----------------------------------------

static void test_what_each_fixture_costs(void) {
  // The most each file may cost, in bytes and in read requests. The card
  // deals in 512-byte sectors and so does the reader. 512: everything wanted
  // is in the file's first sector. 1024: the tags are behind a picture, in
  // one more sector. 640: the last 128 bytes of the file were needed as
  // well, for an ID3v1 tag or the file's size.
  static const struct {
    const char *name;
    tags_format_t format;
    unsigned long bytes;
    unsigned reads;
  } COSTS[] = {
      {"flac_full.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_lowercase_keys.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_track_of_total.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_utf8.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_no_tags.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_empty_values.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"flac_picture_first.flac", TAGS_FORMAT_FLAC, 1024, 2},
      {"flac_long_title.flac", TAGS_FORMAT_FLAC, 512, 1},
      {"mp3_id3v23_latin1.mp3", TAGS_FORMAT_MP3, 512, 1},
      {"mp3_id3v23_utf16.mp3", TAGS_FORMAT_MP3, 512, 1},
      {"mp3_id3v24_utf8.mp3", TAGS_FORMAT_MP3, 512, 1},
      {"mp3_id3v24_utf16be.mp3", TAGS_FORMAT_MP3, 512, 1},
      {"mp3_id3v1_only.mp3", TAGS_FORMAT_MP3, 640, 2},
      {"mp3_both.mp3", TAGS_FORMAT_MP3, 640, 2},
      {"mp3_no_tags.mp3", TAGS_FORMAT_MP3, 640, 2},
      {"mp3_no_xing_cbr.mp3", TAGS_FORMAT_MP3, 640, 2},
      {"mp3_apic.mp3", TAGS_FORMAT_MP3, 1024, 2},
      {"mp3_bad_size.mp3", TAGS_FORMAT_MP3, 640, 2},
  };
  printf("%-26s %8s %8s %6s %6s\n", "file", "size", "bytes", "reads", "seeks");
  for (size_t i = 0; i < sizeof(COSTS) / sizeof(COSTS[0]); i++) {
    tags_t t;
    cost_t c;
    FILE *f = open_fixture(COSTS[i].name);
    long size = size_of(f);
    memset(&t, 0xAA, sizeof(t));
    TEST_ASSERT_TRUE_MESSAGE(read_counted(f, COSTS[i].format, &t, &c),
                             COSTS[i].name);
    printf("%-26s %8ld %8lu %6u %6u\n", COSTS[i].name, size, c.bytes, c.reads,
           c.seeks);
    TEST_ASSERT_TRUE_MESSAGE(c.bytes <= COSTS[i].bytes, COSTS[i].name);
    TEST_ASSERT_TRUE_MESSAGE(c.reads <= COSTS[i].reads, COSTS[i].name);
    // One seek to get to the start, and at most one before each later read.
    TEST_ASSERT_TRUE_MESSAGE(c.seeks <= COSTS[i].reads, COSTS[i].name);
    TEST_ASSERT_TRUE_MESSAGE(c.furthest <= size, COSTS[i].name);
    // No request crosses a sector boundary.
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, c.straddles, COSTS[i].name);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_flac_full);
  RUN_TEST(test_flac_keys_are_case_insensitive);
  RUN_TEST(test_flac_track_of_total);
  RUN_TEST(test_flac_utf8_is_kept_as_utf8);
  RUN_TEST(test_flac_without_tags_gives_empty_fields_and_a_duration);
  RUN_TEST(test_flac_empty_values);
  RUN_TEST(test_flac_picture_is_stepped_over);
  RUN_TEST(test_flac_long_title_is_cut_at_a_character_boundary);
  RUN_TEST(test_mp3_id3v23_latin1);
  RUN_TEST(test_mp3_id3v23_utf16);
  RUN_TEST(test_mp3_id3v24_utf8);
  RUN_TEST(test_mp3_id3v24_utf16be);
  RUN_TEST(test_mp3_id3v1_only);
  RUN_TEST(test_mp3_id3v2_wins_over_v1);
  RUN_TEST(test_mp3_without_tags_has_a_duration_from_xing);
  RUN_TEST(test_mp3_cbr_duration_without_xing);
  RUN_TEST(test_mp3_apic_is_stepped_over);
  RUN_TEST(test_mp3_bad_tag_size_does_not_read_out_of_bounds);
  RUN_TEST(test_wrong_format_returns_false);
  RUN_TEST(test_empty_file_returns_false);
  RUN_TEST(test_format_from_name);
  RUN_TEST(test_mp3_id3v1_space_padded);
  RUN_TEST(test_mp3_id3v1_0_has_no_track);
  RUN_TEST(test_mp3_unsynchronised_tag_is_ignored);
  RUN_TEST(test_mp3_extended_header_is_skipped_v23);
  RUN_TEST(test_mp3_extended_header_is_skipped_v24);
  RUN_TEST(test_mp3_id3v22_is_ignored);
  RUN_TEST(test_mp3_frame_running_past_the_tag_end_is_not_read);
  RUN_TEST(test_mp3_frame_of_size_zero_is_stepped_over);
  RUN_TEST(test_mp3_text_frame_with_only_an_encoding_byte_is_empty);
  RUN_TEST(test_mp3_utf16_with_an_odd_number_of_bytes);
  RUN_TEST(test_mp3_utf16_unpaired_surrogate_is_replaced);
  RUN_TEST(test_mp3_latin1_high_bytes_become_two_byte_utf8);
  RUN_TEST(test_flac_comment_length_past_the_block_end_is_not_read);
  RUN_TEST(test_flac_comment_count_larger_than_the_block_holds);
  RUN_TEST(test_flac_comment_without_equals_is_stepped_over);
  RUN_TEST(test_flac_comments_after_a_block_past_the_end_of_the_file);
  RUN_TEST(test_flac_first_of_repeated_keys_wins);
  RUN_TEST(test_flac_album_artist_is_spelt_two_ways);
  RUN_TEST(test_flac_keys_that_only_resemble_wanted_ones);
  RUN_TEST(test_flac_value_is_kept_exactly);
  RUN_TEST(test_numbers);
  RUN_TEST(test_a_number_too_long_to_read_whole_is_not_used);
  RUN_TEST(test_flac_invalid_utf8_is_made_valid);
  RUN_TEST(test_flac_cut_keeps_whole_characters);
  RUN_TEST(test_flac_behind_an_id3v2_tag);
  RUN_TEST(test_flac_other_blocks_before_the_comments);
  RUN_TEST(test_flac_that_is_not_quite_flac_returns_false);
  RUN_TEST(test_flac_with_only_streaminfo_is_enough);
  RUN_TEST(test_mp3_estimate_leaves_both_tags_out);
  RUN_TEST(test_mp3_estimate_for_mpeg2_and_free_format);
  RUN_TEST(test_mp3_tag_and_no_audio);
  RUN_TEST(test_mp3_frame_with_format_flags_is_stepped_over);
  RUN_TEST(test_mp3_text_ends_at_the_first_terminator);
  RUN_TEST(test_mp3_utf16_byte_order);
  RUN_TEST(test_mp3_unknown_encoding_leaves_the_field_empty);
  RUN_TEST(test_mp3_invalid_utf8_is_made_valid);
  RUN_TEST(test_mp3_long_text_is_cut_at_a_character_boundary);
  RUN_TEST(test_mp3_first_of_repeated_frames_wins);
  RUN_TEST(test_mp3_what_follows_the_frames_is_not_read_as_frames);
  RUN_TEST(test_mp3_footer_is_stepped_over);
  RUN_TEST(test_seeks_that_do_not_fail_outside_the_file);
  RUN_TEST(test_a_value_the_file_ends_inside_is_not_used);
  RUN_TEST(test_reads_from_the_start_whatever_the_position);
  RUN_TEST(test_a_file_cut_at_any_length_is_survived);
  RUN_TEST(test_damaged_tags_are_survived);
  RUN_TEST(test_a_megabyte_of_tiny_comments_is_not_all_read);
  RUN_TEST(test_a_megabyte_of_tiny_frames_is_not_all_read);
  RUN_TEST(test_flac_reading_stops_when_every_field_has_been_seen);
  RUN_TEST(test_sectors_in_a_row_are_read_without_seeking);
  RUN_TEST(test_mp3_reading_stops_when_every_frame_has_been_seen);
  RUN_TEST(test_what_each_fixture_costs);
  return UNITY_END();
}
