#include "lib16.h"
#include "playlist.h"
#include "unity.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The fixtures are written by tools/make_test_playlists.py, which says what
// each one holds. Their paths name the tracks of lib16.h, taken as paths from
// the card root, and one more track with a UTF-8 path that this test adds.

#define FIXTURE(name) FIXTURE_DIR "/" name

#define UTF8_PATH "Sigur R\xc3\xb3s/\xc3\x81g\xc3\xa6tis byrjun/01 Intro.flac"

static const library_track_in_t UTF8_ROW =
    LIB16_ROW(UTF8_PATH, "Intro", "Sigur Ros", "Agaetis byrjun", "Sigur Ros",
              0, 1, 100, TAGS_FORMAT_FLAC);

#define TRACKS (LIB16_TRACKS + 1)
static library_track_in_t rows[TRACKS];
static uint32_t arena_words[LIB16_ARENA_BYTES / 4];
static const library_t *lib;

static playlist_work_t work;

// Files a test has open; tearDown closes them, so a failed assertion (which
// jumps out of the test) cannot leak one.
#define OPEN_MAX 16
static FILE *open_files[OPEN_MAX];

void setUp(void) {
  memcpy(rows, LIB16, sizeof(LIB16));
  rows[LIB16_TRACKS] = UTF8_ROW;
  lib = lib16_build_from(arena_words, rows, TRACKS);
  (void)lib16_build; // lib16.h's helpers that this test has no use for
  (void)lib16_title;
  memset(&work, 0xA5, sizeof(work));
  memset(open_files, 0, sizeof(open_files));
}

void tearDown(void) {
  for (int i = 0; i < OPEN_MAX; i++) {
    if (open_files[i] != NULL) {
      fclose(open_files[i]);
      open_files[i] = NULL;
    }
  }
}

static FILE *keep(FILE *f) {
  TEST_ASSERT_NOT_NULL(f);
  for (int i = 0; i < OPEN_MAX; i++) {
    if (open_files[i] == NULL) {
      open_files[i] = f;
      return f;
    }
  }
  TEST_FAIL_MESSAGE("too many open files");
  return NULL;
}

static FILE *fixture(const char *path) { return keep(fopen(path, "rb")); }

// --- What the module asks of stdio -------------------------------------------

// The linker sends every fread and fwrite in this program to the functions
// below (--wrap, see CMakeLists.txt). While `counting` is set they check the
// rule that playlist files move in blocks: every fread asks for at least 4 KB,
// and every fwrite writes at least 4 KB unless it is the last one of the call.
#define BLOCK_MIN 4096u

static bool counting;
static unsigned small_reads;
static unsigned reads, writes;
static bool last_write_small;
static bool small_write_then_more;

size_t __real_fread(void *p, size_t size, size_t n, FILE *f);
size_t __real_fwrite(const void *p, size_t size, size_t n, FILE *f);

size_t __wrap_fread(void *p, size_t size, size_t n, FILE *f) {
  if (counting) {
    reads++;
    if (size * n < BLOCK_MIN) {
      small_reads++;
    }
  }
  return __real_fread(p, size, n, f);
}

size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *f) {
  if (counting) {
    writes++;
    if (last_write_small) {
      small_write_then_more = true;
    }
    last_write_small = size * n < BLOCK_MIN;
  }
  return __real_fwrite(p, size, n, f);
}

static void count_start(void) {
  counting = true;
  small_reads = 0;
  reads = 0;
  writes = 0;
  last_write_small = false;
  small_write_then_more = false;
}

static void count_check(void) {
  counting = false;
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, small_reads, "an fread under 4 KB");
  TEST_ASSERT_FALSE_MESSAGE(small_write_then_more,
                            "an fwrite under 4 KB that was not the last");
}

static playlist_result_t read_list(FILE *in, uint16_t *tracks, uint32_t max,
                                   uint32_t *count) {
  count_start();
  playlist_result_t r = playlist_read(in, lib, tracks, max, count, &work);
  count_check();
  return r;
}

static playlist_result_t edit_list(FILE *in, FILE *out,
                                   const playlist_edit_t *edit) {
  count_start();
  playlist_result_t r = playlist_edit(in, out, lib, edit, &work);
  count_check();
  return r;
}

// --- Bytes in and out ---------------------------------------------------------

#define BIG (128 * 1024)
static unsigned char got[BIG];
static size_t got_len;
static unsigned char want[BIG];
static size_t want_len;
static unsigned char src[BIG];
static size_t src_len;

// The whole of `f`, from its start, into `buf`.
static size_t slurp(FILE *f, unsigned char *buf, size_t cap) {
  TEST_ASSERT_EQUAL_INT(0, fflush(f));
  rewind(f);
  size_t n = fread(buf, 1, cap, f);
  TEST_ASSERT_TRUE(feof(f));
  return n;
}

static void load_fixture(const char *path) {
  FILE *f = fixture(path);
  src_len = slurp(f, src, sizeof(src));
}

// A file holding `len` bytes of `data`, positioned at its start.
static FILE *file_of(const void *data, size_t len) {
  FILE *f = keep(tmpfile());
  TEST_ASSERT_EQUAL_size_t(len, fwrite(data, 1, len, f));
  rewind(f);
  return f;
}

static FILE *file_of_src(void) { return file_of(src, src_len); }

static void want_clear(void) { want_len = 0; }

static void want_add(const void *p, size_t n) {
  TEST_ASSERT_TRUE(want_len + n <= sizeof(want));
  memcpy(want + want_len, p, n);
  want_len += n;
}

static void want_str(const char *s) { want_add(s, strlen(s)); }

static void want_src(size_t from, size_t to) { want_add(src + from, to - from); }

static void assert_got_want(void) {
  TEST_ASSERT_EQUAL_size_t(want_len, got_len);
  if (want_len > 0) {
    TEST_ASSERT_EQUAL_MEMORY(want, got, want_len);
  }
}

// Edit `in` into a fresh file; its bytes end up in `got`.
static playlist_result_t edit_to_got(FILE *in, const playlist_edit_t *edit) {
  FILE *out = keep(tmpfile());
  playlist_result_t r = edit_list(in, out, edit);
  got_len = slurp(out, got, sizeof(got));
  return r;
}

static playlist_edit_t remove_edit(uint32_t entry) {
  playlist_edit_t e = {.kind = PLAYLIST_EDIT_REMOVE, .entry = entry};
  return e;
}

static playlist_edit_t up_edit(uint32_t entry) {
  playlist_edit_t e = {.kind = PLAYLIST_EDIT_MOVE_UP, .entry = entry};
  return e;
}

static playlist_edit_t down_edit(uint32_t entry) {
  playlist_edit_t e = {.kind = PLAYLIST_EDIT_MOVE_DOWN, .entry = entry};
  return e;
}

static playlist_edit_t append_edit(const uint16_t *tracks, uint32_t count) {
  playlist_edit_t e = {
      .kind = PLAYLIST_EDIT_APPEND, .tracks = tracks, .count = count};
  return e;
}

// --- Tracks -------------------------------------------------------------------

static uint16_t track_of(const char *path) {
  uint32_t t = UINT32_MAX;
  TEST_ASSERT_TRUE_MESSAGE(library_find_path(lib, path, &t), path);
  TEST_ASSERT_TRUE(t < PLAYLIST_MISSING);
  return (uint16_t)t;
}

static uint16_t lib16(int row) { return track_of(LIB16[row].path); }

#define MISSING PLAYLIST_MISSING

static uint16_t tracks[PLAYLIST_ENTRIES_MAX];
static uint32_t count;

static void assert_tracks(const uint16_t *expected, uint32_t n) {
  TEST_ASSERT_EQUAL_UINT32(n, count);
  if (n > 0) {
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, tracks, n);
  }
}

// --- Lines --------------------------------------------------------------------

static char paths[2][PLAYLIST_PATH_MAX];

static void assert_paths(const char *line, uint32_t n, const char *first,
                         const char *second) {
  memset(paths, '?', sizeof(paths));
  uint32_t k = playlist_line_paths(line, strlen(line), paths);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(n, k, line);
  if (n >= 1) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(first, paths[0], line);
  }
  if (n >= 2) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(second, paths[1], line);
  }
}

void test_line_paths_absolute_relative_and_comments(void) {
  assert_paths("/Radiohead/Kid A/01 Everything.flac", 1,
               "Radiohead/Kid A/01 Everything.flac", NULL);
  assert_paths("E:\\Music\\a.flac", 1, "Music/a.flac", NULL);
  assert_paths("e:/Music/a.flac", 1, "Music/a.flac", NULL);
  assert_paths("file:///E:/Music/A%20B.flac", 1, "Music/A B.flac", NULL);
  assert_paths("file:///music/x.flac", 1, "music/x.flac", NULL);
  assert_paths("file://localhost/music/x.flac", 1, "music/x.flac", NULL);
  assert_paths("file:///E:/R%C3%B3s.flac", 1, "R\xc3\xb3s.flac", NULL);
  assert_paths("../x", 1, "x", NULL);
  assert_paths("../lib/x.flac", 1, "lib/x.flac", NULL);
  assert_paths("./x", 2, "x", "playlists/x");
  assert_paths("x", 2, "x", "playlists/x");
  assert_paths("a/../b", 2, "b", "playlists/b");
  assert_paths("a//b/./c/", 2, "a/b/c", "playlists/a/b/c");
  assert_paths("../../x", 0, NULL, NULL);
  assert_paths("/../x", 0, NULL, NULL);
  assert_paths("E:/../x", 0, NULL, NULL);
  assert_paths("#EXTINF:100,Artist - Title", 0, NULL, NULL);
  assert_paths("#EXTM3U", 0, NULL, NULL);
  assert_paths("", 0, NULL, NULL);
  assert_paths("http://example.com/a.mp3", 0, NULL, NULL);
  assert_paths("https://example.com/a.mp3", 0, NULL, NULL);
  assert_paths("rtsp://example.com/a", 0, NULL, NULL);
  // A path that is only the root, or only goes in and out, names no file.
  assert_paths("/", 0, NULL, NULL);
  assert_paths("E:\\", 0, NULL, NULL);
  assert_paths(".", 1, "playlists", NULL);
  // Bad escapes match nothing; escapes mean nothing outside a URL.
  assert_paths("file:///E:/A%2.flac", 0, NULL, NULL);
  assert_paths("file:///E:/A%zz.flac", 0, NULL, NULL);
  assert_paths("file:///E:/A%", 0, NULL, NULL);
  assert_paths("file:///E:/A%00.flac", 0, NULL, NULL);
  assert_paths("/A%20B.flac", 1, "A%20B.flac", NULL);

  // `len` ends the line, not a terminator.
  uint32_t k = playlist_line_paths("../abc.flac\r\n", 11, paths);
  TEST_ASSERT_EQUAL_UINT32(1, k);
  TEST_ASSERT_EQUAL_STRING("abc.flac", paths[0]);
  // A NUL inside the line matches nothing.
  k = playlist_line_paths("../a\0b", 6, paths);
  TEST_ASSERT_EQUAL_UINT32(0, k);

  // The longest path that fits, and one byte more.
  char line[PLAYLIST_LINE_MAX + 1];
  memset(line, 'a', sizeof(line));
  memcpy(line, "../", 3);
  k = playlist_line_paths(line, 3 + PLAYLIST_PATH_MAX - 1, paths);
  TEST_ASSERT_EQUAL_UINT32(1, k);
  TEST_ASSERT_EQUAL_size_t(PLAYLIST_PATH_MAX - 1, strlen(paths[0]));
  k = playlist_line_paths(line, 3 + PLAYLIST_PATH_MAX, paths);
  TEST_ASSERT_EQUAL_UINT32(0, k);
  // From the root a relative path of 255 bytes fits; from "playlists/" not.
  k = playlist_line_paths(line + 3, PLAYLIST_PATH_MAX - 1, paths);
  TEST_ASSERT_EQUAL_UINT32(1, k);
  TEST_ASSERT_EQUAL_size_t(PLAYLIST_PATH_MAX - 1, strlen(paths[0]));
}

void test_line_paths_backslashes_and_case(void) {
  assert_paths("..\\Radiohead\\Kid A\\x.flac", 1, "Radiohead/Kid A/x.flac",
               NULL);
  assert_paths("Music\\SUB/Song.FLAC", 2, "Music/SUB/Song.FLAC",
               "playlists/Music/SUB/Song.FLAC");
  assert_paths("\\Music\\a.flac", 1, "Music/a.flac", NULL);
  assert_paths("..\\..\\x", 0, NULL, NULL);
  assert_paths("FILE:///e:/A%2fB%2Fc.flac", 1, "A/B/c.flac", NULL);
  assert_paths("File:///E:\\Music\\A%20b.flac", 1, "Music/A b.flac", NULL);
  assert_paths("file:///E:/A%c3%b3.flac", 1, "A\xc3\xb3.flac", NULL);
  assert_paths("HTTP://example.com/a.mp3", 0, NULL, NULL);
  assert_paths("z:\\A\\B.Mp3", 1, "A/B.Mp3", NULL);
  // Case is kept as written: the index ignores it when it looks.
  assert_paths("../RADIOHEAD/kid a/01 EVERYTHING.FLAC", 1,
               "RADIOHEAD/kid a/01 EVERYTHING.FLAC", NULL);
}

// --- Reading ------------------------------------------------------------------

void test_read_device_made_file(void) {
  FILE *in = fixture(FIXTURE("device.m3u"));
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  const uint16_t expected[] = {lib16(0),  lib16(5),  lib16(10),
                               lib16(15), lib16(11), track_of(UTF8_PATH)};
  assert_tracks(expected, 6);
}

void test_read_computer_made_file(void) {
  FILE *in = fixture(FIXTURE("computer.m3u8"));
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  const uint16_t utf8 = track_of(UTF8_PATH);
  const uint16_t expected[] = {
      lib16(1),  // E:\Radiohead\Kid A\02 Kid A.flac
      lib16(5),  // file:///E:/Bjork/Homogenic/01%20Hunter.flac
      lib16(10), // ..\Queen\Hits\01 Bohemian.mp3, from /playlists/
      lib16(11), // loose/untagged song.flac, from the root
      lib16(3),  // RADIOHEAD/ok computer/01 AIRBAG.MP3
      utf8,      // /Sigur Rós/..., as UTF-8 bytes
      utf8,      // the same as a file:// URL with %C3%B3
      MISSING,   // e:/Gone/Not Here.flac
      lib16(15), // /deep/a/b/c/d/Deep Track.flac
  };
  assert_tracks(expected, 9);
}

void test_read_marks_missing_entries(void) {
  FILE *in = fixture(FIXTURE("missing.m3u"));
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  const uint16_t expected[] = {MISSING,  lib16(1), MISSING, lib16(6),
                               MISSING,  MISSING,  lib16(12)};
  assert_tracks(expected, 7);
}

void test_read_more_than_a_thousand_entries_is_truncated(void) {
  FILE *in = fixture(FIXTURE("over_limit.m3u"));
  // Exactly the limit, so that the sanitizer sees a write past the end.
  static uint16_t exact[PLAYLIST_ENTRIES_MAX];
  count_start();
  playlist_result_t r =
      playlist_read(in, lib, exact, PLAYLIST_ENTRIES_MAX, &count, &work);
  count_check();
  TEST_ASSERT_EQUAL(PLAYLIST_TRUNCATED, r);
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_ENTRIES_MAX, count);
  for (uint32_t i = 0; i < PLAYLIST_ENTRIES_MAX; i++) {
    TEST_ASSERT_EQUAL_UINT16(lib16((int)(i % 16)), exact[i]);
  }

  // A smaller array is a smaller limit.
  rewind(in);
  uint16_t three[3];
  TEST_ASSERT_EQUAL(PLAYLIST_TRUNCATED, read_list(in, three, 3, &count));
  TEST_ASSERT_EQUAL_UINT32(3, count);
  TEST_ASSERT_EQUAL_UINT16(lib16(2), three[2]);

  // Exactly as many entries as fit is not truncated.
  in = fixture(FIXTURE("device.m3u"));
  uint16_t six[6];
  TEST_ASSERT_EQUAL(PLAYLIST_OK, read_list(in, six, 6, &count));
  TEST_ASSERT_EQUAL_UINT32(6, count);
}

// A file of `len` bytes: `head`, then 'x' up to `len` bytes of line, then
// `tail`.
static FILE *file_with_line(const char *head, size_t line_len,
                            const char *tail) {
  src_len = 0;
  memcpy(src, head, strlen(head));
  src_len += strlen(head);
  memset(src + src_len, 'x', line_len);
  src_len += line_len;
  memcpy(src + src_len, tail, strlen(tail));
  src_len += strlen(tail);
  return file_of_src();
}

void test_read_rejects_a_line_that_is_too_long(void) {
  FILE *in = fixture(FIXTURE("too_long.m3u"));
  TEST_ASSERT_EQUAL(PLAYLIST_BAD_FILE,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));

  // The limit counts the line without its end, and without a byte-order mark.
  const uint16_t one[] = {MISSING};
  struct {
    const char *head;
    size_t len;
    const char *tail;
    playlist_result_t result;
  } cases[] = {
      {"", PLAYLIST_LINE_MAX, "\n", PLAYLIST_OK},
      {"", PLAYLIST_LINE_MAX, "\r\n", PLAYLIST_OK},
      {"", PLAYLIST_LINE_MAX, "", PLAYLIST_OK},
      {"", PLAYLIST_LINE_MAX, "\r", PLAYLIST_OK},
      {"\xef\xbb\xbf", PLAYLIST_LINE_MAX, "\r\n", PLAYLIST_OK},
      {"#\n", PLAYLIST_LINE_MAX, "\r\n", PLAYLIST_OK},
      {"", PLAYLIST_LINE_MAX + 1, "\n", PLAYLIST_BAD_FILE},
      {"", PLAYLIST_LINE_MAX + 1, "", PLAYLIST_BAD_FILE},
      {"", PLAYLIST_LINE_MAX, "\r\r\n", PLAYLIST_BAD_FILE},
      {"\xef\xbb\xbf", PLAYLIST_LINE_MAX + 1, "\n", PLAYLIST_BAD_FILE},
      // A byte-order mark is skipped only at the start of the file.
      {"#\n\xef\xbb\xbf", PLAYLIST_LINE_MAX - 2, "\n", PLAYLIST_BAD_FILE},
      // One huge line, several times the work area.
      {"", 5 * PLAYLIST_WORK_BYTES, "", PLAYLIST_BAD_FILE},
      {"", 5 * PLAYLIST_WORK_BYTES, "\n", PLAYLIST_BAD_FILE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    in = file_with_line(cases[i].head, cases[i].len, cases[i].tail);
    TEST_ASSERT_EQUAL_MESSAGE(cases[i].result,
                              read_list(in, tracks, PLAYLIST_ENTRIES_MAX,
                                        &count),
                              cases[i].head);
    if (cases[i].result == PLAYLIST_OK) {
      assert_tracks(one, 1);
    }
    fclose(in);
    for (int f = 0; f < OPEN_MAX; f++) {
      if (open_files[f] == in) {
        open_files[f] = NULL;
      }
    }
  }
}

void test_read_binary_junk_is_all_missing(void) {
  load_fixture(FIXTURE("junk.m3u"));
  // The entries, counted here by the rule: lines that are not empty (once a
  // CR before the LF is taken off) and do not start with '#'.
  uint32_t entries = 0;
  size_t start = 0;
  for (size_t i = 0; i <= src_len; i++) {
    if (i == src_len || src[i] == '\n') {
      size_t len = i - start;
      if (len > 0 && src[start + len - 1] == '\r') {
        len--;
      }
      if (len > 0 && src[start] != '#') {
        entries++;
      }
      start = i + 1;
    }
  }
  TEST_ASSERT_TRUE(entries > 10);
  FILE *in = fixture(FIXTURE("junk.m3u"));
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(entries, count);
  for (uint32_t i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_UINT16(MISSING, tracks[i]);
  }
}

void test_read_empty_file(void) {
  FILE *in = fixture(FIXTURE("empty.m3u"));
  count = 99;
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(0, count);

  // Nothing but comments, blank lines and a byte-order mark is empty too.
  const char text[] = "\xef\xbb\xbf#EXTM3U\r\n\r\n\n#EXTINF:1,a\n#";
  in = file_of(text, sizeof(text) - 1);
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(0, count);
}

void test_read_io_error(void) {
  // A stream that was opened only for writing fails every read.
  FILE *in = keep(fopen("/dev/null", "wb"));
  TEST_ASSERT_EQUAL(PLAYLIST_IO_ERROR,
                    read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(0, count);
}

// Lines that straddle the edge of a block: the work area is read in blocks,
// and a line can be cut anywhere by the edge of one, a CR LF between its
// two bytes. A file of comment lines pushes six entries (one of them a line
// of the longest length) across the place where the first block ends, one
// byte further each time.
static void build_shifted(size_t prefix) {
  src_len = 0;
  size_t left = prefix;
  while (left > 0) {
    size_t line = left > 1002 ? 1000 : left - 2; // "#...\r\n"
    if (left - line - 2 == 1) {
      line--; // never leave a single byte, which cannot be a line
    }
    if (line == 0 && left == 2) {
      memcpy(src + src_len, "\r\n", 2);
      src_len += 2;
      break;
    }
    src[src_len++] = '#';
    memset(src + src_len, 'c', line - 1);
    src_len += line - 1;
    memcpy(src + src_len, "\r\n", 2);
    src_len += 2;
    left -= line + 2;
  }
  TEST_ASSERT_EQUAL_size_t(prefix, src_len);
  static const char *entries[] = {
      "../Radiohead/Kid A/01 Everything.flac\r\n",
      "#EXTINF:100,Bjork - Hunter\r\n",
      "E:\\Bjork\\Homogenic\\01 Hunter.flac\r\n",
      "../Queen/Hits/01 Bohemian.mp3\r\n",
      "../loose/empty.flac\r\n",
  };
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    size_t n = strlen(entries[i]);
    memcpy(src + src_len, entries[i], n);
    src_len += n;
  }
  // The longest line there may be: a missing entry.
  memcpy(src + src_len, "../", 3);
  memset(src + src_len + 3, 'L', PLAYLIST_LINE_MAX - 3);
  src_len += PLAYLIST_LINE_MAX;
  memcpy(src + src_len, "\r\n../deep/a/b/c/d/Deep Track.flac", 33);
  src_len += 33;
}

void test_read_lines_across_block_edges(void) {
  const uint16_t expected[] = {lib16(0),  lib16(5), lib16(10),
                               lib16(13), MISSING,  lib16(15)};
  // Around the end of the first block of a read (the work area less the
  // paths it keeps) and of an edit (half the work area), and up to well past
  // the six entries' length.
  const size_t edges[] = {PLAYLIST_WORK_BYTES - 2 * PLAYLIST_PATH_MAX,
                          PLAYLIST_WORK_BYTES / 2};
  for (size_t e = 0; e < 2; e++) {
    for (size_t shift = 0; shift < 1300; shift++) {
      build_shifted(edges[e] - 1200 + shift);
      FILE *in = file_of_src();
      TEST_ASSERT_EQUAL(PLAYLIST_OK,
                        read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
      assert_tracks(expected, 6);
      fclose(in);
      open_files[0] = NULL;
    }
  }
}

// --- Editing ------------------------------------------------------------------

// blocks.m3u, block by block: the #EXTM3U line, the four entries' blocks and
// the lines after the last entry.
static const char BLOCKS_HEAD[] = "#EXTM3U\n";
static const char *const BLOCKS[] = {
    "#EXTINF:100,Radiohead - Everything\n"
    "../Radiohead/Kid A/01 Everything.flac\n",
    "#EXTINF:100,Bjork - Hunter\n"
    "\n"
    "../Bjork/Homogenic/01 Hunter.flac\n",
    "../Queen/Hits/01 Bohemian.mp3\n",
    "#EXTINF:131,Nobody - Gone\n"
    "../Gone/missing.mp3\n",
};
static const char BLOCKS_TAIL[] = "# a comment after the last entry\n\n";

// The blocks in the order given by `order` (an entry number per character,
// '0' to '3'), with the head and the tail.
static void want_blocks(const char *order) {
  want_clear();
  want_str(BLOCKS_HEAD);
  for (const char *o = order; *o != '\0'; o++) {
    want_str(BLOCKS[*o - '0']);
  }
  want_str(BLOCKS_TAIL);
}

static FILE *blocks_file(void) {
  load_fixture(FIXTURE("blocks.m3u"));
  want_blocks("0123");
  TEST_ASSERT_EQUAL_size_t(want_len, src_len);
  TEST_ASSERT_EQUAL_MEMORY(want, src, src_len);
  return file_of_src();
}

void test_append_to_nothing_makes_a_device_file(void) {
  const uint16_t add[] = {lib16(0), lib16(15), track_of(UTF8_PATH), lib16(0)};
  playlist_edit_t e = append_edit(add, 4);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(NULL, &e));
  want_clear();
  want_str("../Radiohead/Kid A/01 Everything.flac\n"
           "../deep/a/b/c/d/Deep Track.flac\n"
           "../" UTF8_PATH "\n"
           "../Radiohead/Kid A/01 Everything.flac\n");
  assert_got_want();

  // To an empty file the same.
  FILE *in = fixture(FIXTURE("empty.m3u"));
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(in, &e));
  assert_got_want();

  // Appending nothing to nothing is an empty file.
  e = append_edit(NULL, 0);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(NULL, &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);
}

void test_append_after_a_file_without_a_final_newline(void) {
  load_fixture(FIXTURE("no_final_newline.m3u"));
  TEST_ASSERT_NOT_EQUAL('\n', src[src_len - 1]);
  const uint16_t add[] = {lib16(6)};
  playlist_edit_t e = append_edit(add, 1);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &e));
  want_clear();
  want_src(0, src_len);
  want_str("\n../Bjork/Homogenic/02 Joga.flac\n");
  assert_got_want();

  // After one that ends with a newline no other is added; nor after a file
  // that is nothing but a byte-order mark; a last line ending in a lone CR
  // gets its LF.
  const struct {
    const char *in, *out;
  } cases[] = {
      {"../a\n", "../a\n../Bjork/Homogenic/02 Joga.flac\n"},
      {"../a\r\n", "../a\r\n../Bjork/Homogenic/02 Joga.flac\n"},
      {"\xef\xbb\xbf", "\xef\xbb\xbf../Bjork/Homogenic/02 Joga.flac\n"},
      {"#EXTM3U", "#EXTM3U\n../Bjork/Homogenic/02 Joga.flac\n"},
      {"../a\r", "../a\r\n../Bjork/Homogenic/02 Joga.flac\n"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    FILE *in = file_of(cases[i].in, strlen(cases[i].in));
    TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(in, &e));
    want_clear();
    want_str(cases[i].out);
    assert_got_want();
    for (int f = 0; f < OPEN_MAX; f++) {
      if (open_files[f] != NULL) {
        fclose(open_files[f]);
        open_files[f] = NULL;
      }
    }
  }
}

// A device-made file of `n` entries, track i % TRACKS for entry i.
static void build_entries(uint32_t n) {
  src_len = 0;
  char path[PLAYLIST_PATH_MAX];
  for (uint32_t i = 0; i < n; i++) {
    size_t len = library_track_path(lib, i % TRACKS, path, sizeof(path));
    TEST_ASSERT_TRUE(src_len + len + 4 <= sizeof(src));
    memcpy(src + src_len, "../", 3);
    memcpy(src + src_len + 3, path, len);
    src[src_len + 3 + len] = '\n';
    src_len += len + 4;
  }
}

void test_append_refuses_to_pass_the_limit(void) {
  build_entries(PLAYLIST_ENTRIES_MAX - 1);
  const uint16_t add[] = {lib16(1), lib16(2)};

  // One too many: refused, and nothing written.
  playlist_edit_t e = append_edit(add, 2);
  TEST_ASSERT_EQUAL(PLAYLIST_FULL, edit_to_got(file_of_src(), &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);

  // Exactly to the limit.
  e = append_edit(add, 1);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &e));
  want_clear();
  want_src(0, src_len);
  want_str("../Radiohead/Kid A/02 Kid A.flac\n");
  assert_got_want();
  tearDown();

  // More than the limit at once, to nothing.
  static uint16_t many[PLAYLIST_ENTRIES_MAX + 1];
  for (uint32_t i = 0; i <= PLAYLIST_ENTRIES_MAX; i++) {
    many[i] = (uint16_t)(i % TRACKS);
  }
  e = append_edit(many, PLAYLIST_ENTRIES_MAX + 1);
  TEST_ASSERT_EQUAL(PLAYLIST_FULL, edit_to_got(NULL, &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);
  e = append_edit(many, PLAYLIST_ENTRIES_MAX);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(NULL, &e));
  FILE *back = file_of(got, got_len);
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(back, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_ENTRIES_MAX, count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(many, tracks, PLAYLIST_ENTRIES_MAX);

  tearDown();

  // A file already over the limit takes nothing, not even nothing.
  FILE *over = fixture(FIXTURE("over_limit.m3u"));
  e = append_edit(add, 0);
  TEST_ASSERT_EQUAL(PLAYLIST_FULL, edit_to_got(over, &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);

  // A track the index does not have is refused before anything is written.
  const uint16_t bad[] = {lib16(1), TRACKS};
  e = append_edit(bad, 2);
  TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(file_of_src(), &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);
  e = append_edit(NULL, 1);
  TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(NULL, &e));
  TEST_ASSERT_EQUAL_size_t(0, got_len);
}

void test_remove_first_middle_last(void) {
  const struct {
    uint32_t entry;
    const char *order;
  } cases[] = {{0, "123"}, {1, "023"}, {2, "013"}, {3, "012"}};
  for (size_t i = 0; i < 4; i++) {
    playlist_edit_t e = remove_edit(cases[i].entry);
    TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
    want_blocks(cases[i].order);
    assert_got_want();
    tearDown();
  }

  // The only entry of a file without a final newline, and the last of two.
  const struct {
    const char *in;
    uint32_t entry;
    const char *out;
  } more[] = {
      {"../a", 0, ""},
      {"../a\n../b", 1, "../a\n"},
      {"../a\n../b", 0, "../b"},
      {"\xef\xbb\xbf../a\r\n../b\r\n", 0, "\xef\xbb\xbf../b\r\n"},
  };
  for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
    playlist_edit_t e = remove_edit(more[i].entry);
    TEST_ASSERT_EQUAL(PLAYLIST_OK,
                      edit_to_got(file_of(more[i].in, strlen(more[i].in)), &e));
    want_clear();
    want_str(more[i].out);
    assert_got_want();
    tearDown();
  }
}

void test_move_up_and_down_swap_whole_blocks(void) {
  const struct {
    playlist_edit_t edit;
    const char *order;
  } cases[] = {
      {{.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 1}, "1023"},
      {{.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 2}, "0213"},
      {{.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 3}, "0132"},
      {{.kind = PLAYLIST_EDIT_MOVE_DOWN, .entry = 0}, "1023"},
      {{.kind = PLAYLIST_EDIT_MOVE_DOWN, .entry = 1}, "0213"},
      {{.kind = PLAYLIST_EDIT_MOVE_DOWN, .entry = 2}, "0132"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &cases[i].edit));
    want_blocks(cases[i].order);
    assert_got_want();
    tearDown();
  }

  // The last entry with no newline after it: the file still ends without
  // one, and the lines are still lines.
  const struct {
    const char *in;
    playlist_edit_t edit;
    const char *out;
  } more[] = {
      {"../a\n../b", {.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 1}, "../b\n../a"},
      {"../a\r\n../b",
       {.kind = PLAYLIST_EDIT_MOVE_DOWN, .entry = 0},
       "../b\r\n../a"},
      {"#x\n../a\n#y\n../b",
       {.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 1},
       "#y\n../b\n#x\n../a"},
      {"../a\n../b\r", {.kind = PLAYLIST_EDIT_MOVE_UP, .entry = 1},
       "../b\r\n../a"},
  };
  for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
    TEST_ASSERT_EQUAL(PLAYLIST_OK,
                      edit_to_got(file_of(more[i].in, strlen(more[i].in)),
                                  &more[i].edit));
    want_clear();
    want_str(more[i].out);
    assert_got_want();
    FILE *back = file_of(got, got_len);
    TEST_ASSERT_EQUAL(PLAYLIST_OK,
                      read_list(back, tracks, PLAYLIST_ENTRIES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    tearDown();
  }
}

void test_move_past_either_end_is_refused(void) {
  const playlist_edit_t refused[] = {
      up_edit(0),      down_edit(3),           remove_edit(4),
      up_edit(4),      down_edit(4),           remove_edit(UINT32_MAX),
      up_edit(UINT32_MAX), down_edit(UINT32_MAX),
  };
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
    TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(blocks_file(), &refused[i]));
    TEST_ASSERT_EQUAL_size_t(0, got_len);
    TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(NULL, &refused[i]));
    TEST_ASSERT_EQUAL_size_t(0, got_len);
    tearDown();
  }
  // A one-entry file moves neither way.
  playlist_edit_t e = up_edit(0);
  TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(file_of("../a\n", 5), &e));
  e = down_edit(0);
  TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, edit_to_got(file_of("../a\n", 5), &e));
}

void test_extm3u_stays_first(void) {
  // Entry 0 moved down and removed: #EXTM3U is not part of its block.
  playlist_edit_t e = down_edit(0);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
  TEST_ASSERT_EQUAL_MEMORY("#EXTM3U\n#EXTINF:100,Bjork", got, 25);
  e = remove_edit(0);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
  TEST_ASSERT_EQUAL_MEMORY("#EXTM3U\n#EXTINF:100,Bjork", got, 25);

  // Nor is a byte-order mark, with #EXTM3U or without.
  const struct {
    const char *in, *out;
  } cases[] = {
      {"\xef\xbb\xbf#EXTM3U\r\n../a\r\n../b\r\n",
       "\xef\xbb\xbf#EXTM3U\r\n../b\r\n../a\r\n"},
      {"\xef\xbb\xbf../a\n../b\n", "\xef\xbb\xbf../b\n../a\n"},
      {"#EXTM3U\n../a\n../b", "#EXTM3U\n../b\n../a"},
      // Only a first line is the header: a later #EXTM3U is a comment.
      {"../a\n#EXTM3U\n../b\n", "#EXTM3U\n../b\n../a\n"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    e = down_edit(0);
    TEST_ASSERT_EQUAL(PLAYLIST_OK,
                      edit_to_got(file_of(cases[i].in, strlen(cases[i].in)),
                                  &e));
    want_clear();
    want_str(cases[i].out);
    assert_got_want();
    tearDown();
  }
}

void test_lines_after_the_last_entry_stay_last(void) {
  playlist_edit_t e = up_edit(3);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
  want_blocks("0132");
  assert_got_want();
  tearDown();
  e = remove_edit(3);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
  want_blocks("012");
  assert_got_want();
  const uint16_t add[] = {lib16(6)};
  e = append_edit(add, 1);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(blocks_file(), &e));
  want_blocks("0123");
  want_str("../Bjork/Homogenic/02 Joga.flac\n");
  assert_got_want();
}

// computer.m3u8, block by block.
#define CRLF "\r\n"
static const char COMPUTER_HEAD[] = "\xef\xbb\xbf#EXTM3U" CRLF;
static const char *const COMPUTER[] = {
    "#EXTINF:100,Radiohead - Kid A" CRLF
    "E:\\Radiohead\\Kid A\\02 Kid A.flac" CRLF,
    "#EXTINF:100,Bjork - Hunter" CRLF
    "file:///E:/Bjork/Homogenic/01%20Hunter.flac" CRLF,
    CRLF "..\\Queen\\Hits\\01 Bohemian.mp3" CRLF,
    "loose/untagged song.flac" CRLF,
    "RADIOHEAD/ok computer/01 AIRBAG.MP3" CRLF,
    "#EXTINF:100,Sigur Ros - Intro" CRLF "/" UTF8_PATH CRLF,
    "file:///e:/Sigur%20R%C3%B3s/%C3%81g%C3%A6tis%20byrjun/01%20Intro.flac" CRLF,
    "e:/Gone/Not Here.flac" CRLF,
    "/deep/a/b/c/d/Deep Track.flac" CRLF,
};
#define COMPUTER_N (sizeof(COMPUTER) / sizeof(COMPUTER[0]))
static const char COMPUTER_TAIL[] = "# made by hand" CRLF;

// What an edit of computer.m3u8 must give, built from its blocks.
static void want_computer(const playlist_edit_t *e) {
  uint32_t order[COMPUTER_N];
  uint32_t n = COMPUTER_N;
  for (uint32_t i = 0; i < n; i++) {
    order[i] = i;
  }
  if (e->kind == PLAYLIST_EDIT_REMOVE) {
    memmove(order + e->entry, order + e->entry + 1,
            (n - e->entry - 1) * sizeof(order[0]));
    n--;
  } else if (e->kind != PLAYLIST_EDIT_APPEND) {
    uint32_t j = e->kind == PLAYLIST_EDIT_MOVE_UP ? e->entry - 1 : e->entry;
    order[j] = j + 1;
    order[j + 1] = j;
  }
  want_clear();
  want_str(COMPUTER_HEAD);
  for (uint32_t i = 0; i < n; i++) {
    want_str(COMPUTER[order[i]]);
  }
  want_str(COMPUTER_TAIL);
  if (e->kind == PLAYLIST_EDIT_APPEND) {
    char path[PLAYLIST_PATH_MAX];
    for (uint32_t i = 0; i < e->count; i++) {
      library_track_path(lib, e->tracks[i], path, sizeof(path));
      want_str("../");
      want_str(path);
      want_str("\n");
    }
  }
}

void test_edits_keep_every_other_byte(void) {
  load_fixture(FIXTURE("computer.m3u8"));
  playlist_edit_t none = append_edit(NULL, 0);
  want_computer(&none);
  TEST_ASSERT_EQUAL_size_t(want_len, src_len);
  TEST_ASSERT_EQUAL_MEMORY(want, src, src_len);

  for (uint32_t k = 0; k < COMPUTER_N; k++) {
    playlist_edit_t edits[3] = {remove_edit(k), up_edit(k), down_edit(k)};
    for (int i = 0; i < 3; i++) {
      bool possible = !(edits[i].kind == PLAYLIST_EDIT_MOVE_UP && k == 0) &&
                      !(edits[i].kind == PLAYLIST_EDIT_MOVE_DOWN &&
                        k == COMPUTER_N - 1);
      playlist_result_t r = edit_to_got(file_of_src(), &edits[i]);
      if (!possible) {
        TEST_ASSERT_EQUAL(PLAYLIST_NO_ENTRY, r);
        continue;
      }
      TEST_ASSERT_EQUAL(PLAYLIST_OK, r);
      want_computer(&edits[i]);
      assert_got_want();
    }
    tearDown();
  }
  const uint16_t add[] = {lib16(4), track_of(UTF8_PATH)};
  playlist_edit_t e = append_edit(add, 2);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &e));
  want_computer(&e);
  assert_got_want();
  e = append_edit(add, 0);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &e));
  want_computer(&e);
  assert_got_want();
}

// The same edits with the blocks cut by the edges of the work area's blocks.
void test_edit_across_block_edges(void) {
  const size_t edges[] = {PLAYLIST_WORK_BYTES / 2,
                          PLAYLIST_WORK_BYTES - 2 * PLAYLIST_PATH_MAX};
  for (size_t ed = 0; ed < 2; ed++) {
    for (size_t shift = 0; shift < 1300; shift += 3) {
      size_t prefix = edges[ed] - 1200 + shift;
      build_shifted(prefix);
      // The entries' lines start at `prefix`; find each one's end.
      size_t ends[6];
      size_t at = prefix;
      int found = 0;
      for (size_t i = prefix; i < src_len && found < 6; i++) {
        if (src[i] == '\n') {
          if (src[at] != '#') {
            ends[found++] = i + 1;
          }
          at = i + 1;
        }
      }
      TEST_ASSERT_EQUAL_INT(5, found); // the last has no newline
      ends[5] = src_len;
      // Entry 1's block starts after entry 0's line (with its #EXTINF).
      playlist_edit_t up = up_edit(2);
      TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &up));
      want_clear();
      want_src(0, ends[0]);
      want_src(ends[1], ends[2]);
      want_src(ends[0], ends[1]);
      want_src(ends[2], src_len);
      assert_got_want();

      playlist_edit_t rm = remove_edit(4);
      TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &rm));
      want_clear();
      want_src(0, ends[3]);
      want_src(ends[4], src_len);
      assert_got_want();

      // The last two, the last without a newline: it gets the CR LF of the
      // one before.
      playlist_edit_t down = down_edit(4);
      TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of_src(), &down));
      want_clear();
      want_src(0, ends[3]);
      want_src(ends[4], src_len);
      want_str("\r\n");
      want_src(ends[3], ends[4] - 2);
      assert_got_want();
      tearDown();
    }
  }
}

void test_edit_io_error_on_write(void) {
  // A stream opened only for reading fails every write.
  FILE *out = fixture(FIXTURE("empty.m3u"));
  playlist_edit_t e = up_edit(1);
  TEST_ASSERT_EQUAL(PLAYLIST_IO_ERROR, edit_list(blocks_file(), out, &e));
  const uint16_t add[] = {lib16(0)};
  e = append_edit(add, 1);
  TEST_ASSERT_EQUAL(PLAYLIST_IO_ERROR, edit_list(NULL, out, &e));
  // A file bigger than the work area: the failure comes at the first block.
  build_entries(PLAYLIST_ENTRIES_MAX);
  e = remove_edit(500);
  TEST_ASSERT_EQUAL(PLAYLIST_IO_ERROR, edit_list(file_of_src(), out, &e));

  // And reading: a stream opened only for writing.
  FILE *in = keep(fopen("/dev/null", "wb"));
  FILE *ok_out = keep(tmpfile());
  e = remove_edit(0);
  TEST_ASSERT_EQUAL(PLAYLIST_IO_ERROR, edit_list(in, ok_out, &e));
}

void test_device_file_reads_back_as_written(void) {
  static uint16_t add[TRACKS * 3];
  for (uint32_t i = 0; i < TRACKS * 3; i++) {
    add[i] = (uint16_t)((i * 7) % TRACKS);
  }
  playlist_edit_t e = append_edit(add, TRACKS);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(NULL, &e));
  // Then more to that file.
  e = append_edit(add + TRACKS, 2 * TRACKS);
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(file_of(got, got_len), &e));
  FILE *back = file_of(got, got_len);
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(back, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(TRACKS * 3, count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(add, tracks, TRACKS * 3);
}

static double seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void test_thousand_entries_timing(void) {
  build_entries(PLAYLIST_ENTRIES_MAX);
  static uint16_t expected[PLAYLIST_ENTRIES_MAX];
  for (uint32_t i = 0; i < PLAYLIST_ENTRIES_MAX; i++) {
    char path[PLAYLIST_PATH_MAX];
    library_track_path(lib, i % TRACKS, path, sizeof(path));
    expected[i] = track_of(path);
  }
  enum { REPS = 50 };
  FILE *in = file_of_src();

  double t0 = seconds();
  for (int r = 0; r < REPS; r++) {
    rewind(in);
    TEST_ASSERT_EQUAL(PLAYLIST_OK,
                      read_list(in, tracks, PLAYLIST_ENTRIES_MAX, &count));
  }
  double read_us = (seconds() - t0) / REPS * 1e6;
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_ENTRIES_MAX, count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, tracks, PLAYLIST_ENTRIES_MAX);

  FILE *out = keep(tmpfile());
  playlist_edit_t e = down_edit(500);
  t0 = seconds();
  for (int r = 0; r < REPS; r++) {
    rewind(in);
    rewind(out);
    TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_list(in, out, &e));
  }
  double move_us = (seconds() - t0) / REPS * 1e6;
  TEST_ASSERT_EQUAL_INT(0, fflush(out));
  TEST_ASSERT_EQUAL_INT64((long)src_len, ftell(out));
  got_len = slurp(out, got, sizeof(got));
  FILE *back = file_of(got, got_len);
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(back, tracks, PLAYLIST_ENTRIES_MAX, &count));
  uint16_t swap = expected[500];
  expected[500] = expected[501];
  expected[501] = swap;
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_ENTRIES_MAX, count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, tracks, PLAYLIST_ENTRIES_MAX);

  // Remove one, then append it again at the end.
  e = remove_edit(0);
  rewind(back);
  t0 = seconds();
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(back, &e));
  double remove_us = (seconds() - t0) * 1e6;
  e = append_edit(expected, 1);
  FILE *removed = file_of(got, got_len);
  t0 = seconds();
  TEST_ASSERT_EQUAL(PLAYLIST_OK, edit_to_got(removed, &e));
  double append_us = (seconds() - t0) * 1e6;
  back = file_of(got, got_len);
  TEST_ASSERT_EQUAL(PLAYLIST_OK,
                    read_list(back, tracks, PLAYLIST_ENTRIES_MAX, &count));
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_ENTRIES_MAX, count);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(expected + 1, tracks,
                                 PLAYLIST_ENTRIES_MAX - 1);
  TEST_ASSERT_EQUAL_UINT16(expected[0], tracks[PLAYLIST_ENTRIES_MAX - 1]);

  printf("1,000 entries, %zu bytes: read %.0f us, move %.0f us, "
         "remove %.0f us, append %.0f us (host, under ASan)\n",
         src_len, read_us, move_us, remove_us, append_us);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_line_paths_absolute_relative_and_comments);
  RUN_TEST(test_line_paths_backslashes_and_case);
  RUN_TEST(test_read_device_made_file);
  RUN_TEST(test_read_computer_made_file);
  RUN_TEST(test_read_marks_missing_entries);
  RUN_TEST(test_read_more_than_a_thousand_entries_is_truncated);
  RUN_TEST(test_read_rejects_a_line_that_is_too_long);
  RUN_TEST(test_read_binary_junk_is_all_missing);
  RUN_TEST(test_read_empty_file);
  RUN_TEST(test_read_io_error);
  RUN_TEST(test_read_lines_across_block_edges);
  RUN_TEST(test_append_to_nothing_makes_a_device_file);
  RUN_TEST(test_append_after_a_file_without_a_final_newline);
  RUN_TEST(test_append_refuses_to_pass_the_limit);
  RUN_TEST(test_remove_first_middle_last);
  RUN_TEST(test_move_up_and_down_swap_whole_blocks);
  RUN_TEST(test_move_past_either_end_is_refused);
  RUN_TEST(test_extm3u_stays_first);
  RUN_TEST(test_lines_after_the_last_entry_stay_last);
  RUN_TEST(test_edits_keep_every_other_byte);
  RUN_TEST(test_edit_across_block_edges);
  RUN_TEST(test_edit_io_error_on_write);
  RUN_TEST(test_device_file_reads_back_as_written);
  RUN_TEST(test_thousand_entries_timing);
  return UNITY_END();
}
