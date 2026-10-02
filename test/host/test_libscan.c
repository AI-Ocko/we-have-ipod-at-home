#include "libscan.h"
#include "library.h"
#include "tags.h"
#include "unity.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Expectations for the fixture tree come from fixtures/lib/README.md, which
// lists every file and its tags.
//
// The two files there with a music extension and no music in them
// (loose/empty.flac, loose/text.mp3) are indexed under their file names:
// the rule is that a file that opens is listed, and only one that cannot be
// opened or read is counted as unreadable. So the tree gives 16 tracks.

// The arena the device uses: 1.5 MB. Words, so that it is aligned like a
// block from the device's allocator.
#define ARENA_BYTES 1572864u
static uint32_t arena_words[ARENA_BYTES / 4];
#define ARENA ((void *)arena_words)

static const library_t *lib;
static libscan_stats_t stats;

// A scratch tree for the tests the fixtures cannot serve. tearDown removes
// it, so a failed assertion leaves nothing behind.
static char tmp_root[64];
static bool tmp_made;
static char tmp_path[1024];

static void remove_tree(char *path, size_t len, size_t cap) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    int n = snprintf(path + len, cap - len, "/%s", ent->d_name);
    if (n < 0 || (size_t)n >= cap - len) {
      continue;
    }
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      remove_tree(path, len + (size_t)n, cap);
      path[len + (size_t)n] = '\0';
      rmdir(path);
    } else {
      unlink(path);
    }
  }
  path[len] = '\0';
  closedir(dir);
}

void setUp(void) {
  lib = NULL;
  memset(&stats, 0xA5, sizeof(stats)); // the scan must set every field
  tmp_made = false;
}

void tearDown(void) {
  if (tmp_made) {
    static char path[2048];
    snprintf(path, sizeof(path), "%s", tmp_root);
    remove_tree(path, strlen(path), sizeof(path));
    rmdir(tmp_root);
    tmp_made = false;
  }
}

static void tmp_begin(void) {
  const char *base = getenv("TMPDIR");
  snprintf(tmp_root, sizeof(tmp_root), "%s/libscanXXXXXX",
           (base && strlen(base) < 40) ? base : "/tmp");
  TEST_ASSERT_NOT_NULL(mkdtemp(tmp_root));
  tmp_made = true;
}

static const char *tmp_join(const char *rel) {
  int n = snprintf(tmp_path, sizeof(tmp_path), "%s/%s", tmp_root, rel);
  TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(tmp_path));
  return tmp_path;
}

static void tmp_dir(const char *rel) {
  TEST_ASSERT_EQUAL_INT(0, mkdir(tmp_join(rel), 0700));
}

// An empty file. With a music extension that is a track, listed under its
// file name.
static void tmp_file(const char *rel) {
  FILE *f = fopen(tmp_join(rel), "wb");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

// How many files and folders this process has open: a scan must leave the
// number as it found it, however it ends.
static int open_handles(void) {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == NULL) {
    return -1; // not Linux: the callers then compare -1 with -1
  }
  int n = 0;
  while (readdir(dir) != NULL) {
    n++;
  }
  closedir(dir);
  return n;
}

static library_builder_t *begin(void) {
  library_builder_t *b = library_builder_begin(ARENA, ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  return b;
}

// Scan the fixture tree into `lib`.
static void scan_fixture(void) {
  library_builder_t *b = begin();
  TEST_ASSERT_TRUE(libscan_run(FIXTURE_DIR, b, NULL, NULL, &stats));
  lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
}

static void scan_tmp(void) {
  library_builder_t *b = begin();
  TEST_ASSERT_TRUE(libscan_run(tmp_root, b, NULL, NULL, &stats));
  lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
}

static uint32_t artist_called(const char *name) {
  for (uint32_t i = 0; i < library_artist_count(lib); i++) {
    if (strcmp(library_artist_name(lib, i), name) == 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

// The album of that name filed under that artist.
static uint32_t album_of(const char *artist, const char *album) {
  uint32_t first, count;
  library_artist_albums(lib, artist_called(artist), &first, &count);
  for (uint32_t i = first; i < first + count; i++) {
    if (strcmp(library_album_name(lib, i), album) == 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

static uint32_t track_at(const char *path) {
  uint32_t track = UINT32_MAX;
  TEST_ASSERT_TRUE_MESSAGE(library_find_path(lib, path, &track), path);
  return track;
}

static void assert_track(uint32_t track, const char *title, const char *artist,
                         unsigned disc, unsigned no) {
  library_track_t t;
  TEST_ASSERT_TRUE(library_track(lib, track, &t));
  TEST_ASSERT_EQUAL_STRING(title, t.title);
  TEST_ASSERT_EQUAL_STRING(artist, t.artist);
  TEST_ASSERT_EQUAL_UINT(disc, t.disc_no);
  TEST_ASSERT_EQUAL_UINT(no, t.track_no);
}

// --- The fixture tree --------------------------------------------------------

void test_scan_finds_the_sixteen_tracks(void) {
  scan_fixture();
  TEST_ASSERT_EQUAL_UINT32(16, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_FALSE(stats.full);
  TEST_ASSERT_EQUAL_UINT32(16, library_track_count(lib));
}

void test_hidden_and_system_folders_are_skipped(void) {
  scan_fixture();
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, artist_called("Hidden Artist"));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, artist_called("System Artist"));
  TEST_ASSERT_FALSE(library_find_path(lib, ".hidden/secret.flac", NULL));
  TEST_ASSERT_FALSE(
      library_find_path(lib, "System Volume Information/x.flac", NULL));
  // Not entered, so not counted as anything either.
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
}

void test_non_audio_files_are_ignored(void) {
  scan_fixture();
  TEST_ASSERT_FALSE(library_find_path(lib, "notes.txt", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, "cover.jpg", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, "README.md", NULL));
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  // Every track is a .flac or an .mp3.
  for (uint32_t i = 0; i < library_track_count(lib); i++) {
    char path[LIBSCAN_PATH_MAX];
    size_t len = library_track_path(lib, i, path, sizeof(path));
    TEST_ASSERT_TRUE(len > 5 && len < sizeof(path));
    tags_format_t format;
    TEST_ASSERT_TRUE_MESSAGE(tags_format_from_name(path, &format), path);
  }
}

void test_artists_and_albums_match_the_fixture(void) {
  scan_fixture();
  static const char *const ARTISTS[] = {"Bjork",          "Deep Artist",
                                        "Queen",          "Radiohead",
                                        "Unknown Artist", "Various Artists"};
  TEST_ASSERT_EQUAL_UINT32(6, library_artist_count(lib));
  for (uint32_t i = 0; i < 6; i++) {
    TEST_ASSERT_EQUAL_STRING(ARTISTS[i], library_artist_name(lib, i));
  }
  TEST_ASSERT_EQUAL_UINT32(7, library_album_count(lib));

  uint32_t first, count;
  library_artist_albums(lib, artist_called("Radiohead"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("Kid A", library_album_name(lib, first));
  TEST_ASSERT_EQUAL_STRING("OK Computer", library_album_name(lib, first + 1));
  // All five Radiohead tracks are under the one artist, although one of
  // them spells it "radiohead".
  library_artist_tracks(lib, artist_called("Radiohead"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(5, count);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, artist_called("radiohead"));

  uint32_t ok = album_of("Radiohead", "OK Computer");
  library_album_tracks(lib, ok, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  assert_track(first, "Airbag", "Radiohead", 0, 1);
  // The track keeps its own spelling; only the grouping ignores case.
  assert_track(first + 1, "Paranoid Android", "radiohead", 0, 2);

  // Two different albums share a name.
  uint32_t queen = album_of("Queen", "Greatest Hits");
  uint32_t various = album_of("Various Artists", "Greatest Hits");
  TEST_ASSERT_NOT_EQUAL(UINT32_MAX, queen);
  TEST_ASSERT_NOT_EQUAL(UINT32_MAX, various);
  TEST_ASSERT_NOT_EQUAL(queen, various);
  library_album_tracks(lib, queen, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  // Tagged with ID3v1.1 only.
  assert_track(first, "Bohemian Rhapsody", "Queen", 0, 1);
  library_album_tracks(lib, various, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(3, count);

  // Tag text is converted for the LCD.
  library_artist_albums(lib, artist_called("Bjork"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  TEST_ASSERT_EQUAL_STRING("Homogenic", library_album_name(lib, first));
  library_album_tracks(lib, first, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  assert_track(first, "Hunter", "Bjork", 0, 1);
  assert_track(first + 1, "Joga", "Bjork", 0, 2);

  library_artist_albums(lib, artist_called("Deep Artist"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  TEST_ASSERT_EQUAL_STRING("Deep Album", library_album_name(lib, first));
  library_artist_albums(lib, artist_called("Unknown Artist"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  TEST_ASSERT_EQUAL_STRING("Unknown Album", library_album_name(lib, first));
}

void test_album_order_within_kid_a(void) {
  scan_fixture();
  uint32_t first, count;
  library_album_tracks(lib, album_of("Radiohead", "Kid A"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(3, count);
  assert_track(first, "Everything In Its Right Place", "Radiohead", 1, 1);
  assert_track(first + 1, "Kid A", "Radiohead", 1, 2);
  assert_track(first + 2, "Motion Picture Soundtrack", "Radiohead", 1, 10);
  library_track_t t;
  TEST_ASSERT_TRUE(library_track(lib, first, &t));
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_FLAC, t.format);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms);
}

void test_compilation_disc_and_track_order(void) {
  scan_fixture();
  uint32_t album = album_of("Various Artists", "Greatest Hits");
  uint32_t first, count;
  library_album_tracks(lib, album, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(3, count);
  // Filed under the album artist; each track keeps its own.
  assert_track(first, "Alpha", "The Alphas", 1, 1);
  assert_track(first + 1, "Beta", "The Betas", 1, 2);
  assert_track(first + 2, "Gamma", "The Gammas", 2, 1);
  for (uint32_t i = first; i < first + 3; i++) {
    library_track_t t;
    TEST_ASSERT_TRUE(library_track(lib, i, &t));
    TEST_ASSERT_EQUAL_UINT32(artist_called("Various Artists"), t.group_artist);
  }
  // The track artists are not artists of the library.
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, artist_called("The Alphas"));
}

void test_untagged_files_use_their_file_names(void) {
  scan_fixture();
  uint32_t unknown = album_of("Unknown Artist", "Unknown Album");
  TEST_ASSERT_NOT_EQUAL(UINT32_MAX, unknown);

  uint32_t track = track_at("loose/untagged song.flac");
  assert_track(track, "untagged song", "Unknown Artist", 0, 0);
  library_track_t t;
  TEST_ASSERT_TRUE(library_track(lib, track, &t));
  TEST_ASSERT_EQUAL_UINT32(unknown, t.album);
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_FLAC, t.format);
  TEST_ASSERT_EQUAL_UINT32(100, t.duration_ms); // known without any tag

  track = track_at("loose/also untagged.mp3");
  assert_track(track, "also untagged", "Unknown Artist", 0, 0);
  TEST_ASSERT_TRUE(library_track(lib, track, &t));
  TEST_ASSERT_EQUAL_UINT32(unknown, t.album);
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_MP3, t.format);
}

static void make_unopenable(void) {
  // A link to nothing: it is listed, has a music extension, and cannot be
  // opened.
  TEST_ASSERT_EQUAL_INT(0, symlink("/nonexistent/nowhere.flac",
                                   tmp_join("gone.flac")));
}

void test_zero_byte_and_text_files_fall_back_or_are_counted(void) {
  // Files that open but hold no music are listed under their file names.
  scan_fixture();
  uint32_t unknown = album_of("Unknown Artist", "Unknown Album");
  uint32_t first, count;
  library_album_tracks(lib, unknown, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(4, count);
  // No numbers, so in file-name order.
  assert_track(first, "also untagged", "Unknown Artist", 0, 0);
  assert_track(first + 1, "empty", "Unknown Artist", 0, 0);
  assert_track(first + 2, "text", "Unknown Artist", 0, 0);
  assert_track(first + 3, "untagged song", "Unknown Artist", 0, 0);
  TEST_ASSERT_EQUAL_UINT32(first + 1, track_at("loose/empty.flac"));
  TEST_ASSERT_EQUAL_UINT32(first + 2, track_at("loose/text.mp3"));
  library_track_t t;
  TEST_ASSERT_TRUE(library_track(lib, first + 1, &t));
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_FLAC, t.format);
  TEST_ASSERT_TRUE(library_track(lib, first + 2, &t));
  TEST_ASSERT_EQUAL_UINT32(0, t.duration_ms);
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_MP3, t.format);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);

  // A file that cannot be opened is left out and counted.
  tmp_begin();
  tmp_file("here.flac");
  make_unopenable();
  scan_tmp();
  TEST_ASSERT_EQUAL_UINT32(1, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(1, stats.unreadable);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_EQUAL_UINT32(1, library_track_count(lib));
  TEST_ASSERT_FALSE(library_find_path(lib, "gone.flac", NULL));
  assert_track(track_at("here.flac"), "here", "Unknown Artist", 0, 0);
}

void test_paths_are_relative_and_open_the_right_file(void) {
  scan_fixture();
  for (uint32_t i = 0; i < library_track_count(lib); i++) {
    char rel[LIBSCAN_PATH_MAX];
    size_t len = library_track_path(lib, i, rel, sizeof(rel));
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(rel));
    TEST_ASSERT_NOT_EQUAL('/', rel[0]);
    TEST_ASSERT_NULL(strstr(rel, "//"));
    TEST_ASSERT_NULL(strstr(rel, FIXTURE_DIR));

    static char full[sizeof(FIXTURE_DIR) + 1 + LIBSCAN_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", FIXTURE_DIR, rel);
    FILE *f = fopen(full, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, full);
    // The file at that path is the one the row describes: its own tags
    // give the row's track number and length.
    library_track_t row;
    TEST_ASSERT_TRUE(library_track(lib, i, &row));
    static tags_t tags;
    bool read = tags_read(f, (tags_format_t)row.format, &tags);
    fclose(f);
    if (read) {
      TEST_ASSERT_EQUAL_UINT_MESSAGE(tags.track_no, row.track_no, full);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(tags.duration_ms, row.duration_ms,
                                       full);
    }
  }
  // Paths as the README writes them, deepest included.
  assert_track(track_at("Radiohead/Kid A/02 Kid A.flac"), "Kid A", "Radiohead",
               1, 2);
  assert_track(track_at("deep/a/b/c/d/Deep Track.flac"), "Deep Track",
               "Deep Artist", 0, 1);
  assert_track(track_at("Various/Hits/01 c.flac"), "Gamma", "The Gammas", 2,
               1);
}

void test_a_trailing_slash_on_the_root_changes_nothing(void) {
  library_builder_t *b = begin();
  TEST_ASSERT_TRUE(libscan_run(FIXTURE_DIR "/", b, NULL, NULL, &stats));
  lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(16, library_track_count(lib));
  assert_track(track_at("Queen/Hits/01 Bohemian.mp3"), "Bohemian Rhapsody",
               "Queen", 0, 1);
}

// --- Trees made for the purpose ---------------------------------------------

void test_too_deep_folders_are_skipped_and_counted(void) {
  tmp_begin();
  // LIBSCAN_MAX_DEPTH folders below the root are entered; a ninth is not.
  char rel[128] = "";
  size_t len = 0;
  for (int level = 1; level <= LIBSCAN_MAX_DEPTH + 2; level++) {
    len += (size_t)snprintf(rel + len, sizeof(rel) - len, "%sd%d",
                            level > 1 ? "/" : "", level);
    tmp_dir(rel);
    char file[160];
    snprintf(file, sizeof(file), "%s/t%d.flac", rel, level);
    tmp_file(file);
    snprintf(file, sizeof(file), "%s/u%d.mp3", rel, level);
    tmp_file(file);
  }
  tmp_file("top.flac");

  int before = open_handles();
  scan_tmp();
  TEST_ASSERT_EQUAL_INT(before, open_handles());
  // The top file and two files in each of the eight folders.
  TEST_ASSERT_EQUAL_UINT32(1 + 2 * LIBSCAN_MAX_DEPTH, stats.tracks);
  // The ninth folder is counted once, whatever is inside it.
  TEST_ASSERT_EQUAL_UINT32(1, stats.skipped);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  TEST_ASSERT_FALSE(stats.full);
  TEST_ASSERT_TRUE(
      library_find_path(lib, "d1/d2/d3/d4/d5/d6/d7/d8/t8.flac", NULL));
  TEST_ASSERT_FALSE(
      library_find_path(lib, "d1/d2/d3/d4/d5/d6/d7/d8/d9/t9.flac", NULL));
  TEST_ASSERT_FALSE(
      library_find_path(lib, "d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/t10.flac", NULL));
}

// `n` bytes of a name: a letter repeated.
static const char *long_name(char letter, size_t n, const char *end) {
  static char name[300];
  TEST_ASSERT_TRUE(n + strlen(end) < sizeof(name));
  memset(name, letter, n);
  strcpy(name + n, end);
  return name;
}

void test_paths_too_long_are_skipped_and_counted_never_cut(void) {
  tmp_begin();
  char rel[600];
  // Two folders of 100 bytes: "a.../b.../" is 202 bytes.
  char a[101], b[101];
  strcpy(a, long_name('a', 100, ""));
  strcpy(b, long_name('b', 100, ""));
  tmp_dir(a);
  snprintf(rel, sizeof(rel), "%s/%s", a, b);
  tmp_dir(rel);
  // A path of exactly LIBSCAN_PATH_MAX - 1 bytes fits: 202 + 48 + 5.
  snprintf(rel, sizeof(rel), "%s/%s/%s", a, b, long_name('f', 48, ".flac"));
  TEST_ASSERT_EQUAL_size_t(LIBSCAN_PATH_MAX - 1, strlen(rel));
  tmp_file(rel);
  char fits[LIBSCAN_PATH_MAX];
  strcpy(fits, rel);
  // One byte more does not.
  snprintf(rel, sizeof(rel), "%s/%s/%s", a, b, long_name('g', 49, ".flac"));
  tmp_file(rel);
  // Nor does a much longer one; and a file that is not music is not counted
  // however long its path.
  snprintf(rel, sizeof(rel), "%s/%s/%s", a, b, long_name('h', 200, ".mp3"));
  tmp_file(rel);
  snprintf(rel, sizeof(rel), "%s/%s/%s", a, b, long_name('i', 200, ".txt"));
  tmp_file(rel);
  // A folder whose own path is too long is counted once, files and all.
  snprintf(rel, sizeof(rel), "%s/%s/%s", a, b, long_name('j', 60, ""));
  tmp_dir(rel);
  size_t len = strlen(rel);
  snprintf(rel + len, sizeof(rel) - len, "/x.flac");
  tmp_file(rel);
  snprintf(rel + len, sizeof(rel) - len, "/y.flac");
  tmp_file(rel);

  int before = open_handles();
  scan_tmp();
  TEST_ASSERT_EQUAL_INT(before, open_handles());
  TEST_ASSERT_EQUAL_UINT32(1, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(3, stats.skipped);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);

  char got[LIBSCAN_PATH_MAX];
  TEST_ASSERT_EQUAL_size_t(LIBSCAN_PATH_MAX - 1,
                           library_track_path(lib, 0, got, sizeof(got)));
  TEST_ASSERT_EQUAL_STRING(fits, got);
  // The title is the whole file name, not a piece of it.
  library_track_t t;
  TEST_ASSERT_TRUE(library_track(lib, 0, &t));
  TEST_ASSERT_EQUAL_STRING(long_name('f', 48, ""), t.title);
}

void test_extensions_match_in_any_case(void) {
  tmp_begin();
  tmp_file("ONE.FLAC");
  tmp_file("two.Mp3");
  tmp_file("three.flacx");
  tmp_file("four.mp4");
  tmp_file("flac");
  tmp_file("five.flac.bak");
  tmp_dir("folder.flac"); // a folder is entered, not read
  tmp_file("folder.flac/six.mP3");
  scan_tmp();
  TEST_ASSERT_EQUAL_UINT32(3, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  assert_track(track_at("ONE.FLAC"), "ONE", "Unknown Artist", 0, 0);
  assert_track(track_at("two.Mp3"), "two", "Unknown Artist", 0, 0);
  assert_track(track_at("folder.flac/six.mP3"), "six", "Unknown Artist", 0, 0);
}

void test_dot_folders_are_skipped_at_any_depth(void) {
  tmp_begin();
  tmp_dir("music");
  tmp_dir("music/.Trash");
  tmp_file("music/.Trash/old.flac");
  tmp_dir("music/system volume information"); // FAT ignores case
  tmp_file("music/system volume information/x.mp3");
  tmp_dir(".dap");
  tmp_file(".dap/y.flac");
  tmp_dir("music/Not.hidden");
  tmp_file("music/Not.hidden/keep.flac");
  scan_tmp();
  TEST_ASSERT_EQUAL_UINT32(1, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_TRUE(library_find_path(lib, "music/Not.hidden/keep.flac", NULL));
}

void test_dot_files_are_skipped(void) {
  // macOS leaves a "._Name" companion beside each file it copies to a FAT
  // card. It has the music file's extension and is not music.
  tmp_begin();
  tmp_file("Song.flac");
  tmp_file("._Song.flac");
  tmp_file(".hidden.mp3");
  tmp_file(".flac");
  tmp_dir("album");
  tmp_file("album/Other.mp3");
  tmp_file("album/._Other.mp3");
  // One that could not even be opened: still not counted, because it is
  // never tried.
  TEST_ASSERT_EQUAL_INT(0, symlink("/nonexistent/nowhere.flac",
                                   tmp_join("album/._gone.flac")));
  scan_tmp();
  TEST_ASSERT_EQUAL_UINT32(2, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_EQUAL_UINT32(2, library_track_count(lib));
  TEST_ASSERT_TRUE(library_find_path(lib, "Song.flac", NULL));
  TEST_ASSERT_TRUE(library_find_path(lib, "album/Other.mp3", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, "._Song.flac", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, ".hidden.mp3", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, ".flac", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, "album/._Other.mp3", NULL));
}

void test_an_empty_tree_is_an_empty_library(void) {
  tmp_begin();
  tmp_dir("nothing");
  tmp_file("nothing/readme.txt");
  scan_tmp();
  TEST_ASSERT_EQUAL_UINT32(0, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_FALSE(stats.full);
  TEST_ASSERT_EQUAL_UINT32(0, library_track_count(lib));
}

// --- Progress ---------------------------------------------------------------

typedef struct {
  uint32_t calls;
  uint32_t stop_after; // 0: never
  uint32_t last_tracks;
  bool in_step; // each call saw exactly one more track than the last
} progress_t;

static bool on_progress(void *ctx, const libscan_stats_t *s) {
  progress_t *p = ctx;
  p->calls++;
  p->in_step = p->in_step && (s->tracks == p->last_tracks + 1);
  p->last_tracks = s->tracks;
  return p->stop_after == 0 || p->calls < p->stop_after;
}

void test_progress_is_called_once_for_each_track(void) {
  progress_t p = {.in_step = true};
  library_builder_t *b = begin();
  TEST_ASSERT_TRUE(libscan_run(FIXTURE_DIR, b, on_progress, &p, &stats));
  TEST_ASSERT_EQUAL_UINT32(16, p.calls);
  TEST_ASSERT_TRUE(p.in_step);
  TEST_ASSERT_EQUAL_UINT32(16, stats.tracks);
}

void test_progress_can_abandon_the_scan(void) {
  progress_t p = {.in_step = true, .stop_after = 3};
  int before = open_handles();
  library_builder_t *b = begin();
  TEST_ASSERT_FALSE(libscan_run(FIXTURE_DIR, b, on_progress, &p, &stats));
  // Nothing is left open, although the scan stopped inside the tree.
  TEST_ASSERT_EQUAL_INT(before, open_handles());
  TEST_ASSERT_EQUAL_UINT32(3, p.calls);
  TEST_ASSERT_EQUAL_UINT32(3, stats.tracks);
  TEST_ASSERT_FALSE(stats.full);
  // What was added before is still a library.
  lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(3, library_track_count(lib));
}

// --- Failures ---------------------------------------------------------------

void test_missing_root_returns_false(void) {
  library_builder_t *b = begin();
  TEST_ASSERT_FALSE(
      libscan_run(FIXTURE_DIR "/no such folder", b, NULL, NULL, &stats));
  TEST_ASSERT_EQUAL_UINT32(0, stats.tracks);
  TEST_ASSERT_EQUAL_UINT32(0, stats.unreadable);
  TEST_ASSERT_EQUAL_UINT32(0, stats.skipped);
  TEST_ASSERT_FALSE(stats.full);

  // A file is not a folder.
  TEST_ASSERT_FALSE(
      libscan_run(FIXTURE_DIR "/notes.txt", b, NULL, NULL, &stats));
  // Nor is nothing at all, and a root too long to be one is refused too.
  TEST_ASSERT_FALSE(libscan_run(NULL, b, NULL, NULL, &stats));
  TEST_ASSERT_FALSE(libscan_run(FIXTURE_DIR, NULL, NULL, NULL, &stats));
  TEST_ASSERT_FALSE(
      libscan_run(long_name('r', LIBSCAN_PATH_MAX, ""), b, NULL, NULL, &stats));
  TEST_ASSERT_EQUAL_UINT32(0, stats.tracks);

  // The statistics are optional.
  TEST_ASSERT_FALSE(
      libscan_run(FIXTURE_DIR "/no such folder", b, NULL, NULL, NULL));
  TEST_ASSERT_TRUE(libscan_run(FIXTURE_DIR, b, NULL, NULL, NULL));
  lib = library_builder_finish(b);
  TEST_ASSERT_EQUAL_UINT32(16, library_track_count(lib));
}

void test_full_index_stops_the_scan_cleanly(void) {
  // Find the smallest arena a build can start in, then one a little larger:
  // room for some of the sixteen tracks but not all.
  static uint32_t small_words[16384];
  size_t least = 0;
  for (size_t bytes = 64; bytes <= sizeof(small_words); bytes += 64) {
    if (library_builder_begin(small_words, bytes) != NULL) {
      least = bytes;
      break;
    }
  }
  TEST_ASSERT_NOT_EQUAL(0, least);

  bool seen_partial = false;
  for (size_t bytes = least; bytes <= sizeof(small_words); bytes += 256) {
    library_builder_t *b = library_builder_begin(small_words, bytes);
    TEST_ASSERT_NOT_NULL(b);
    progress_t p = {.in_step = true};
    int before = open_handles();
    libscan_stats_t s;
    // Full is not a failure: the scan stops and what it has is kept.
    TEST_ASSERT_TRUE(libscan_run(FIXTURE_DIR, b, on_progress, &p, &s));
    TEST_ASSERT_EQUAL_INT(before, open_handles());
    const library_t *small = library_builder_finish(b);
    TEST_ASSERT_NOT_NULL(small);
    TEST_ASSERT_EQUAL_UINT32(s.tracks, library_track_count(small));
    // Progress is reported for tracks that went in, not for the one that
    // did not.
    TEST_ASSERT_EQUAL_UINT32(s.tracks, p.calls);
    TEST_ASSERT_EQUAL_UINT32(0, s.unreadable);
    if (s.tracks == 16) {
      TEST_ASSERT_FALSE(s.full);
      break;
    }
    TEST_ASSERT_TRUE(s.full);
    if (s.tracks > 0) {
      seen_partial = true;
      // What is there is whole: every track has its path and a title.
      for (uint32_t i = 0; i < s.tracks; i++) {
        char path[LIBSCAN_PATH_MAX];
        library_track_t t;
        TEST_ASSERT_TRUE(library_track_path(small, i, path, sizeof(path)) > 0);
        TEST_ASSERT_TRUE(library_track(small, i, &t));
        TEST_ASSERT_TRUE(t.title[0] != '\0');
        TEST_ASSERT_TRUE(library_find_path(small, path, NULL));
      }
    }
  }
  TEST_ASSERT_TRUE(seen_partial);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_scan_finds_the_sixteen_tracks);
  RUN_TEST(test_hidden_and_system_folders_are_skipped);
  RUN_TEST(test_non_audio_files_are_ignored);
  RUN_TEST(test_artists_and_albums_match_the_fixture);
  RUN_TEST(test_album_order_within_kid_a);
  RUN_TEST(test_compilation_disc_and_track_order);
  RUN_TEST(test_untagged_files_use_their_file_names);
  RUN_TEST(test_zero_byte_and_text_files_fall_back_or_are_counted);
  RUN_TEST(test_paths_are_relative_and_open_the_right_file);
  RUN_TEST(test_a_trailing_slash_on_the_root_changes_nothing);
  RUN_TEST(test_too_deep_folders_are_skipped_and_counted);
  RUN_TEST(test_paths_too_long_are_skipped_and_counted_never_cut);
  RUN_TEST(test_extensions_match_in_any_case);
  RUN_TEST(test_dot_folders_are_skipped_at_any_depth);
  RUN_TEST(test_dot_files_are_skipped);
  RUN_TEST(test_an_empty_tree_is_an_empty_library);
  RUN_TEST(test_progress_is_called_once_for_each_track);
  RUN_TEST(test_progress_can_abandon_the_scan);
  RUN_TEST(test_missing_root_returns_false);
  RUN_TEST(test_full_index_stops_the_scan_cleanly);
  return UNITY_END();
}
