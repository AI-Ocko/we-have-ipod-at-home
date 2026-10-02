#include "library.h"
#include "tags.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The arena the device uses: 1.5 MB.
#define ARENA_BYTES 1572864u

// Arrays of words, so that they are 4-byte aligned like a block from the
// device's allocator.
static uint32_t arena_a_words[ARENA_BYTES / 4];
static uint32_t arena_b_words[ARENA_BYTES / 4];
#define ARENA_A ((void *)arena_a_words)
#define ARENA_B ((void *)arena_b_words)

// Saved index files, read back into memory.
static uint8_t img_a[ARENA_BYTES];
static uint8_t img_b[ARENA_BYTES];

// The file a test has open. tearDown closes it, so a failed assertion (which
// jumps out of the test) cannot leak it and trip the leak checker.
static FILE *open_file;

void setUp(void) { open_file = NULL; }

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

// --- The index file as the comment at the top of library.c describes it ----
//
// The tests that damage a file need to know where things are.

enum {
  H_MAGIC = 0,
  H_VERSION = 4,
  H_TOTAL = 8,
  H_CRC = 12,
  H_TRACKS = 16,
  H_ALBUMS = 20,
  H_ARTISTS = 24,
  H_DIRS = 28,
  H_POOL_BYTES = 32,
  H_OFF_TRACKS = 36,
  H_OFF_ALBUMS = 40,
  H_OFF_ARTISTS = 44,
  H_OFF_DIRS = 48,
  H_OFF_SONGS = 52,
  H_OFF_PATHS = 56,
  H_OFF_ALBUM_NAMES = 60,
  H_OFF_FORMATS = 64,
  H_OFF_POOL = 68,
  HEADER_BYTES = 72,
  CRC_FROM = 16, // the checksum covers everything from here to the end

  TRACK_ROW = 24,
  TR_TITLE = 0,
  TR_ARTIST = 4,
  TR_NAME = 8,
  TR_DURATION = 12,
  TR_ALBUM = 16,
  TR_DIR = 18,

  ALBUM_ROW = 8,
  AL_NAME = 0,
  AL_FIRST_TRACK = 4,
  AL_ARTIST = 6,

  ARTIST_ROW = 8,
  AR_NAME = 0,
  AR_FIRST_ALBUM = 4,
  AR_FIRST_TRACK = 6,

  DIR_ROW = 8,
  DI_NAME = 0,
  DI_FIRST = 4,
};

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

static void wr16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

// CRC-32 as zlib, PNG and Ethernet compute it, one bit at a time: written
// independently of the table in library.c.
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

static void fix_checksum(uint8_t *img, size_t n) {
  wr32(img + H_CRC, crc32_bitwise(img + CRC_FROM, n - CRC_FROM));
}

// --- Building, saving, loading ---------------------------------------------

static library_track_in_t trk(const char *path, const char *title,
                              const char *artist, const char *album,
                              const char *group_artist, uint16_t track_no,
                              uint16_t disc_no) {
  library_track_in_t t = {
      .path = path,
      .title = title,
      .artist = artist,
      .album = album,
      .group_artist = group_artist,
      .track_no = track_no,
      .disc_no = disc_no,
      .duration_ms = 1000u * track_no + 7u,
      .format = TAGS_FORMAT_FLAC,
  };
  return t;
}

// Build `n` tracks, added in the order order[0], order[1], ... (or as given
// if `order` is NULL).
static const library_t *build_ordered(void *arena, size_t arena_bytes,
                                      const library_track_in_t *tracks,
                                      size_t n, const size_t *order) {
  library_builder_t *b = library_builder_begin(arena, arena_bytes);
  TEST_ASSERT_NOT_NULL(b);
  for (size_t i = 0; i < n; i++) {
    const library_track_in_t *t = &tracks[order != NULL ? order[i] : i];
    TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, t));
  }
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(n, library_track_count(lib));
  return lib;
}

static const library_t *build(const library_track_in_t *tracks, size_t n) {
  return build_ordered(ARENA_A, ARENA_BYTES, tracks, n, NULL);
}

static size_t save_image(const library_t *lib, uint8_t *img, size_t cap) {
  FILE *f = fresh_tmp();
  TEST_ASSERT_EQUAL(LIBRARY_OK, library_save(lib, f));
  long end = ftell(f);
  TEST_ASSERT_TRUE(end > 0);
  TEST_ASSERT_TRUE((size_t)end <= cap);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)end, library_bytes(lib));
  rewind(f);
  TEST_ASSERT_EQUAL_size_t((size_t)end, fread(img, 1, (size_t)end, f));
  return (size_t)end;
}

static library_result_t load_image(const uint8_t *img, size_t n, void *arena,
                                   size_t arena_bytes, const library_t **out) {
  FILE *f = fresh_tmp();
  if (n > 0) {
    TEST_ASSERT_EQUAL_size_t(n, fwrite(img, 1, n, f));
  }
  rewind(f);
  return library_load(arena, arena_bytes, f, out);
}

static void assert_rejected(const uint8_t *img, size_t n, const char *what) {
  const library_t *lib = (const library_t *)img; // anything but NULL
  library_result_t r = load_image(img, n, ARENA_B, ARENA_BYTES, &lib);
  TEST_ASSERT_EQUAL_MESSAGE(LIBRARY_BAD_FILE, r, what);
  TEST_ASSERT_NULL_MESSAGE(lib, what);
}

// --- What "sorted" means, written independently ---------------------------

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Order ignoring case; 0 if the two differ only in case.
static int fold_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = fold((unsigned char)*a);
    int cb = fold((unsigned char)*b);
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
    if (ca == 0) {
      return 0;
    }
  }
}

static const char *file_name_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

static uint32_t artist_named(const library_t *lib, const char *name) {
  for (uint32_t a = 0; a < library_artist_count(lib); a++) {
    if (strcmp(library_artist_name(lib, a), name) == 0) {
      return a;
    }
  }
  TEST_FAIL_MESSAGE(name);
  return 0;
}

static uint32_t track_at(const library_t *lib, const char *path) {
  uint32_t t = 0xDEADBEEFu;
  TEST_ASSERT_TRUE_MESSAGE(library_find_path(lib, path, &t), path);
  TEST_ASSERT_TRUE(t < library_track_count(lib));
  return t;
}

static library_track_t track_info(const library_t *lib, uint32_t t) {
  library_track_t info;
  memset(&info, 0, sizeof(info));
  TEST_ASSERT_TRUE(library_track(lib, t, &info));
  return info;
}

static char path_a[1024];
static char path_b[1024];
static bool seen[65536];

// Everything the header promises about how the lists hang together, checked
// for a whole library.
static void assert_well_formed(const library_t *lib) {
  uint32_t n_tracks = library_track_count(lib);
  uint32_t n_albums = library_album_count(lib);
  uint32_t n_artists = library_artist_count(lib);
  TEST_ASSERT_TRUE(n_artists <= n_albums);
  TEST_ASSERT_TRUE(n_albums <= n_tracks);

  // Artists A to Z, each owning the next run of albums and of tracks.
  uint32_t next_album = 0;
  uint32_t next_track = 0;
  for (uint32_t a = 0; a < n_artists; a++) {
    if (a > 0) {
      TEST_ASSERT_TRUE(fold_cmp(library_artist_name(lib, a - 1),
                                library_artist_name(lib, a)) < 0);
    }
    uint32_t first_album, album_count, first_track, track_count;
    library_artist_albums(lib, a, &first_album, &album_count);
    library_artist_tracks(lib, a, &first_track, &track_count);
    TEST_ASSERT_EQUAL_UINT32(next_album, first_album);
    TEST_ASSERT_EQUAL_UINT32(next_track, first_track);
    TEST_ASSERT_TRUE(album_count > 0);
    uint32_t tracks_in_albums = 0;
    for (uint32_t al = first_album; al < first_album + album_count; al++) {
      TEST_ASSERT_EQUAL_UINT32(a, library_album_artist(lib, al));
      if (al > first_album) {
        TEST_ASSERT_TRUE(fold_cmp(library_album_name(lib, al - 1),
                                  library_album_name(lib, al)) < 0);
      }
      uint32_t first, count;
      library_album_tracks(lib, al, &first, &count);
      TEST_ASSERT_EQUAL_UINT32(next_track + tracks_in_albums, first);
      TEST_ASSERT_TRUE(count > 0);
      tracks_in_albums += count;
      for (uint32_t t = first; t < first + count; t++) {
        library_track_t info = track_info(lib, t);
        TEST_ASSERT_EQUAL_UINT32(al, info.album);
        TEST_ASSERT_EQUAL_UINT32(a, info.group_artist);
        if (t > first) {
          library_track_t prev = track_info(lib, t - 1);
          TEST_ASSERT_TRUE(prev.disc_no <= info.disc_no);
          if (prev.disc_no == info.disc_no) {
            TEST_ASSERT_TRUE(prev.track_no <= info.track_no);
            if (prev.track_no == info.track_no) {
              library_track_path(lib, t - 1, path_a, sizeof(path_a));
              library_track_path(lib, t, path_b, sizeof(path_b));
              TEST_ASSERT_TRUE(
                  fold_cmp(file_name_of(path_a), file_name_of(path_b)) <= 0);
            }
          }
        }
      }
    }
    TEST_ASSERT_EQUAL_UINT32(tracks_in_albums, track_count);
    next_album += album_count;
    next_track += track_count;
  }
  TEST_ASSERT_EQUAL_UINT32(n_albums, next_album);
  TEST_ASSERT_EQUAL_UINT32(n_tracks, next_track);

  // Albums A to Z: every album once, in order.
  memset(seen, 0, n_albums);
  for (uint32_t i = 0; i < n_albums; i++) {
    uint32_t al = library_album_by_name(lib, i);
    TEST_ASSERT_TRUE(al < n_albums);
    TEST_ASSERT_FALSE(seen[al]);
    seen[al] = true;
    if (i > 0) {
      uint32_t before = library_album_by_name(lib, i - 1);
      int c = fold_cmp(library_album_name(lib, before),
                       library_album_name(lib, al));
      TEST_ASSERT_TRUE(c <= 0);
      if (c == 0) {
        TEST_ASSERT_TRUE(before < al);
      }
    }
  }

  // Songs A to Z: every track once, in order.
  memset(seen, 0, n_tracks);
  const char *title_before = NULL;
  uint32_t track_before = 0;
  for (uint32_t i = 0; i < n_tracks; i++) {
    uint32_t t = library_song_by_title(lib, i);
    TEST_ASSERT_TRUE(t < n_tracks);
    TEST_ASSERT_FALSE(seen[t]);
    seen[t] = true;
    const char *title = track_info(lib, t).title;
    if (i > 0) {
      TEST_ASSERT_TRUE(fold_cmp(title_before, title) <= 0);
      if (strcmp(title_before, title) == 0) {
        TEST_ASSERT_TRUE(track_before < t);
      }
    }
    title_before = title;
    track_before = t;
  }

  // Every track can be found again by its path.
  for (uint32_t t = 0; t < n_tracks; t++) {
    size_t len = library_track_path(lib, t, path_a, sizeof(path_a));
    TEST_ASSERT_TRUE(len < sizeof(path_a));
    TEST_ASSERT_EQUAL_size_t(strlen(path_a), len);
    uint32_t found = track_at(lib, path_a);
    library_track_path(lib, found, path_b, sizeof(path_b));
    TEST_ASSERT_EQUAL_STRING(path_a, path_b);
  }
}

// Every query gives the same answer from both.
static void assert_same_answers(const library_t *a, const library_t *b) {
  TEST_ASSERT_EQUAL_UINT32(library_bytes(a), library_bytes(b));
  TEST_ASSERT_EQUAL_UINT32(library_track_count(a), library_track_count(b));
  TEST_ASSERT_EQUAL_UINT32(library_album_count(a), library_album_count(b));
  TEST_ASSERT_EQUAL_UINT32(library_artist_count(a), library_artist_count(b));
  uint32_t fa, ca, fb, cb;
  for (uint32_t i = 0; i < library_artist_count(a); i++) {
    TEST_ASSERT_EQUAL_STRING(library_artist_name(a, i),
                             library_artist_name(b, i));
    library_artist_albums(a, i, &fa, &ca);
    library_artist_albums(b, i, &fb, &cb);
    TEST_ASSERT_EQUAL_UINT32(fa, fb);
    TEST_ASSERT_EQUAL_UINT32(ca, cb);
    library_artist_tracks(a, i, &fa, &ca);
    library_artist_tracks(b, i, &fb, &cb);
    TEST_ASSERT_EQUAL_UINT32(fa, fb);
    TEST_ASSERT_EQUAL_UINT32(ca, cb);
  }
  for (uint32_t i = 0; i < library_album_count(a); i++) {
    TEST_ASSERT_EQUAL_STRING(library_album_name(a, i),
                             library_album_name(b, i));
    TEST_ASSERT_EQUAL_UINT32(library_album_artist(a, i),
                             library_album_artist(b, i));
    library_album_tracks(a, i, &fa, &ca);
    library_album_tracks(b, i, &fb, &cb);
    TEST_ASSERT_EQUAL_UINT32(fa, fb);
    TEST_ASSERT_EQUAL_UINT32(ca, cb);
    TEST_ASSERT_EQUAL_UINT32(library_album_by_name(a, i),
                             library_album_by_name(b, i));
  }
  for (uint32_t i = 0; i < library_track_count(a); i++) {
    library_track_t ta = track_info(a, i);
    library_track_t tb = track_info(b, i);
    TEST_ASSERT_EQUAL_STRING(ta.title, tb.title);
    TEST_ASSERT_EQUAL_STRING(ta.artist, tb.artist);
    TEST_ASSERT_EQUAL_UINT32(ta.album, tb.album);
    TEST_ASSERT_EQUAL_UINT32(ta.group_artist, tb.group_artist);
    TEST_ASSERT_EQUAL_UINT16(ta.track_no, tb.track_no);
    TEST_ASSERT_EQUAL_UINT16(ta.disc_no, tb.disc_no);
    TEST_ASSERT_EQUAL_UINT32(ta.duration_ms, tb.duration_ms);
    TEST_ASSERT_EQUAL_UINT8(ta.format, tb.format);
    size_t la = library_track_path(a, i, path_a, sizeof(path_a));
    size_t lb = library_track_path(b, i, path_b, sizeof(path_b));
    TEST_ASSERT_EQUAL_size_t(la, lb);
    TEST_ASSERT_EQUAL_STRING(path_a, path_b);
    TEST_ASSERT_EQUAL_UINT32(track_at(a, path_a), track_at(b, path_a));
    TEST_ASSERT_EQUAL_UINT32(library_song_by_title(a, i),
                             library_song_by_title(b, i));
  }
}

// --- A synthetic library with the shape of a real one ----------------------
//
// Albums of 8 to 12 tracks (one in ten is a double album of 20), each in its
// own folder under its artist; an artist every three or four albums; a
// compilation now and then, filed under "Various Artists" with a different
// artist on every track. Folder paths come to 20 to 50 bytes, file names to
// 15 to 45, titles to 10 to 40 characters. Everything follows from the track
// number, so any track can be made again to check what the index says.

static const char *const W1[48] = {
    "Silver",  "Morning",  "River",    "Glass",    "Hollow",    "Electric",
    "Paper",   "Midnight", "Golden",   "Broken",   "Distant",   "Quiet",
    "Velvet",  "Northern", "Crimson",  "Wild",     "Lonely",    "Burning",
    "Frozen",  "Hidden",   "Little",   "Sacred",   "Wandering", "Neon",
    "Rusty",   "Pale",     "Gentle",   "Savage",   "Empty",     "Faded",
    "Summer",  "Winter",   "Autumn",   "Stolen",   "Borrowed",  "Bitter",
    "Sweet",   "Heavy",    "Shallow",  "Endless",  "Secret",    "Shining",
    "Falling", "Rising",   "Drifting", "Sleeping", "Dancing",   "Waiting"};
static const char *const W2[48] = {
    "Hearts",   "Road",      "Machine",   "Garden",   "Horses",     "Signal",
    "Tigers",   "Harbor",    "Windows",   "Thunder",  "Mirrors",    "Letters",
    "Engines",  "Rooms",     "Oceans",    "Feathers", "Shadows",    "Lanterns",
    "Wolves",   "Radio",     "Cathedral", "Orchard",  "Satellites", "Avenue",
    "Bridges",  "Wires",     "Kingdom",   "Fever",    "Echoes",     "Stations",
    "Parade",   "Monuments", "Rivers",    "Diamonds", "Valley",     "Strangers",
    "Carnival", "Whispers",  "Motel",     "Fires",    "Anthem",     "Tides",
    "Colours",  "Pilots",    "Hours",     "Arrows",   "Ghosts",     "Ladders"};
static const char *const LINK[8] = {"of the", "in",   "and the", "for",
                                    "under",  "on a", "like",    "from the"};

static uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

#define GEN_MAX_ALBUMS 1400u
#define GEN_MAX_ARTISTS 4096u
static uint32_t gen_album_first[GEN_MAX_ALBUMS + 1];
static bool gen_ready;

static uint32_t gen_album_size(uint32_t album) {
  return (album % 10 == 9) ? 20 : 8 + mix(album) % 5;
}

static bool gen_is_compilation(uint32_t album) { return album % 23 == 7; }

static uint32_t gen_album_owner(uint32_t album) { return album * 2 / 7; }

static void gen_init(void) {
  if (gen_ready) {
    return;
  }
  uint32_t first = 0;
  for (uint32_t al = 0; al <= GEN_MAX_ALBUMS; al++) {
    gen_album_first[al] = first;
    first += gen_album_size(al);
  }
  gen_ready = true;
}

static uint32_t gen_album_of(uint32_t track) {
  uint32_t lo = 0;
  uint32_t hi = GEN_MAX_ALBUMS;
  TEST_ASSERT_TRUE(track < gen_album_first[GEN_MAX_ALBUMS]);
  while (hi - lo > 1) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (gen_album_first[mid] <= track) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static void gen_artist_name(uint32_t artist, char *out, size_t cap) {
  snprintf(out, cap, "%s%s %s", (artist % 5 == 0) ? "The " : "",
           W1[artist % 48], W2[(artist / 48 + artist % 48 * 5) % 48]);
}

static void gen_album_name(uint32_t album, char *out, size_t cap) {
  snprintf(out, cap, "%s %s%s", W1[(album * 5 + 3) % 48], W2[album % 48],
           (album % 6 == 0) ? " (Deluxe Edition)" : "");
}

static void gen_title(uint32_t track, char *out, size_t cap) {
  uint32_t h = mix(track * 2654435761u + 17);
  size_t want = 10 + h % 31;
  size_t len = (size_t)snprintf(out, cap, "%s %s", W1[(h >> 5) % 48],
                                W2[(h >> 11) % 48]);
  while (len < want) {
    h = mix(h);
    len += (size_t)snprintf(out + len, cap - len, " %s %s", LINK[h % 8],
                            W2[(h >> 8) % 48]);
  }
  // Cut to the length wanted (in the middle of a word if need be), so that
  // lengths are spread evenly over 10 to 40 and average 25.
  out[want] = '\0';
  while (out[strlen(out) - 1] == ' ') {
    out[strlen(out) - 1] = '\0';
  }
}

typedef struct {
  char path[256];
  char title[64];
  char artist[64];
  char album[64];
  char group[64];
  library_track_in_t in;
} gen_track_t;

static void gen_track(uint32_t i, gen_track_t *g) {
  gen_init();
  uint32_t album = gen_album_of(i);
  uint32_t pos = i - gen_album_first[album];
  uint32_t size = gen_album_size(album);
  gen_album_name(album, g->album, sizeof(g->album));
  gen_title(i, g->title, sizeof(g->title));
  if (gen_is_compilation(album)) {
    snprintf(g->group, sizeof(g->group), "Various Artists");
    gen_artist_name(1000 + mix(i) % 400, g->artist, sizeof(g->artist));
  } else {
    gen_artist_name(gen_album_owner(album), g->group, sizeof(g->group));
    snprintf(g->artist, sizeof(g->artist), "%s", g->group);
  }
  bool mp3 = (album % 7 == 3);
  uint32_t disc = 0;
  uint32_t number = pos + 1;
  char folder[16] = "";
  if (size == 20) {
    disc = 1 + pos / 10;
    number = 1 + pos % 10;
    snprintf(folder, sizeof(folder), "CD%u/", (unsigned)disc);
  }
  snprintf(g->path, sizeof(g->path), "Music/%s/%s/%s%02u - %s.%s", g->group,
           g->album, folder, (unsigned)number, g->title, mp3 ? "mp3" : "flac");
  g->in.path = g->path;
  g->in.title = g->title;
  g->in.artist = g->artist;
  g->in.album = g->album;
  g->in.group_artist = g->group;
  g->in.track_no = (uint16_t)number;
  g->in.disc_no = (uint16_t)disc;
  g->in.duration_ms = 120000u + mix(i ^ 0x55u) % 240000u;
  g->in.format = mp3 ? TAGS_FORMAT_MP3 : TAGS_FORMAT_FLAC;
}

static gen_track_t gen;

// How many albums and artists the first `n` generated tracks make.
static void gen_counts(uint32_t n, uint32_t *albums, uint32_t *artists) {
  static bool used[GEN_MAX_ARTISTS];
  gen_init();
  memset(used, 0, sizeof(used));
  *albums = 0;
  *artists = 0;
  bool various = false;
  for (uint32_t al = 0; gen_album_first[al] < n; al++) {
    (*albums)++;
    if (gen_is_compilation(al)) {
      various = true;
    } else if (!used[gen_album_owner(al)]) {
      used[gen_album_owner(al)] = true;
      (*artists)++;
    }
  }
  if (various) {
    (*artists)++;
  }
}

// The index holds generated track `i`, exactly as it was handed over.
static void assert_holds_generated(const library_t *lib, uint32_t i) {
  gen_track(i, &gen);
  uint32_t t = track_at(lib, gen.path);
  library_track_t info = track_info(lib, t);
  TEST_ASSERT_EQUAL_STRING(gen.title, info.title);
  TEST_ASSERT_EQUAL_STRING(gen.artist, info.artist);
  TEST_ASSERT_EQUAL_STRING(gen.album, library_album_name(lib, info.album));
  TEST_ASSERT_EQUAL_STRING(gen.group,
                           library_artist_name(lib, info.group_artist));
  TEST_ASSERT_EQUAL_UINT32(info.group_artist,
                           library_album_artist(lib, info.album));
  TEST_ASSERT_EQUAL_UINT16(gen.in.track_no, info.track_no);
  TEST_ASSERT_EQUAL_UINT16(gen.in.disc_no, info.disc_no);
  TEST_ASSERT_EQUAL_UINT32(gen.in.duration_ms, info.duration_ms);
  TEST_ASSERT_EQUAL_UINT8(gen.in.format, info.format);
  size_t len = library_track_path(lib, t, path_a, sizeof(path_a));
  TEST_ASSERT_EQUAL_size_t(strlen(gen.path), len);
  TEST_ASSERT_EQUAL_STRING(gen.path, path_a);
}

// Add generated tracks 0, 1, 2, ... until `limit` have gone in or the arena
// is full. Returns how many went in.
static uint32_t gen_fill(library_builder_t *b, uint32_t limit) {
  for (uint32_t i = 0; i < limit; i++) {
    gen_track(i, &gen);
    library_result_t r = library_builder_add(b, &gen.in);
    if (r == LIBRARY_FULL) {
      return i;
    }
    TEST_ASSERT_EQUAL(LIBRARY_OK, r);
  }
  return limit;
}

// --- Small libraries -------------------------------------------------------

void test_empty_library(void) {
  library_builder_t *b = library_builder_begin(ARENA_A, ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(0, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(0, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(0, library_album_count(lib));
  TEST_ASSERT_EQUAL_UINT32(HEADER_BYTES, library_bytes(lib));
  uint32_t t = 99;
  TEST_ASSERT_FALSE(library_find_path(lib, "music/a.flac", &t));
  TEST_ASSERT_FALSE(library_find_path(lib, "", &t));
  library_track_t info;
  TEST_ASSERT_FALSE(library_track(lib, 0, &info));
  assert_well_formed(lib);

  // An empty index is still an index: it saves and loads.
  size_t n = save_image(lib, img_a, sizeof(img_a));
  TEST_ASSERT_EQUAL_size_t(HEADER_BYTES, n);
  const library_t *loaded = NULL;
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &loaded));
  TEST_ASSERT_NOT_NULL(loaded);
  TEST_ASSERT_EQUAL_UINT32(0, library_track_count(loaded));
}

void test_one_track(void) {
  library_track_in_t in = {
      .path = "music/16 - Kanye West - Hey Mama.flac",
      .title = "Hey Mama",
      .artist = "Kanye West",
      .album = "Late Registration",
      .group_artist = "Kanye West",
      .track_no = 16,
      .disc_no = 1,
      .duration_ms = 305067,
      .format = TAGS_FORMAT_FLAC,
  };
  const library_t *lib = build(&in, 1);
  TEST_ASSERT_EQUAL_UINT32(1, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(1, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(1, library_album_count(lib));
  TEST_ASSERT_EQUAL_STRING("Kanye West", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("Late Registration", library_album_name(lib, 0));
  TEST_ASSERT_EQUAL_UINT32(0, library_album_artist(lib, 0));
  uint32_t first = 9, count = 9;
  library_artist_albums(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  first = count = 9;
  library_artist_tracks(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  first = count = 9;
  library_album_tracks(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  TEST_ASSERT_EQUAL_UINT32(0, library_album_by_name(lib, 0));
  TEST_ASSERT_EQUAL_UINT32(0, library_song_by_title(lib, 0));

  library_track_t info = track_info(lib, 0);
  TEST_ASSERT_EQUAL_STRING("Hey Mama", info.title);
  TEST_ASSERT_EQUAL_STRING("Kanye West", info.artist);
  TEST_ASSERT_EQUAL_UINT32(0, info.album);
  TEST_ASSERT_EQUAL_UINT32(0, info.group_artist);
  TEST_ASSERT_EQUAL_UINT16(16, info.track_no);
  TEST_ASSERT_EQUAL_UINT16(1, info.disc_no);
  TEST_ASSERT_EQUAL_UINT32(305067, info.duration_ms);
  TEST_ASSERT_EQUAL_UINT8(TAGS_FORMAT_FLAC, info.format);

  char path[64];
  TEST_ASSERT_EQUAL_size_t(strlen(in.path),
                           library_track_path(lib, 0, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING(in.path, path);
  TEST_ASSERT_EQUAL_UINT32(0, track_at(lib, in.path));

  // One name used three times (artist, group artist, and once more as the
  // album below) is stored once: the track's artist and the artist list hand
  // out the very same bytes.
  TEST_ASSERT_EQUAL_PTR(library_artist_name(lib, 0), info.artist);
  assert_well_formed(lib);
}

void test_artists_are_sorted_ignoring_case(void) {
  const library_track_in_t tracks[] = {
      trk("a/1.flac", "t1", "zebra", "A", "zebra", 1, 0),
      trk("b/1.flac", "t2", "Apple", "A", "Apple", 1, 0),
      trk("c/1.flac", "t3", "mango", "A", "mango", 1, 0),
      trk("d/1.flac", "t4", "Banana", "A", "Banana", 1, 0),
      // No special treatment of a leading "The": it sorts under T.
      trk("e/1.flac", "t5", "The Beatles", "A", "The Beatles", 1, 0),
      trk("f/1.flac", "t6", "U2", "A", "U2", 1, 0),
      // Digits and punctuation are in plain byte order, before letters.
      trk("g/1.flac", "t7", "10cc", "A", "10cc", 1, 0),
      trk("h/1.flac", "t8", "!!!", "A", "!!!", 1, 0),
      trk("i/1.flac", "t9", "apple pie", "A", "apple pie", 1, 0),
  };
  const library_t *lib = build(tracks, 9);
  const char *const want[] = {"!!!",         "10cc",   "Apple",
                              "apple pie",   "Banana", "mango",
                              "The Beatles", "U2",     "zebra"};
  TEST_ASSERT_EQUAL_UINT32(9, library_artist_count(lib));
  for (uint32_t a = 0; a < 9; a++) {
    TEST_ASSERT_EQUAL_STRING(want[a], library_artist_name(lib, a));
  }
  assert_well_formed(lib);
}

void test_artist_names_differing_in_case_or_spaces_are_one_artist(void) {
  const library_track_in_t tracks[] = {
      trk("a/1.flac", "One", "Pink Floyd", "The Wall", "Pink Floyd", 1, 0),
      trk("a/2.flac", "Two", "x", "the wall ", " pink floyd ", 2, 0),
      trk("a/3.flac", "Three", "x", "  THE WALL", "PINK FLOYD", 3, 0),
      trk("a/4.flac", "Four", "x", "The Wall", "Pink Floyd   ", 4, 0),
      // A space inside the name is part of the name.
      trk("b/1.flac", "Five", "x", "The Wall", "PinkFloyd", 1, 0),
      trk("b/2.flac", "Six", "x", "The  Wall", "Pink Floyd", 1, 0),
  };
  const library_t *lib = build(tracks, 6);
  TEST_ASSERT_EQUAL_UINT32(2, library_artist_count(lib));
  TEST_ASSERT_EQUAL_STRING("Pink Floyd", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("PinkFloyd", library_artist_name(lib, 1));
  // "The  Wall" (two spaces), then "The Wall" with four tracks, under Pink
  // Floyd; "The Wall" again under PinkFloyd.
  TEST_ASSERT_EQUAL_UINT32(3, library_album_count(lib));
  uint32_t first, count;
  library_artist_albums(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("The  Wall", library_album_name(lib, first));
  TEST_ASSERT_EQUAL_STRING("The Wall", library_album_name(lib, first + 1));
  library_album_tracks(lib, first + 1, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(4, count);
  assert_well_formed(lib);
}

void test_first_spelling_seen_is_kept(void) {
  const library_track_in_t tracks[] = {
      trk("a/1.flac", "One", "x", "Arrival", "ABBA", 1, 0),
      trk("a/2.flac", "Two", "x", "ARRIVAL", "Abba", 2, 0),
      trk("a/3.flac", "Three", "x", "arrival", "abba", 3, 0),
  };
  const library_t *lib = build(tracks, 3);
  TEST_ASSERT_EQUAL_UINT32(1, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(1, library_album_count(lib));
  TEST_ASSERT_EQUAL_STRING("ABBA", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("Arrival", library_album_name(lib, 0));

  const size_t backwards[] = {2, 1, 0};
  lib = build_ordered(ARENA_A, ARENA_BYTES, tracks, 3, backwards);
  TEST_ASSERT_EQUAL_UINT32(1, library_artist_count(lib));
  TEST_ASSERT_EQUAL_STRING("abba", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("arrival", library_album_name(lib, 0));
}

void test_albums_group_under_their_artist(void) {
  const library_track_in_t tracks[] = {
      trk("r/k/1.flac", "Airbag", "Radiohead", "OK Computer", "Radiohead", 1,
          0),
      trk("b/h/1.flac", "Help!", "The Beatles", "Help!", "The Beatles", 1, 0),
      trk("r/b/1.flac", "Planet Telex", "Radiohead", "the bends", "Radiohead",
          1, 0),
      trk("r/k/2.flac", "Paranoid Android", "Radiohead", "OK Computer",
          "Radiohead", 2, 0),
      trk("r/a/1.flac", "Everything", "Radiohead", "Kid A", "Radiohead", 1, 0),
      trk("b/a/1.flac", "Come Together", "The Beatles", "Abbey Road",
          "The Beatles", 1, 0),
  };
  const library_t *lib = build(tracks, 6);
  TEST_ASSERT_EQUAL_UINT32(2, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(5, library_album_count(lib));

  uint32_t radiohead = artist_named(lib, "Radiohead");
  uint32_t beatles = artist_named(lib, "The Beatles");
  TEST_ASSERT_EQUAL_UINT32(0, radiohead);
  TEST_ASSERT_EQUAL_UINT32(1, beatles);

  uint32_t first, count;
  library_artist_albums(lib, radiohead, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(3, count);
  // A to Z within the artist, ignoring case.
  TEST_ASSERT_EQUAL_STRING("Kid A", library_album_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("OK Computer", library_album_name(lib, 1));
  TEST_ASSERT_EQUAL_STRING("the bends", library_album_name(lib, 2));
  library_artist_albums(lib, beatles, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(3, first);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("Abbey Road", library_album_name(lib, 3));
  TEST_ASSERT_EQUAL_STRING("Help!", library_album_name(lib, 4));
  for (uint32_t al = 0; al < 5; al++) {
    TEST_ASSERT_EQUAL_UINT32(al < 3 ? radiohead : beatles,
                             library_album_artist(lib, al));
  }
  library_album_tracks(lib, 1, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(1, first);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("Airbag", track_info(lib, 1).title);
  TEST_ASSERT_EQUAL_STRING("Paranoid Android", track_info(lib, 2).title);
  assert_well_formed(lib);
}

void test_same_album_name_under_two_artists_is_two_albums(void) {
  const library_track_in_t tracks[] = {
      trk("q/1.flac", "Bohemian Rhapsody", "Queen", "Greatest Hits", "Queen", 1,
          0),
      trk("a/1.flac", "Waterloo", "ABBA", "Greatest Hits", "ABBA", 1, 0),
      trk("q/2.flac", "Killer Queen", "Queen", "greatest hits", "Queen", 2, 0),
      trk("a/2.flac", "SOS", "ABBA", "Greatest Hits", "ABBA", 2, 0),
  };
  const library_t *lib = build(tracks, 4);
  TEST_ASSERT_EQUAL_UINT32(2, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(2, library_album_count(lib));
  TEST_ASSERT_EQUAL_STRING("Greatest Hits", library_album_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("Greatest Hits", library_album_name(lib, 1));
  TEST_ASSERT_EQUAL_UINT32(artist_named(lib, "ABBA"),
                           library_album_artist(lib, 0));
  TEST_ASSERT_EQUAL_UINT32(artist_named(lib, "Queen"),
                           library_album_artist(lib, 1));
  uint32_t first, count;
  library_album_tracks(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("Waterloo", track_info(lib, first).title);
  library_album_tracks(lib, 1, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", track_info(lib, first).title);
  // The name itself is stored once.
  TEST_ASSERT_EQUAL_PTR(library_album_name(lib, 0), library_album_name(lib, 1));
  assert_well_formed(lib);
}

void test_compilation_is_one_album_under_its_group_artist(void) {
  const library_track_in_t tracks[] = {
      trk("va/now/01.flac", "Song One", "Blur", "Now 30", "Various Artists", 1,
          0),
      trk("va/now/02.flac", "Song Two", "Oasis", "Now 30", "Various Artists", 2,
          0),
      trk("va/now/03.flac", "Song Three", "Pulp", "Now 30", "Various Artists",
          3, 0),
  };
  const library_t *lib = build(tracks, 3);
  // The track artists are not artists of the library; only the group is.
  TEST_ASSERT_EQUAL_UINT32(1, library_artist_count(lib));
  TEST_ASSERT_EQUAL_UINT32(1, library_album_count(lib));
  TEST_ASSERT_EQUAL_STRING("Various Artists", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("Now 30", library_album_name(lib, 0));
  uint32_t first, count;
  library_album_tracks(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(3, count);
  const char *const artists[] = {"Blur", "Oasis", "Pulp"};
  for (uint32_t t = 0; t < 3; t++) {
    library_track_t info = track_info(lib, t);
    TEST_ASSERT_EQUAL_STRING(artists[t], info.artist);
    TEST_ASSERT_EQUAL_UINT32(0, info.group_artist);
    TEST_ASSERT_EQUAL_UINT32(0, info.album);
  }
  assert_well_formed(lib);
}

void test_album_tracks_are_ordered_by_disc_then_track_then_file_name(void) {
  const library_track_in_t tracks[] = {
      trk("x/d2-02.flac", "F", "A", "Album", "A", 2, 2),
      trk("x/d1-03.flac", "C", "A", "Album", "A", 3, 1),
      trk("x/d2-01.flac", "E", "A", "Album", "A", 1, 2),
      trk("x/d1-01.flac", "A", "A", "Album", "A", 1, 1),
      // Same disc and number twice: the file name decides, ignoring case.
      trk("x/d1-02 b.flac", "B2", "A", "Album", "A", 2, 1),
      trk("x/D1-02 a.flac", "B1", "A", "Album", "A", 2, 1),
      // A number above 255, and a disc with no number, which comes first.
      trk("x/d1-300.flac", "D", "A", "Album", "A", 300, 1),
      trk("x/bonus.flac", "Z", "A", "Album", "A", 9, 0),
  };
  const library_t *lib = build(tracks, 8);
  const char *const want[] = {"Z", "A", "B1", "B2", "C", "D", "E", "F"};
  uint32_t first, count;
  library_album_tracks(lib, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(8, count);
  for (uint32_t t = 0; t < 8; t++) {
    TEST_ASSERT_EQUAL_STRING(want[t], track_info(lib, first + t).title);
  }
  TEST_ASSERT_EQUAL_UINT16(300, track_info(lib, 5).track_no);
  assert_well_formed(lib);
}

void test_tracks_without_numbers_fall_back_to_file_name_order(void) {
  const library_track_in_t tracks[] = {
      trk("x/delta.flac", "4", "A", "Album", "A", 0, 0),
      trk("x/Bravo.flac", "2", "A", "Album", "A", 0, 0),
      trk("x/alpha.flac", "1", "A", "Album", "A", 0, 0),
      trk("x/Charlie.flac", "3", "A", "Album", "A", 0, 0),
      // The file name, not the folder, decides.
      trk("a/echo.flac", "5", "A", "Album", "A", 0, 0),
  };
  const library_t *lib = build(tracks, 5);
  for (uint32_t t = 0; t < 5; t++) {
    char want[2] = {(char)('1' + t), '\0'};
    TEST_ASSERT_EQUAL_STRING(want, track_info(lib, t).title);
  }
  assert_well_formed(lib);
}

void test_artist_tracks_are_a_contiguous_range(void) {
  const library_track_in_t tracks[] = {
      trk("b/y/1.flac", "b-y-1", "B", "Y", "B", 1, 0),
      trk("a/x/2.flac", "a-x-2", "A", "X", "A", 2, 0),
      trk("b/x/1.flac", "b-x-1", "B", "X", "B", 1, 0),
      trk("c/x/1.flac", "c-x-1", "C", "X", "C", 1, 0),
      trk("b/y/2.flac", "b-y-2", "B", "Y", "B", 2, 0),
      trk("a/x/1.flac", "a-x-1", "A", "X", "A", 1, 0),
      trk("b/x/2.flac", "b-x-2", "B", "X", "B", 2, 0),
  };
  const library_t *lib = build(tracks, 7);
  uint32_t first, count;
  library_artist_tracks(lib, artist_named(lib, "A"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, first);
  TEST_ASSERT_EQUAL_UINT32(2, count);
  library_artist_tracks(lib, artist_named(lib, "B"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(2, first);
  TEST_ASSERT_EQUAL_UINT32(4, count);
  // Album by album, each in its own order.
  const char *const want[] = {"b-x-1", "b-x-2", "b-y-1", "b-y-2"};
  for (uint32_t t = 0; t < 4; t++) {
    TEST_ASSERT_EQUAL_STRING(want[t], track_info(lib, first + t).title);
  }
  library_artist_tracks(lib, artist_named(lib, "C"), &first, &count);
  TEST_ASSERT_EQUAL_UINT32(6, first);
  TEST_ASSERT_EQUAL_UINT32(1, count);
  assert_well_formed(lib);
}

void test_albums_by_name_and_songs_by_title_are_sorted(void) {
  const library_track_in_t tracks[] = {
      trk("z/1.flac", "yesterday", "Zed", "Mirror", "Zed", 1, 0),
      trk("z/2.flac", "Angel", "Zed", "Mirror", "Zed", 2, 0),
      trk("a/1.flac", "Zebra", "Abe", "zoo", "Abe", 1, 0),
      trk("a/2.flac", "angel", "Abe", "zoo", "Abe", 2, 0),
      trk("m/1.flac", "Intro", "Moe", "Atlas", "Moe", 1, 0),
      trk("m/2.flac", "Intro", "Moe", "Mirror", "Moe", 1, 0),
      trk("m/3.flac", "10 Years", "Moe", "Mirror", "Moe", 2, 0),
  };
  const library_t *lib = build(tracks, 7);
  // Albums by artist, then name: Abe/zoo, Moe/Atlas, Moe/Mirror, Zed/Mirror.
  TEST_ASSERT_EQUAL_UINT32(4, library_album_count(lib));
  // A to Z over all of them; the two Mirrors in album order.
  const uint32_t albums[] = {1, 2, 3, 0};
  for (uint32_t i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_UINT32(albums[i], library_album_by_name(lib, i));
  }
  // Tracks: 0 Zebra, 1 angel, 2 Intro (Atlas), 3 Intro (Mirror), 4 10 Years,
  // 5 yesterday, 6 Angel. Songs A to Z: "10 Years", then "Angel" before
  // "angel" (same letters: capitals first), the two Intros in track order,
  // "yesterday", "Zebra".
  const char *const titles[] = {"10 Years", "Angel",     "angel", "Intro",
                                "Intro",    "yesterday", "Zebra"};
  const uint32_t songs[] = {4, 6, 1, 2, 3, 5, 0};
  for (uint32_t i = 0; i < 7; i++) {
    uint32_t t = library_song_by_title(lib, i);
    TEST_ASSERT_EQUAL_UINT32(songs[i], t);
    TEST_ASSERT_EQUAL_STRING(titles[i], track_info(lib, t).title);
  }
  assert_well_formed(lib);
}

// The rule for text that differs only in case, which the file format fixes:
// plain byte order, so the first letter that differs decides and capitals
// come first.
void test_names_differing_only_in_case_are_in_byte_order(void) {
  const library_track_in_t tracks[] = {
      trk("x/1.flac", "ab", "A", "B", "A", 1, 0),
      trk("x/2.flac", "aB", "A", "B", "A", 2, 0),
      trk("x/3.flac", "Ab", "A", "B", "A", 3, 0),
      trk("x/4.flac", "AB", "A", "B", "A", 4, 0),
      trk("x/5.flac", "aa", "A", "B", "A", 5, 0),
      trk("x/6.flac", "AC", "A", "B", "A", 6, 0),
  };
  const library_t *lib = build(tracks, 6);
  const char *const want[] = {"aa", "AB", "Ab", "aB", "ab", "AC"};
  for (uint32_t i = 0; i < 6; i++) {
    TEST_ASSERT_EQUAL_STRING(
        want[i], track_info(lib, library_song_by_title(lib, i)).title);
  }
  assert_well_formed(lib);
}

// --- Paths -----------------------------------------------------------------

static const char *const ODD_PATHS[] = {
    "music/16 - Kanye West - Hey Mama.flac",
    "lib1000/Artist 03/Album 07/05 Track.flac",
    "top level.mp3",
    "Musik/Bj\xC3\xB6rk/Hom\xC3\xB3genic/01 J\xC3\xB3ga.flac",
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E/\xE9\x9F\xB3\xE6\xA5\xBD.mp3",
    "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/deep one.flac",
    "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/deep two.flac",
    "music/Hey Mama.flac",
    "music /Hey Mama.flac",
    "Music/Hey Mama.flac",
    "music/hey mama.flac",
    "a",
    "a/b",
};
#define ODD_PATH_COUNT (sizeof(ODD_PATHS) / sizeof(ODD_PATHS[0]))

static char odd_titles[ODD_PATH_COUNT][8];

static const library_t *build_odd_paths(void) {
  static library_track_in_t tracks[ODD_PATH_COUNT];
  for (size_t i = 0; i < ODD_PATH_COUNT; i++) {
    snprintf(odd_titles[i], sizeof(odd_titles[i]), "T%02u", (unsigned)i);
    tracks[i] = trk(ODD_PATHS[i], odd_titles[i], "A", "B", "A", 0, 0);
  }
  return build(tracks, ODD_PATH_COUNT);
}

void test_track_path_round_trips(void) {
  const library_t *lib = build_odd_paths();
  for (size_t i = 0; i < ODD_PATH_COUNT; i++) {
    uint32_t t = track_at(lib, ODD_PATHS[i]);
    TEST_ASSERT_EQUAL_STRING(odd_titles[i], track_info(lib, t).title);
    memset(path_a, 'x', sizeof(path_a));
    size_t len = library_track_path(lib, t, path_a, sizeof(path_a));
    TEST_ASSERT_EQUAL_size_t(strlen(ODD_PATHS[i]), len);
    TEST_ASSERT_EQUAL_STRING(ODD_PATHS[i], path_a);
  }
  assert_well_formed(lib);
}

void test_track_path_reports_needed_length_when_the_buffer_is_small(void) {
  library_track_in_t in =
      trk("music/Album/01 Song.flac", "Song", "A", "B", "A", 1, 0);
  const library_t *lib = build(&in, 1);
  size_t need = strlen(in.path);
  char out[40];

  // Exactly enough.
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, need + 1));
  TEST_ASSERT_EQUAL_STRING(in.path, out);
  TEST_ASSERT_EQUAL_CHAR('x', out[need + 1]);

  // One too few: cut, terminated, nothing written past the size given.
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, need));
  TEST_ASSERT_EQUAL_STRING("music/Album/01 Song.fla", out);
  TEST_ASSERT_EQUAL_CHAR('x', out[need]);

  // Cut inside the folder part.
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, 4));
  TEST_ASSERT_EQUAL_STRING("mus", out);
  TEST_ASSERT_EQUAL_CHAR('x', out[4]);

  // Cut exactly between folder and file name.
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, 13));
  TEST_ASSERT_EQUAL_STRING("music/Album/", out);

  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, 1));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_CHAR('x', out[1]);

  // No room at all: only the length.
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, out, 0));
  TEST_ASSERT_EQUAL_CHAR('x', out[0]);
  TEST_ASSERT_EQUAL_size_t(need, library_track_path(lib, 0, NULL, 0));
}

void test_find_path_finds_every_track_and_rejects_others(void) {
  const library_t *lib = build_odd_paths();
  memset(seen, 0, ODD_PATH_COUNT);
  for (size_t i = 0; i < ODD_PATH_COUNT; i++) {
    uint32_t t = track_at(lib, ODD_PATHS[i]);
    TEST_ASSERT_FALSE(seen[t]); // a different track for every path
    seen[t] = true;
  }
  const char *const absent[] = {
      "",
      "/",
      "music",
      "music/",
      "music/16 - Kanye West - Hey Mama.fla",
      "music/16 - Kanye West - Hey Mama.flac ",
      "music/16 - Kanye West - Hey Mama.flacx",
      "/music/16 - Kanye West - Hey Mama.flac",
      "MUSIC/HEY MAMA.FLAC",
      "music/Hey mama.flac",
      "music//Hey Mama.flac",
      "Hey Mama.flac",
      "top level.flac",
      "x/top level.mp3",
      "a/b/c",
      "a/",
      "b",
      "A",
      "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/deep two.flac",
      "Musik/Bjork/Homogenic/01 Joga.flac",
      "zzz/nothing.flac",
  };
  for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
    uint32_t t = 12345;
    TEST_ASSERT_FALSE_MESSAGE(library_find_path(lib, absent[i], &t), absent[i]);
    TEST_ASSERT_EQUAL_UINT32(12345, t); // untouched when nothing is found
  }
  // The caller may not want the number.
  TEST_ASSERT_TRUE(library_find_path(lib, "a/b", NULL));
  TEST_ASSERT_FALSE(library_find_path(lib, NULL, NULL));
}

// The scan never hands over the same file twice, but if a caller does, both
// are kept and the lower-numbered one is found.
void test_the_same_path_twice_is_two_tracks(void) {
  const library_track_in_t tracks[] = {
      trk("x/1.flac", "Second", "A", "B", "A", 2, 0),
      trk("x/1.flac", "First", "A", "B", "A", 1, 0),
  };
  const library_t *lib = build(tracks, 2);
  TEST_ASSERT_EQUAL_UINT32(2, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(0, track_at(lib, "x/1.flac"));
  TEST_ASSERT_EQUAL_STRING("First", track_info(lib, 0).title);
  assert_well_formed(lib);
}

void test_out_of_range_numbers_are_safe(void) {
  const library_track_in_t tracks[] = {
      trk("a/1.flac", "One", "A", "X", "A", 1, 0),
      trk("b/1.flac", "Two", "B", "Y", "B", 1, 0),
  };
  const library_t *lib = build(tracks, 2);
  const uint32_t beyond[] = {2, 3, 65535, 65536, 0x7FFFFFFFu, 0xFFFFFFFFu};
  for (size_t i = 0; i < sizeof(beyond) / sizeof(beyond[0]); i++) {
    uint32_t n = beyond[i];
    // Names come back empty, not NULL: a menu that asks for one row too many
    // shows a blank line instead of crashing the player.
    TEST_ASSERT_EQUAL_STRING("", library_artist_name(lib, n));
    TEST_ASSERT_EQUAL_STRING("", library_album_name(lib, n));
    uint32_t first = 77, count = 77;
    library_artist_albums(lib, n, &first, &count);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    first = count = 77;
    library_artist_tracks(lib, n, &first, &count);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    first = count = 77;
    library_album_tracks(lib, n, &first, &count);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    // Numbers come back as one that is itself out of range everywhere.
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_album_artist(lib, n));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_album_by_name(lib, n));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_song_by_title(lib, n));
    library_track_t info;
    memset(&info, 0x55, sizeof(info));
    TEST_ASSERT_FALSE(library_track(lib, n, &info));
    TEST_ASSERT_EQUAL_STRING("", info.title);
    TEST_ASSERT_EQUAL_STRING("", info.artist);
    char out[8] = "xxxxxxx";
    TEST_ASSERT_EQUAL_size_t(0, library_track_path(lib, n, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_size_t(0, library_track_path(lib, n, NULL, 0));
  }
  // The last valid ones still answer.
  TEST_ASSERT_EQUAL_STRING("B", library_artist_name(lib, 1));
  TEST_ASSERT_EQUAL_STRING("Two", track_info(lib, 1).title);
}

// --- Determinism -----------------------------------------------------------

#define ORDER_TRACKS 600u
static library_track_in_t order_tracks[ORDER_TRACKS];
static gen_track_t order_gen[ORDER_TRACKS];
static size_t order_of[ORDER_TRACKS];

static void make_order_tracks(void) {
  for (uint32_t i = 0; i < ORDER_TRACKS; i++) {
    gen_track(i, &order_gen[i]);
    order_tracks[i] = order_gen[i].in;
  }
}

void test_order_of_adding_does_not_change_the_result(void) {
  make_order_tracks();
  const library_t *lib =
      build_ordered(ARENA_A, ARENA_BYTES, order_tracks, ORDER_TRACKS, NULL);
  size_t len_a = save_image(lib, img_a, sizeof(img_a));
  assert_well_formed(lib);

  // Backwards.
  for (size_t i = 0; i < ORDER_TRACKS; i++) {
    order_of[i] = ORDER_TRACKS - 1 - i;
  }
  lib =
      build_ordered(ARENA_B, ARENA_BYTES, order_tracks, ORDER_TRACKS, order_of);
  size_t len_b = save_image(lib, img_b, sizeof(img_b));
  TEST_ASSERT_EQUAL_size_t(len_a, len_b);
  TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, len_a));

  // Scattered: 7 has no factor in common with 600, so this visits every one.
  for (size_t i = 0; i < ORDER_TRACKS; i++) {
    order_of[i] = (i * 7 + 311) % ORDER_TRACKS;
  }
  lib =
      build_ordered(ARENA_B, ARENA_BYTES, order_tracks, ORDER_TRACKS, order_of);
  len_b = save_image(lib, img_b, sizeof(img_b));
  TEST_ASSERT_EQUAL_size_t(len_a, len_b);
  TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, len_a));

  // And in a smaller arena, where the working tables are a different size.
  lib = build_ordered(ARENA_B, ARENA_BYTES / 8, order_tracks, ORDER_TRACKS,
                      order_of);
  len_b = save_image(lib, img_b, sizeof(img_b));
  TEST_ASSERT_EQUAL_size_t(len_a, len_b);
  TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, len_a));
}

// Ties everywhere: tracks that differ in one field only, so that every
// tie-break is needed to get the same file from two orders.
void test_order_of_adding_does_not_matter_when_tracks_are_nearly_equal(void) {
  library_track_in_t tracks[] = {
      trk("x/1.flac", "Same", "A", "Album", "A", 1, 1),
      trk("y/1.flac", "Same", "A", "Album", "A", 1, 1),  // other folder
      trk("x/1.flac", "Other", "A", "Album", "A", 1, 1), // same path
      trk("x/1.flac", "Same", "B", "Album", "A", 1, 1),  // other track artist
      trk("x/1.flac", "Same", "A", "Album", "A", 1, 1),  // other length
      trk("x/1.flac", "Same", "A", "Album", "A", 1, 1),  // other format
      trk("x/1.flac", "Same", "A", "Album", "A", 1, 1),  // a true duplicate
      trk("x/1.flac", "same", "A", "Album", "A", 1, 1),  // case of the title
      trk("X/1.flac", "Same", "A", "Album", "A", 1, 1),  // case of the folder
      trk("x/1.FLAC", "Same", "A", "Album", "A", 1, 1),  // case of the name
  };
  tracks[4].duration_ms += 1;
  tracks[5].format = TAGS_FORMAT_MP3;
  const size_t n = sizeof(tracks) / sizeof(tracks[0]);
  const library_t *lib = build_ordered(ARENA_A, ARENA_BYTES, tracks, n, NULL);
  size_t len_a = save_image(lib, img_a, sizeof(img_a));
  assert_well_formed(lib);
  for (size_t shift = 1; shift < n; shift++) {
    size_t order[16];
    for (size_t i = 0; i < n; i++) {
      order[i] = (i * 3 + shift) % n; // 3 has no factor in common with 10
    }
    lib = build_ordered(ARENA_B, ARENA_BYTES, tracks, n, order);
    size_t len_b = save_image(lib, img_b, sizeof(img_b));
    TEST_ASSERT_EQUAL_size_t(len_a, len_b);
    TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, len_a));
  }
}

// "First spelling kept" is the one thing that depends on the order. It
// changes the name shown and nothing else: every number, range and order is
// the same.
void test_order_of_adding_changes_only_the_spelling_kept(void) {
  const library_track_in_t tracks[] = {
      trk("q/1.flac", "One", "Queen", "Hits", "Queen", 1, 0),
      trk("q/2.flac", "Two", "Queen", "HITS", "QUEEN", 2, 0),
      trk("a/1.flac", "Three", "Abba", "hits", "Abba", 1, 0),
      trk("z/1.flac", "Four", "ZZ Top", "Hits", "ZZ Top", 1, 0),
      trk("z/2.flac", "Five", "ZZ Top", "hitS", "zz top", 2, 0),
      trk("q/3.flac", "Six", "Queen", "A Night", "queen", 1, 0),
  };
  const size_t backwards[] = {5, 4, 3, 2, 1, 0};
  const library_t *a = build_ordered(ARENA_A, ARENA_BYTES, tracks, 6, NULL);
  const library_t *b =
      build_ordered(ARENA_B, ARENA_BYTES, tracks, 6, backwards);
  TEST_ASSERT_EQUAL_STRING("Queen", library_artist_name(a, 1));
  TEST_ASSERT_EQUAL_STRING("queen", library_artist_name(b, 1));
  TEST_ASSERT_EQUAL_STRING("ZZ Top", library_artist_name(a, 2));
  TEST_ASSERT_EQUAL_STRING("zz top", library_artist_name(b, 2));
  TEST_ASSERT_EQUAL_UINT32(3, library_artist_count(b));
  TEST_ASSERT_EQUAL_UINT32(4, library_album_count(a));
  TEST_ASSERT_EQUAL_UINT32(4, library_album_count(b));
  for (uint32_t i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL_INT(
        0, fold_cmp(library_album_name(a, i), library_album_name(b, i)));
    TEST_ASSERT_EQUAL_UINT32(library_album_artist(a, i),
                             library_album_artist(b, i));
    TEST_ASSERT_EQUAL_UINT32(library_album_by_name(a, i),
                             library_album_by_name(b, i));
  }
  for (uint32_t t = 0; t < 6; t++) {
    library_track_t ta = track_info(a, t);
    library_track_t tb = track_info(b, t);
    TEST_ASSERT_EQUAL_STRING(ta.title, tb.title);
    TEST_ASSERT_EQUAL_UINT32(ta.album, tb.album);
    TEST_ASSERT_EQUAL_UINT32(library_song_by_title(a, t),
                             library_song_by_title(b, t));
  }
  assert_well_formed(a);
  assert_well_formed(b);
}

// The name kept is the first spelling without its surrounding spaces.
void test_kept_names_are_trimmed(void) {
  const library_track_in_t tracks[] = {
      trk("q/1.flac", " One ", "  Queen ", " Jazz  ", "  Queen ", 1, 0),
  };
  const library_t *lib = build(tracks, 1);
  TEST_ASSERT_EQUAL_STRING("Queen", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_STRING("Jazz", library_album_name(lib, 0));
  // Only the names things are grouped by: the rest is kept as given.
  TEST_ASSERT_EQUAL_STRING(" One ", track_info(lib, 0).title);
  TEST_ASSERT_EQUAL_STRING("  Queen ", track_info(lib, 0).artist);
}

// --- Saving and loading ----------------------------------------------------

static size_t make_image(uint32_t tracks) {
  library_builder_t *b = library_builder_begin(ARENA_A, ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL_UINT32(tracks, gen_fill(b, tracks));
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  return save_image(lib, img_a, sizeof(img_a));
}

void test_save_and_load_round_trip(void) {
  library_builder_t *b = library_builder_begin(ARENA_A, ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL_UINT32(700, gen_fill(b, 700));
  const library_t *built = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(built);
  size_t n = save_image(built, img_a, sizeof(img_a));

  const library_t *loaded = NULL;
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &loaded));
  TEST_ASSERT_NOT_NULL(loaded);
  TEST_ASSERT_TRUE((const void *)loaded != (const void *)built);
  // Both are alive at once, each in its own arena.
  assert_same_answers(built, loaded);
  assert_well_formed(loaded);
  for (uint32_t i = 0; i < 700; i += 13) {
    assert_holds_generated(loaded, i);
  }

  // What was loaded is the file, byte for byte, and saves as the same file.
  TEST_ASSERT_EQUAL_INT(0, memcmp(loaded, img_a, n));
  TEST_ASSERT_EQUAL_size_t(n, save_image(loaded, img_b, sizeof(img_b)));
  TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, n));

  // The built one going away does not disturb the loaded one.
  memset(ARENA_A, 0xEE, ARENA_BYTES);
  assert_well_formed(loaded);

  // An arena of exactly the index's size is enough to load into.
  TEST_ASSERT_EQUAL(LIBRARY_OK, load_image(img_a, n, ARENA_A, n, &loaded));
  assert_well_formed(loaded);
}

void test_checksum_is_the_standard_crc32(void) {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                          crc32_bitwise((const uint8_t *)"123456789", 9));
  size_t n = make_image(60);
  TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(img_a + CRC_FROM, n - CRC_FROM),
                          rd32(img_a + H_CRC));
  TEST_ASSERT_EQUAL_UINT32(n, rd32(img_a + H_TOTAL));
  TEST_ASSERT_EQUAL_UINT32(60, rd32(img_a + H_TRACKS));
  TEST_ASSERT_EQUAL_UINT32(HEADER_BYTES, rd32(img_a + H_OFF_TRACKS));
  TEST_ASSERT_EQUAL_MEMORY("DAPL", img_a + H_MAGIC, 4);
  TEST_ASSERT_EQUAL_UINT32(1, rd32(img_a + H_VERSION));
}

void test_load_rejects_bad_magic(void) {
  size_t n = make_image(60);
  img_a[H_MAGIC] ^= 0x20; // "dAPL"
  assert_rejected(img_a, n, "magic");
  // Not an index at all.
  memset(img_a, 0, 200);
  assert_rejected(img_a, 200, "zeros");
  memcpy(img_a, "fLaC", 4);
  assert_rejected(img_a, 200, "flac");
}

void test_load_rejects_wrong_version(void) {
  size_t n = make_image(60);
  wr32(img_a + H_VERSION, 2);
  assert_rejected(img_a, n, "version 2");
  wr32(img_a + H_VERSION, 0);
  assert_rejected(img_a, n, "version 0");
  // Even with the checksum made right again.
  fix_checksum(img_a, n);
  assert_rejected(img_a, n, "version 0, good checksum");
}

void test_load_rejects_a_truncated_file(void) {
  size_t n = make_image(60);
  const size_t cuts[] = {0,
                         1,
                         3,
                         4,
                         15,
                         16,
                         HEADER_BYTES - 1,
                         HEADER_BYTES,
                         HEADER_BYTES + 1,
                         n / 3,
                         n / 2,
                         n - 5,
                         n - 4,
                         n - 1};
  for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
    char what[32];
    snprintf(what, sizeof(what), "cut at %u", (unsigned)cuts[i]);
    assert_rejected(img_a, cuts[i], what);
  }
  // The whole of it still loads.
  const library_t *lib = NULL;
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &lib));
}

void test_load_rejects_a_file_with_something_after_the_index(void) {
  size_t n = make_image(60);
  img_a[n] = 0;
  assert_rejected(img_a, n + 1, "one byte more");
  memcpy(img_a + n, img_a, n);
  assert_rejected(img_a, 2 * n, "twice");
}

void test_load_rejects_a_flipped_bit_anywhere(void) {
  size_t n = make_image(60);
  for (size_t i = 0; i < 64; i++) {
    // Spread over the whole file, first and last byte included.
    size_t at = (i == 63) ? n - 1 : i * n / 64;
    uint8_t bit = (uint8_t)(1u << (i % 8));
    img_a[at] ^= bit;
    const library_t *lib = (const library_t *)img_a;
    library_result_t r = load_image(img_a, n, ARENA_B, ARENA_BYTES, &lib);
    char what[48];
    snprintf(what, sizeof(what), "bit %u of byte %u", (unsigned)(i % 8),
             (unsigned)at);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(LIBRARY_OK, r, what);
    TEST_ASSERT_NULL_MESSAGE(lib, what);
    img_a[at] ^= bit;
  }
  // Every bit of the header, too: none of it is outside the checks.
  for (size_t at = 0; at < HEADER_BYTES; at++) {
    for (int b = 0; b < 8; b++) {
      img_a[at] ^= (uint8_t)(1u << b);
      const library_t *lib = (const library_t *)img_a;
      library_result_t r = load_image(img_a, n, ARENA_B, ARENA_BYTES, &lib);
      TEST_ASSERT_NOT_EQUAL(LIBRARY_OK, r);
      TEST_ASSERT_NULL(lib);
      img_a[at] ^= (uint8_t)(1u << b);
    }
  }
  const library_t *lib = NULL;
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &lib));
}

void test_load_rejects_an_index_larger_than_the_arena(void) {
  size_t n = make_image(60);
  const library_t *lib = (const library_t *)img_a;
  TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, ARENA_B, n - 1, &lib));
  TEST_ASSERT_NULL(lib);
  lib = (const library_t *)img_a;
  TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, ARENA_B, n / 2, &lib));
  TEST_ASSERT_NULL(lib);
  lib = (const library_t *)img_a;
  TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, ARENA_B, 10, &lib));
  TEST_ASSERT_NULL(lib);
  lib = (const library_t *)img_a;
  TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, ARENA_B, 0, &lib));
  TEST_ASSERT_NULL(lib);
  // The arena beyond the size given is not touched.
  memset(ARENA_B, 0x5A, ARENA_BYTES);
  TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, ARENA_B, n - 4, &lib));
  const uint8_t *arena = ARENA_B;
  for (size_t i = n - 4; i < n + 64; i++) {
    TEST_ASSERT_EQUAL_HEX8(0x5A, arena[i]);
  }
  TEST_ASSERT_EQUAL(LIBRARY_OK, load_image(img_a, n, ARENA_B, n, &lib));
  TEST_ASSERT_NOT_NULL(lib);
}

// Damage one field of a good file, make the checksum right again, and load:
// only the checks on the structure itself can turn it away.
typedef struct {
  const char *what;
  uint32_t at;
  int width; // bytes: 1, 2 or 4
  uint32_t value;
  bool harmless;
} damage_t;

void test_load_rejects_inconsistent_offsets_with_a_valid_checksum(void) {
  size_t n = make_image(60);
  memcpy(img_b, img_a, n); // the good copy
  uint32_t tracks = rd32(img_b + H_TRACKS);
  uint32_t albums = rd32(img_b + H_ALBUMS);
  uint32_t artists = rd32(img_b + H_ARTISTS);
  uint32_t dirs = rd32(img_b + H_DIRS);
  uint32_t pool = rd32(img_b + H_POOL_BYTES);
  uint32_t o_tracks = rd32(img_b + H_OFF_TRACKS);
  uint32_t o_albums = rd32(img_b + H_OFF_ALBUMS);
  uint32_t o_artists = rd32(img_b + H_OFF_ARTISTS);
  uint32_t o_dirs = rd32(img_b + H_OFF_DIRS);
  uint32_t o_songs = rd32(img_b + H_OFF_SONGS);
  uint32_t o_paths = rd32(img_b + H_OFF_PATHS);
  uint32_t o_names = rd32(img_b + H_OFF_ALBUM_NAMES);
  uint32_t o_formats = rd32(img_b + H_OFF_FORMATS);
  uint32_t o_pool = rd32(img_b + H_OFF_POOL);
  TEST_ASSERT_EQUAL_UINT32(60, tracks);
  TEST_ASSERT_TRUE(albums >= 3);
  TEST_ASSERT_TRUE(artists >= 2);
  TEST_ASSERT_TRUE(dirs >= 3);
  uint32_t last_track = o_tracks + (tracks - 1) * TRACK_ROW;

  const damage_t damage[] = {
      // Two controls: the checksum the test computes is the one the loader
      // wants, and a changed field that nothing follows is let through.
      {"nothing changed", H_TRACKS, 4, tracks, true},
      {"a track's length", o_tracks + TR_DURATION, 4, 123456, true},
      {"a track's format", o_formats + 5, 1, 9, true},

      // Counts.
      {"one track more", H_TRACKS, 4, tracks + 1, false},
      {"one track fewer", H_TRACKS, 4, tracks - 1, false},
      {"no tracks", H_TRACKS, 4, 0, false},
      {"65536 tracks more", H_TRACKS, 4, tracks + 65536, false},
      {"a huge track count", H_TRACKS, 4, 0xFFFFFFF0u, false},
      {"one album more", H_ALBUMS, 4, albums + 1, false},
      {"two albums fewer", H_ALBUMS, 4, albums - 2, false},
      {"one artist more", H_ARTISTS, 4, artists + 1, false},
      {"one artist fewer", H_ARTISTS, 4, artists - 1, false},
      {"no artists", H_ARTISTS, 4, 0, false},
      {"one folder more", H_DIRS, 4, dirs + 1, false},
      {"one folder fewer", H_DIRS, 4, dirs - 1, false},
      {"a longer string pool", H_POOL_BYTES, 4, pool + 4, false},
      {"a huge string pool", H_POOL_BYTES, 4, 0xFFFFFFFCu, false},
      {"a shorter string pool", H_POOL_BYTES, 4, pool - 8, false},
      {"a smaller total", H_TOTAL, 4, (uint32_t)n - 4, false},
      {"a larger total", H_TOTAL, 4, (uint32_t)n + 4, false},

      // Where the tables are.
      {"tracks table moved", H_OFF_TRACKS, 4, o_tracks + 4, false},
      {"albums table moved", H_OFF_ALBUMS, 4, o_albums - 8, false},
      {"artists table moved", H_OFF_ARTISTS, 4, o_artists + 8, false},
      {"artists table far away", H_OFF_ARTISTS, 4, 0x40000000u, false},
      {"folders table moved", H_OFF_DIRS, 4, o_dirs + 8, false},
      {"songs order moved", H_OFF_SONGS, 4, o_songs + 2, false},
      {"path order moved", H_OFF_PATHS, 4, o_paths - 4, false},
      {"album order moved", H_OFF_ALBUM_NAMES, 4, o_names + 4, false},
      {"formats moved", H_OFF_FORMATS, 4, o_formats + 4, false},
      {"pool moved", H_OFF_POOL, 4, o_pool - 4, false},
      {"pool beyond the end", H_OFF_POOL, 4, (uint32_t)n, false},

      // Tracks.
      {"title beyond the pool", o_tracks + TR_TITLE, 4, pool, false},
      {"title far beyond", last_track + TR_TITLE, 4, 0xFFFFFFFFu, false},
      {"artist beyond the pool", o_tracks + TR_ARTIST, 4, pool + 100, false},
      {"name beyond the pool", last_track + TR_NAME, 4, 0x80000000u, false},
      {"album that is not there", o_tracks + TRACK_ROW + TR_ALBUM, 2, albums,
       false},
      {"album 65535", last_track + TR_ALBUM, 2, 0xFFFF, false},
      {"track in another album", o_tracks + TR_ALBUM, 2, 1, false},
      {"folder that is not there", o_tracks + TR_DIR, 2, dirs, false},
      {"folder 65535", last_track + TR_DIR, 2, 0xFFFF, false},

      // Albums.
      {"album name beyond the pool", o_albums + AL_NAME, 4, pool, false},
      {"album starting past the end", o_albums + ALBUM_ROW + AL_FIRST_TRACK, 2,
       tracks, false},
      {"album starting where the one before does",
       o_albums + ALBUM_ROW + AL_FIRST_TRACK, 2, 0, false},
      {"first album not at track 0", o_albums + AL_FIRST_TRACK, 2, 1, false},
      {"albums out of order", o_albums + 2 * ALBUM_ROW + AL_FIRST_TRACK, 2, 1,
       false},
      {"album of an artist that is not there", o_albums + AL_ARTIST, 2, artists,
       false},
      {"album under another artist", o_albums + AL_ARTIST, 2, 1, false},

      // Artists.
      {"artist name beyond the pool", o_artists + AR_NAME, 4, pool + 1, false},
      {"artist starting past the end", o_artists + ARTIST_ROW + AR_FIRST_ALBUM,
       2, albums, false},
      {"artist starting where the one before does",
       o_artists + ARTIST_ROW + AR_FIRST_ALBUM, 2, 0, false},
      {"first artist not at album 0", o_artists + AR_FIRST_ALBUM, 2, 1, false},
      {"artist whose tracks start elsewhere", o_artists + AR_FIRST_TRACK, 2, 5,
       false},
      {"artist whose tracks start past the end",
       o_artists + ARTIST_ROW + AR_FIRST_TRACK, 2, 0xFFFF, false},

      // Folders.
      {"folder name beyond the pool", o_dirs + DI_NAME, 4, pool, false},
      {"folder starting past the end", o_dirs + DIR_ROW + DI_FIRST, 2, tracks,
       false},
      {"folder starting where the one before does", o_dirs + DIR_ROW + DI_FIRST,
       2, 0, false},
      {"first folder not at 0", o_dirs + DI_FIRST, 2, 1, false},

      // The three orders.
      {"song order naming a missing track", o_songs, 2, tracks, false},
      {"song order naming track 65535", o_songs + 2 * (tracks - 1), 2, 0xFFFF,
       false},
      {"path order naming a missing track", o_paths + 2 * (tracks - 1), 2,
       tracks, false},
      {"path order naming track 65535", o_paths, 2, 0xFFFF, false},
      {"album order naming a missing album", o_names, 2, albums, false},
      {"album order naming album 65535", o_names + 2 * (albums - 1), 2, 0xFFFF,
       false},

      // The pool.
      {"last string not terminated", o_pool + pool - 1, 1, 'x', false},
  };

  for (size_t i = 0; i < sizeof(damage) / sizeof(damage[0]); i++) {
    const damage_t *d = &damage[i];
    memcpy(img_a, img_b, n);
    TEST_ASSERT_TRUE_MESSAGE(d->at + (uint32_t)d->width <= n, d->what);
    if (d->width == 4) {
      wr32(img_a + d->at, d->value);
    } else if (d->width == 2) {
      wr16(img_a + d->at, d->value);
    } else {
      img_a[d->at] = (uint8_t)d->value;
    }
    if (!d->harmless) {
      // The damage is real: without a new checksum the file is refused for
      // its checksum (or its size), and that is not what is being tested.
      TEST_ASSERT_TRUE_MESSAGE(memcmp(img_a, img_b, n) != 0, d->what);
    }
    fix_checksum(img_a, n);
    TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(img_a + CRC_FROM, n - CRC_FROM),
                            rd32(img_a + H_CRC));
    const library_t *lib = (const library_t *)img_a;
    library_result_t r = load_image(img_a, n, ARENA_B, ARENA_BYTES, &lib);
    if (d->harmless) {
      TEST_ASSERT_EQUAL_MESSAGE(LIBRARY_OK, r, d->what);
      TEST_ASSERT_NOT_NULL_MESSAGE(lib, d->what);
    } else {
      TEST_ASSERT_EQUAL_MESSAGE(LIBRARY_BAD_FILE, r, d->what);
      TEST_ASSERT_NULL_MESSAGE(lib, d->what);
    }
  }

  // A file cut to match a smaller stated total, checksum made right.
  memcpy(img_a, img_b, n);
  wr32(img_a + H_TOTAL, (uint32_t)n - 4);
  fix_checksum(img_a, n - 4);
  assert_rejected(img_a, n - 4, "cut, with total and checksum to match");
}

// An arena of exactly `n` bytes whose end is the end of an array, so that
// the sanitizer stops any read past the index.
static void *arena_ending_at_a_wall(size_t n) {
  TEST_ASSERT_EQUAL_size_t(0, n % 4);
  return (uint8_t *)arena_b_words + sizeof(arena_b_words) - n;
}

// Ask everything of an index, one past the end of every list included. The
// sanitizer watches where the answers come from; this checks that every run
// lies inside its list.
static void ask_everything(const library_t *lib) {
  uint32_t n_tracks = library_track_count(lib);
  uint32_t n_albums = library_album_count(lib);
  uint32_t n_artists = library_artist_count(lib);
  uint32_t first, count;
  size_t text = 0;
  for (uint32_t a = 0; a <= n_artists; a++) {
    text += strlen(library_artist_name(lib, a));
    library_artist_albums(lib, a, &first, &count);
    TEST_ASSERT_TRUE(first <= n_albums && count <= n_albums - first);
    library_artist_tracks(lib, a, &first, &count);
    TEST_ASSERT_TRUE(first <= n_tracks && count <= n_tracks - first);
  }
  for (uint32_t al = 0; al <= n_albums; al++) {
    text += strlen(library_album_name(lib, al));
    library_album_tracks(lib, al, &first, &count);
    TEST_ASSERT_TRUE(first <= n_tracks && count <= n_tracks - first);
    if (al < n_albums) {
      TEST_ASSERT_TRUE(library_album_artist(lib, al) < n_artists);
      TEST_ASSERT_TRUE(library_album_by_name(lib, al) < n_albums);
    }
  }
  for (uint32_t t = 0; t <= n_tracks; t++) {
    library_track_t info;
    if (library_track(lib, t, &info)) {
      text += strlen(info.title) + strlen(info.artist);
      TEST_ASSERT_TRUE(info.album < n_albums);
      TEST_ASSERT_TRUE(info.group_artist < n_artists);
      TEST_ASSERT_TRUE(library_song_by_title(lib, t) < n_tracks);
    }
    size_t len = library_track_path(lib, t, path_a, sizeof(path_a));
    TEST_ASSERT_TRUE(len < sizeof(path_a));
    TEST_ASSERT_EQUAL_size_t(len, strlen(path_a));
    uint32_t found = 0;
    if (library_find_path(lib, path_a, &found)) {
      TEST_ASSERT_TRUE(found < n_tracks);
    }
  }
  TEST_ASSERT_TRUE(text < ARENA_BYTES);
}

static uint32_t rnd_state;

static uint32_t rnd(void) {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return mix(rnd_state);
}

// Thousands of files, each a good file with one to three fields overwritten
// and the checksum made right again. Whichever the loader lets through must
// be safe to ask anything of.
void test_damaged_files_with_good_checksums_never_read_outside_the_arena(void) {
  size_t n = make_image(60);
  memcpy(img_b, img_a, n);
  uint32_t o_pool = rd32(img_b + H_OFF_POOL);
  uint32_t counts[] = {rd32(img_b + H_TRACKS), rd32(img_b + H_ALBUMS),
                       rd32(img_b + H_ARTISTS), rd32(img_b + H_DIRS),
                       rd32(img_b + H_POOL_BYTES)};
  void *arena = arena_ending_at_a_wall(n);
  uint32_t accepted = 0;
  uint32_t refused = 0;
  rnd_state = 20261002;
  for (uint32_t round = 0; round < 3000; round++) {
    memcpy(img_a, img_b, n);
    uint32_t changes = 1 + rnd() % 3;
    for (uint32_t c = 0; c < changes; c++) {
      // Mostly in the tables, sometimes anywhere (the header, the text).
      uint32_t at = (rnd() % 4 != 0)
                        ? HEADER_BYTES + rnd() % (o_pool - HEADER_BYTES)
                        : rnd() % (uint32_t)n;
      at &= ~1u;
      uint32_t old = img_a[at] | (uint32_t)img_a[at + 1] << 8;
      uint32_t value;
      switch (rnd() % 8) {
      case 0:
        value = 0;
        break;
      case 1:
        value = 0xFFFF;
        break;
      case 2:
        value = old + 1;
        break;
      case 3:
        value = old - 1;
        break;
      case 4:
        value = counts[rnd() % 5]; // one past the end of some list
        break;
      case 5:
        value = counts[rnd() % 5] - 1; // the last of some list
        break;
      case 6:
        value = rnd() % 64;
        break;
      default:
        value = rnd();
        break;
      }
      wr16(img_a + at, value);
    }
    fix_checksum(img_a, n);
    const library_t *lib = NULL;
    library_result_t r = load_image(img_a, n, arena, n, &lib);
    if (r == LIBRARY_OK) {
      TEST_ASSERT_NOT_NULL(lib);
      ask_everything(lib);
      accepted++;
    } else {
      TEST_ASSERT_TRUE(r == LIBRARY_BAD_FILE || r == LIBRARY_FULL);
      TEST_ASSERT_NULL(lib);
      refused++;
    }
  }
  // Both happen often: the test means something either way.
  TEST_ASSERT_TRUE(accepted > 300);
  TEST_ASSERT_TRUE(refused > 300);
  printf("library fuzz: %u damaged files loaded and queried, %u refused\n",
         (unsigned)accepted, (unsigned)refused);
}

// A file whose three orders name tracks that exist, but in no sensible
// order, is not turned away (checking would cost a string comparison per
// track at every start-up). It must still be safe to ask anything of.
void test_a_scrambled_but_in_range_file_is_safe_to_query(void) {
  size_t n = make_image(60);
  uint32_t tracks = rd32(img_a + H_TRACKS);
  uint32_t albums = rd32(img_a + H_ALBUMS);
  uint32_t o_songs = rd32(img_a + H_OFF_SONGS);
  uint32_t o_paths = rd32(img_a + H_OFF_PATHS);
  uint32_t o_names = rd32(img_a + H_OFF_ALBUM_NAMES);
  uint32_t o_tracks = rd32(img_a + H_OFF_TRACKS);
  uint32_t pool = rd32(img_a + H_POOL_BYTES);
  for (uint32_t i = 0; i < tracks; i++) {
    wr16(img_a + o_songs + 2 * i, mix(i) % tracks);
    wr16(img_a + o_paths + 2 * i, mix(i + 99) % tracks);
    // Strings that start in the middle of other strings, or at the pool's
    // last byte.
    wr32(img_a + o_tracks + i * TRACK_ROW + TR_TITLE, mix(i + 7) % pool);
    wr32(img_a + o_tracks + i * TRACK_ROW + TR_NAME,
         (i % 2) ? pool - 1 : mix(i + 3) % pool);
  }
  for (uint32_t i = 0; i < albums; i++) {
    wr16(img_a + o_names + 2 * i, mix(i) % albums);
  }
  fix_checksum(img_a, n);
  const library_t *lib = NULL;
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, arena_ending_at_a_wall(n), n, &lib));
  TEST_ASSERT_NOT_NULL(lib);
  ask_everything(lib);
  const uint8_t *lo = (const uint8_t *)lib;
  const uint8_t *hi = lo + n;
  for (uint32_t t = 0; t < tracks; t++) {
    library_track_t info = track_info(lib, t);
    TEST_ASSERT_TRUE((const uint8_t *)info.title >= lo);
    TEST_ASSERT_TRUE((const uint8_t *)info.title + strlen(info.title) < hi);
    TEST_ASSERT_TRUE(info.album < albums);
    TEST_ASSERT_TRUE(library_song_by_title(lib, t) < tracks);
    size_t len = library_track_path(lib, t, path_a, sizeof(path_a));
    TEST_ASSERT_TRUE(len < sizeof(path_a));
    uint32_t found = 0;
    if (library_find_path(lib, path_a, &found)) {
      TEST_ASSERT_TRUE(found < tracks);
    }
  }
  for (uint32_t i = 0; i < 60; i++) {
    gen_track(i, &gen);
    uint32_t found = 0;
    if (library_find_path(lib, gen.path, &found)) {
      TEST_ASSERT_TRUE(found < tracks);
    }
  }
}

void test_io_failures_are_reported(void) {
  library_track_in_t in = trk("a/1.flac", "One", "A", "X", "A", 1, 0);
  const library_t *lib = build(&in, 1);
  size_t n = save_image(lib, img_a, sizeof(img_a));

  // A stream that cannot be written to.
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = fopen("/dev/null", "rb");
  TEST_ASSERT_NOT_NULL(open_file);
  TEST_ASSERT_EQUAL(LIBRARY_IO, library_save(lib, open_file));
  // A disk that is full: the write is accepted and the flush fails.
  fclose(open_file);
  open_file = fopen("/dev/full", "wb");
  if (open_file != NULL) {
    TEST_ASSERT_EQUAL(LIBRARY_IO, library_save(lib, open_file));
    fclose(open_file);
  }
  // A stream that cannot be read is not "a bad file".
  open_file = fopen("/dev/null", "wb");
  TEST_ASSERT_NOT_NULL(open_file);
  const library_t *loaded = lib;
  TEST_ASSERT_EQUAL(LIBRARY_IO,
                    library_load(ARENA_B, ARENA_BYTES, open_file, &loaded));
  TEST_ASSERT_NULL(loaded);
  // An empty file is.
  assert_rejected(img_a, 0, "empty");
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &loaded));
}

void test_null_arguments_and_finished_builders_are_refused(void) {
  library_track_in_t in = trk("a/1.flac", "One", "A", "X", "A", 1, 0);
  TEST_ASSERT_EQUAL(LIBRARY_FULL, library_builder_add(NULL, &in));
  TEST_ASSERT_NULL(library_builder_finish(NULL));

  library_builder_t *b = library_builder_begin(ARENA_A, ARENA_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL(LIBRARY_FULL, library_builder_add(b, NULL));
  TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, &in));
  // Missing text is empty text.
  library_track_in_t blank;
  memset(&blank, 0, sizeof(blank));
  TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, &blank));
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(2, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(2, library_artist_count(lib));
  TEST_ASSERT_EQUAL_STRING("", library_artist_name(lib, 0));
  TEST_ASSERT_EQUAL_UINT32(0, track_at(lib, ""));
  TEST_ASSERT_EQUAL_STRING("", track_info(lib, 0).title);
  assert_well_formed(lib);

  // The builder is finished: its arena now holds the index.
  TEST_ASSERT_EQUAL(LIBRARY_FULL, library_builder_add(b, &in));
  TEST_ASSERT_NULL(library_builder_finish(b));
  TEST_ASSERT_EQUAL_UINT32(2, library_track_count(lib));
  assert_well_formed(lib);

  FILE *f = fresh_tmp();
  const library_t *out = lib;
  TEST_ASSERT_EQUAL(LIBRARY_IO, library_save(NULL, f));
  TEST_ASSERT_EQUAL(LIBRARY_IO, library_save(lib, NULL));
  TEST_ASSERT_EQUAL(LIBRARY_IO, library_load(ARENA_B, ARENA_BYTES, NULL, &out));
  TEST_ASSERT_NULL(out);
  TEST_ASSERT_EQUAL(LIBRARY_BAD_FILE,
                    library_load(ARENA_B, ARENA_BYTES, f, NULL));

  // No library is an empty library.
  TEST_ASSERT_EQUAL_UINT32(0, library_bytes(NULL));
  TEST_ASSERT_EQUAL_UINT32(0, library_track_count(NULL));
  TEST_ASSERT_EQUAL_UINT32(0, library_album_count(NULL));
  TEST_ASSERT_EQUAL_UINT32(0, library_artist_count(NULL));
  TEST_ASSERT_EQUAL_STRING("", library_artist_name(NULL, 0));
  TEST_ASSERT_EQUAL_STRING("", library_album_name(NULL, 0));
  TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_album_artist(NULL, 0));
  TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_album_by_name(NULL, 0));
  TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, library_song_by_title(NULL, 0));
  uint32_t first = 7, count = 7;
  library_artist_albums(NULL, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, count);
  library_artist_tracks(NULL, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, count);
  library_album_tracks(NULL, 0, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(0, count);
  library_track_t info;
  TEST_ASSERT_FALSE(library_track(NULL, 0, &info));
  TEST_ASSERT_FALSE(library_track(lib, 0, NULL));
  TEST_ASSERT_EQUAL_size_t(0, library_track_path(NULL, 0, path_a, 10));
  TEST_ASSERT_FALSE(library_find_path(NULL, "a/1.flac", NULL));
}

// --- Arenas ----------------------------------------------------------------

void test_begin_refuses_an_arena_too_small_to_start(void) {
  TEST_ASSERT_NULL(library_builder_begin(NULL, 1000));
  TEST_ASSERT_NULL(library_builder_begin(ARENA_A, 0));
  TEST_ASSERT_NULL(library_builder_begin(ARENA_A, 16));
  TEST_ASSERT_NULL(library_builder_begin(ARENA_A, HEADER_BYTES));
}

// An arena that starts at an odd address works: the index sits at the next
// multiple of four inside it.
void test_the_arena_need_not_be_aligned(void) {
  make_order_tracks();
  const library_t *lib =
      build_ordered(ARENA_A, ARENA_BYTES, order_tracks, 200, NULL);
  size_t n = save_image(lib, img_a, sizeof(img_a));
  for (size_t skew = 1; skew < 4; skew++) {
    uint8_t *odd = (uint8_t *)ARENA_B + skew;
    lib = build_ordered(odd, 300000, order_tracks, 200, NULL);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)lib % 4);
    TEST_ASSERT_TRUE((const uint8_t *)lib >= odd);
    TEST_ASSERT_TRUE((const uint8_t *)lib < odd + 4);
    assert_well_formed(lib);
    TEST_ASSERT_EQUAL_size_t(n, save_image(lib, img_b, sizeof(img_b)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, n));

    const library_t *loaded = NULL;
    TEST_ASSERT_EQUAL(LIBRARY_OK, load_image(img_a, n, odd, n + 4, &loaded));
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)loaded % 4);
    assert_well_formed(loaded);
    // The bytes lost to lining up count against the arena.
    TEST_ASSERT_EQUAL(LIBRARY_FULL, load_image(img_a, n, odd, n, &loaded));
  }
}

// A small arena between two guards.
#define SMALL_MAX 24000u
#define GUARD_WORDS 16u
static uint32_t small_words[GUARD_WORDS + SMALL_MAX / 4 + GUARD_WORDS];

static void guards_set(void) { memset(small_words, 0xC3, sizeof(small_words)); }

static void assert_guards_intact(size_t arena_bytes) {
  const uint8_t *bytes = (const uint8_t *)small_words;
  for (size_t i = 0; i < GUARD_WORDS * 4; i++) {
    TEST_ASSERT_EQUAL_HEX8(0xC3, bytes[i]);
    TEST_ASSERT_EQUAL_HEX8(0xC3, bytes[GUARD_WORDS * 4 + arena_bytes + i]);
  }
}

void test_full_arena_keeps_what_was_added(void) {
  uint32_t most = 0;
  uint32_t sizes_tried = 0;
  // Many sizes (the step of 3 visits every remainder of 4), so that the last
  // track lands on every side of every limit.
  for (size_t bytes = 64; bytes <= SMALL_MAX;
       bytes += (bytes < 3000) ? 3 : 97) {
    void *arena = small_words + GUARD_WORDS;
    guards_set();
    library_builder_t *b = library_builder_begin(arena, bytes);
    if (b == NULL) {
      assert_guards_intact(bytes);
      continue;
    }
    sizes_tried++;
    uint32_t added = gen_fill(b, 100000);
    // Full stays full: the same track again, and a tiny one.
    gen_track(added, &gen);
    TEST_ASSERT_EQUAL(LIBRARY_FULL, library_builder_add(b, &gen.in));
    const library_t *lib = library_builder_finish(b);
    TEST_ASSERT_NOT_NULL(lib);
    assert_guards_intact(bytes);
    TEST_ASSERT_EQUAL_UINT32(added, library_track_count(lib));
    TEST_ASSERT_TRUE(library_bytes(lib) <= bytes);
    for (uint32_t i = 0; i < added; i++) {
      assert_holds_generated(lib, i);
    }
    // The track that did not fit left nothing behind.
    gen_track(added, &gen);
    TEST_ASSERT_FALSE(library_find_path(lib, gen.path, NULL));
    uint32_t albums, artists;
    gen_counts(added, &albums, &artists);
    TEST_ASSERT_EQUAL_UINT32(albums, library_album_count(lib));
    TEST_ASSERT_EQUAL_UINT32(artists, library_artist_count(lib));
    assert_well_formed(lib);

    // It is the same index a roomy arena makes of the same tracks.
    size_t n = save_image(lib, img_a, sizeof(img_a));
    library_builder_t *roomy = library_builder_begin(ARENA_B, 8 * SMALL_MAX);
    TEST_ASSERT_NOT_NULL(roomy);
    TEST_ASSERT_EQUAL_UINT32(added, gen_fill(roomy, added));
    const library_t *big = library_builder_finish(roomy);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_EQUAL_size_t(n, save_image(big, img_b, sizeof(img_b)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(img_a, img_b, n));
    if (added > most) {
      most = added;
    }
  }
  TEST_ASSERT_TRUE(sizes_tried > 1000);
  TEST_ASSERT_TRUE(most > 80);
}

// --- Scale -----------------------------------------------------------------

typedef struct {
  uint32_t added;
  uint32_t index_bytes;
  uint32_t peak_bytes;
  double build_ms;
} scale_t;

// The arena bytes a build wrote to: all of it but the longest stretch still
// holding the filler.
static uint32_t bytes_touched(const uint8_t *arena, size_t bytes) {
  size_t longest = 0;
  size_t run = 0;
  for (size_t i = 0; i < bytes; i++) {
    if (arena[i] == 0xA5) {
      run++;
      if (run > longest) {
        longest = run;
      }
    } else {
      run = 0;
    }
  }
  return (uint32_t)(bytes - longest);
}

static const library_t *build_generated(void *arena, size_t bytes,
                                        uint32_t tracks, scale_t *scale) {
  gen_init();
  memset(arena, 0xA5, bytes);
  double add_ms = 0;
  clock_t t0 = clock();
  library_builder_t *b = library_builder_begin(arena, bytes);
  add_ms += (double)(clock() - t0);
  TEST_ASSERT_NOT_NULL(b);
  uint32_t added = 0;
  for (; added < tracks; added++) {
    // Scattered order, the way a card that has been written to for years
    // lists its folders. Making the track is not timed.
    gen_track(added, &gen);
    t0 = clock();
    library_result_t r = library_builder_add(b, &gen.in);
    add_ms += (double)(clock() - t0);
    if (r == LIBRARY_FULL) {
      break;
    }
    TEST_ASSERT_EQUAL(LIBRARY_OK, r);
  }
  t0 = clock();
  const library_t *lib = library_builder_finish(b);
  double finish_ms = (double)(clock() - t0);
  TEST_ASSERT_NOT_NULL(lib);
  scale->added = added;
  scale->index_bytes = library_bytes(lib);
  scale->peak_bytes = bytes_touched(arena, bytes);
  scale->build_ms = (add_ms + finish_ms) * 1000.0 / CLOCKS_PER_SEC;
  uint32_t albums, artists;
  gen_counts(added, &albums, &artists);
  TEST_ASSERT_EQUAL_UINT32(added, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(albums, library_album_count(lib));
  TEST_ASSERT_EQUAL_UINT32(artists, library_artist_count(lib));
  printf("library scale: asked %u, added %u tracks (%u albums, %u artists): "
         "index %u bytes = %u.%u bytes/track, checksum %08x; arena touched "
         "during build %u of %u bytes = %u bytes/track; build %u.%u ms (adds "
         "%u.%u ms)\n",
         (unsigned)tracks, (unsigned)added, (unsigned)albums, (unsigned)artists,
         (unsigned)scale->index_bytes, (unsigned)(scale->index_bytes / added),
         (unsigned)(scale->index_bytes * 10u / added % 10u),
         (unsigned)rd32((const uint8_t *)lib + H_CRC),
         (unsigned)scale->peak_bytes, (unsigned)bytes,
         (unsigned)(scale->peak_bytes / added), (unsigned)scale->build_ms,
         (unsigned)((long)(scale->build_ms * 10) % 10),
         (unsigned)(add_ms * 1000.0 / CLOCKS_PER_SEC),
         (unsigned)((long)(add_ms * 10000.0 / CLOCKS_PER_SEC) % 10));
  return lib;
}

// The smallest arena, to the nearest kilobyte, that takes the first `tracks`
// generated tracks. (The working tables are sized from the arena, so this
// is less than a build in a large arena touches when the library is small.)
static void print_smallest_arena(uint32_t tracks) {
  size_t too_small = 0;
  size_t enough = ARENA_BYTES;
  while (enough - too_small > 1024) {
    size_t bytes = too_small + (enough - too_small) / 2;
    library_builder_t *b = library_builder_begin(ARENA_B, bytes);
    if (b != NULL && gen_fill(b, tracks) == tracks) {
      enough = bytes;
    } else {
      too_small = bytes;
    }
  }
  printf("library scale: the smallest arena that builds %u tracks is %u "
         "bytes = %u bytes/track\n",
         (unsigned)tracks, (unsigned)enough, (unsigned)(enough / tracks));
}

void test_five_thousand_tracks_fit_and_answer(void) {
  scale_t scale;
  const library_t *lib = build_generated(ARENA_A, ARENA_BYTES, 5000, &scale);
  TEST_ASSERT_EQUAL_UINT32(5000, scale.added);
  // Room to spare: the finished index in well under half the arena, and the
  // build itself in under three quarters.
  TEST_ASSERT_TRUE(scale.index_bytes < ARENA_BYTES * 2 / 5);
  TEST_ASSERT_TRUE(scale.peak_bytes < ARENA_BYTES * 3 / 4);

  assert_well_formed(lib);
  for (uint32_t i = 0; i < 5000; i++) {
    assert_holds_generated(lib, i);
  }

  // An album's tracks are the generator's, in order.
  gen_track(2500, &gen);
  uint32_t t = track_at(lib, gen.path);
  uint32_t album = track_info(lib, t).album;
  uint32_t gen_album = gen_album_of(2500);
  uint32_t first, count;
  library_album_tracks(lib, album, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(gen_album_size(gen_album), count);
  for (uint32_t k = 0; k < count; k++) {
    gen_track(gen_album_first[gen_album] + k, &gen);
    TEST_ASSERT_EQUAL_STRING(gen.title, track_info(lib, first + k).title);
  }
  // A double album runs disc 1 then disc 2.
  gen_track(gen_album_first[9], &gen);
  album = track_info(lib, track_at(lib, gen.path)).album;
  library_album_tracks(lib, album, &first, &count);
  TEST_ASSERT_EQUAL_UINT32(20, count);
  TEST_ASSERT_EQUAL_UINT16(1, track_info(lib, first).disc_no);
  TEST_ASSERT_EQUAL_UINT16(1, track_info(lib, first + 9).disc_no);
  TEST_ASSERT_EQUAL_UINT16(10, track_info(lib, first + 9).track_no);
  TEST_ASSERT_EQUAL_UINT16(2, track_info(lib, first + 10).disc_no);
  TEST_ASSERT_EQUAL_UINT16(1, track_info(lib, first + 10).track_no);
  // The compilations are all under one artist.
  uint32_t various = artist_named(lib, "Various Artists");
  library_artist_albums(lib, various, &first, &count);
  uint32_t compilations = 0;
  for (uint32_t al = 0; gen_album_first[al] < 5000; al++) {
    compilations += gen_is_compilation(al) ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_UINT32(compilations, count);
  TEST_ASSERT_FALSE(library_find_path(lib, "Music/nothing here.flac", NULL));

  // It saves, and loads into another arena as the same library.
  size_t n = save_image(lib, img_a, sizeof(img_a));
  const library_t *loaded = NULL;
  clock_t t0 = clock();
  TEST_ASSERT_EQUAL(LIBRARY_OK,
                    load_image(img_a, n, ARENA_B, ARENA_BYTES, &loaded));
  printf("library scale: load of 5000 tracks (%u bytes, read, checksum and "
         "checks) %u us\n",
         (unsigned)n,
         (unsigned)((double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC));
  assert_same_answers(lib, loaded);
  print_smallest_arena(5000);
}

void test_one_thousand_tracks(void) {
  scale_t scale;
  const library_t *lib = build_generated(ARENA_A, ARENA_BYTES, 1000, &scale);
  TEST_ASSERT_EQUAL_UINT32(1000, scale.added);
  assert_well_formed(lib);
  print_smallest_arena(1000);
}

// Ten thousand do not fit in 1.5 MB: the build stops cleanly where it is
// full and everything before that is there.
void test_ten_thousand_tracks_fill_the_arena_cleanly(void) {
  scale_t scale;
  const library_t *lib = build_generated(ARENA_A, ARENA_BYTES, 10000, &scale);
  TEST_ASSERT_TRUE(scale.added > 5000);
  TEST_ASSERT_TRUE(scale.added < 10000);
  TEST_ASSERT_TRUE(scale.peak_bytes <= ARENA_BYTES);
  assert_well_formed(lib);
  for (uint32_t i = 0; i < scale.added; i++) {
    assert_holds_generated(lib, i);
  }
}

// Track numbers are 16 bits in the file, so the builder stops at 65535
// however large the arena is.
#define BIG_BYTES (12u * 1024u * 1024u)
static uint32_t big_words[BIG_BYTES / 4];

void test_track_count_is_capped_at_65535(void) {
  library_builder_t *b = library_builder_begin(big_words, BIG_BYTES);
  TEST_ASSERT_NOT_NULL(b);
  char path[32];
  library_track_in_t in = trk(path, "t", "a", "b", "a", 0, 0);
  for (uint32_t i = 0; i < 65535; i++) {
    snprintf(path, sizeof(path), "d%u/%05u", (unsigned)(i % 300), (unsigned)i);
    in.track_no = (uint16_t)(65535 - i);
    TEST_ASSERT_EQUAL(LIBRARY_OK, library_builder_add(b, &in));
  }
  snprintf(path, sizeof(path), "one too many");
  TEST_ASSERT_EQUAL(LIBRARY_FULL, library_builder_add(b, &in));
  const library_t *lib = library_builder_finish(b);
  TEST_ASSERT_NOT_NULL(lib);
  TEST_ASSERT_EQUAL_UINT32(65535, library_track_count(lib));
  TEST_ASSERT_EQUAL_UINT32(1, library_album_count(lib));
  // Sorted by track number: the last added is first.
  TEST_ASSERT_EQUAL_UINT16(1, track_info(lib, 0).track_no);
  TEST_ASSERT_EQUAL_UINT16(65535, track_info(lib, 65534).track_no);
  TEST_ASSERT_EQUAL_UINT32(65534, track_at(lib, "d0/00000"));
  TEST_ASSERT_EQUAL_UINT32(0, track_at(lib, "d134/65534"));
  TEST_ASSERT_FALSE(library_find_path(lib, "one too many", NULL));
  assert_well_formed(lib);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_library);
  RUN_TEST(test_one_track);
  RUN_TEST(test_artists_are_sorted_ignoring_case);
  RUN_TEST(test_artist_names_differing_in_case_or_spaces_are_one_artist);
  RUN_TEST(test_first_spelling_seen_is_kept);
  RUN_TEST(test_albums_group_under_their_artist);
  RUN_TEST(test_same_album_name_under_two_artists_is_two_albums);
  RUN_TEST(test_compilation_is_one_album_under_its_group_artist);
  RUN_TEST(test_album_tracks_are_ordered_by_disc_then_track_then_file_name);
  RUN_TEST(test_tracks_without_numbers_fall_back_to_file_name_order);
  RUN_TEST(test_artist_tracks_are_a_contiguous_range);
  RUN_TEST(test_albums_by_name_and_songs_by_title_are_sorted);
  RUN_TEST(test_names_differing_only_in_case_are_in_byte_order);
  RUN_TEST(test_track_path_round_trips);
  RUN_TEST(test_track_path_reports_needed_length_when_the_buffer_is_small);
  RUN_TEST(test_find_path_finds_every_track_and_rejects_others);
  RUN_TEST(test_the_same_path_twice_is_two_tracks);
  RUN_TEST(test_out_of_range_numbers_are_safe);
  RUN_TEST(test_order_of_adding_does_not_change_the_result);
  RUN_TEST(test_order_of_adding_does_not_matter_when_tracks_are_nearly_equal);
  RUN_TEST(test_order_of_adding_changes_only_the_spelling_kept);
  RUN_TEST(test_kept_names_are_trimmed);
  RUN_TEST(test_save_and_load_round_trip);
  RUN_TEST(test_checksum_is_the_standard_crc32);
  RUN_TEST(test_load_rejects_bad_magic);
  RUN_TEST(test_load_rejects_wrong_version);
  RUN_TEST(test_load_rejects_a_truncated_file);
  RUN_TEST(test_load_rejects_a_file_with_something_after_the_index);
  RUN_TEST(test_load_rejects_a_flipped_bit_anywhere);
  RUN_TEST(test_load_rejects_an_index_larger_than_the_arena);
  RUN_TEST(test_load_rejects_inconsistent_offsets_with_a_valid_checksum);
  RUN_TEST(test_damaged_files_with_good_checksums_never_read_outside_the_arena);
  RUN_TEST(test_a_scrambled_but_in_range_file_is_safe_to_query);
  RUN_TEST(test_io_failures_are_reported);
  RUN_TEST(test_null_arguments_and_finished_builders_are_refused);
  RUN_TEST(test_begin_refuses_an_arena_too_small_to_start);
  RUN_TEST(test_the_arena_need_not_be_aligned);
  RUN_TEST(test_full_arena_keeps_what_was_added);
  RUN_TEST(test_five_thousand_tracks_fit_and_answer);
  RUN_TEST(test_one_thousand_tracks);
  RUN_TEST(test_ten_thousand_tracks_fill_the_arena_cleanly);
  RUN_TEST(test_track_count_is_capped_at_65535);
  return UNITY_END();
}
