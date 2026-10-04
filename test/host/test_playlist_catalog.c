#include "menu.h"
#include "playlist_catalog.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static playlist_catalog_t cat;

void setUp(void) {
  memset(&cat, 0xA5, sizeof(cat)); // clear must not rely on zeros
  playlist_catalog_clear(&cat);
}

void tearDown(void) {}

// --- Helpers -----------------------------------------------------------------

static const char *name_of(uint32_t i) {
  static char text[300];
  memset(text, '?', sizeof(text));
  playlist_catalog_name(&cat, i, text, sizeof(text));
  TEST_ASSERT_NOT_NULL(memchr(text, '\0', sizeof(text)));
  return text;
}

static void add_all(const char *const *files, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(playlist_catalog_add(&cat, files[i]), files[i]);
  }
}

// The catalog lists exactly these file names, in this order.
static void assert_files(const char *const *files, uint32_t n) {
  TEST_ASSERT_EQUAL_UINT32(n, cat.count);
  for (uint32_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL_STRING(files[i], playlist_catalog_file(&cat, i));
  }
  TEST_ASSERT_EQUAL_STRING("", playlist_catalog_file(&cat, n));
  TEST_ASSERT_EQUAL_STRING("", playlist_catalog_file(&cat, UINT32_MAX));
}

// The order the library uses for text (library.c, ORDER), applied to names:
// A-Z folded first, then the bytes as they are.
static int names_cmp(const char *a, const char *b) {
  int as_written = 0;
  for (size_t i = 0;; i++) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
    if (ca != cb) {
      unsigned char fa = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
      unsigned char fb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
      if (fa != fb) {
        return fa < fb ? -1 : 1;
      }
      if (as_written == 0) {
        as_written = ca < cb ? -1 : 1;
      }
    } else if (ca == 0) {
      return as_written;
    }
  }
}

static void assert_sorted(void) {
  static char prev[300];
  for (uint32_t i = 0; i < cat.count; i++) {
    const char *now = name_of(i);
    if (i > 0) {
      TEST_ASSERT_TRUE_MESSAGE(names_cmp(prev, now) <= 0, now);
      // The letter groups of a sorted list never go back (menu_jump_group's
      // requirement).
      TEST_ASSERT_TRUE((unsigned char)menu_group(prev) <=
                       (unsigned char)menu_group(now));
    }
    snprintf(prev, sizeof(prev), "%s", now);
  }
}

// --- The catalog -------------------------------------------------------------

void test_add_only_playlist_files(void) {
  static const char *const YES[] = {"Rock.m3u", "chill.M3U8", "x.M3u",
                                    "a.b.m3u8", "Night Drive.m3U"};
  static const char *const NO[] = {
      "song.mp3",   ".hidden.m3u", ".m3u",       "Rock.m3u.tmp",
      "x.m3u8.tmp", "m3u",         "Rockm3u",    "a.m3u8x",
      "a.m3",       "README",      "",           "list.m3u ",
      "list.pls",   ".m3u8",       "notes.m3u~", "x.m3u8.TMP"};
  add_all(YES, 5);
  for (size_t i = 0; i < sizeof(NO) / sizeof(NO[0]); i++) {
    TEST_ASSERT_FALSE_MESSAGE(playlist_catalog_add(&cat, NO[i]), NO[i]);
  }
  TEST_ASSERT_FALSE(playlist_catalog_add(&cat, NULL));
  TEST_ASSERT_FALSE(playlist_catalog_add(NULL, "Rock.m3u"));
  TEST_ASSERT_FALSE(cat.truncated); // refusing a file is not running out
  // Added in the order found until sorted.
  assert_files(YES, 5);
  // The pool holds the names and nothing else.
  size_t used = 0;
  for (size_t i = 0; i < 5; i++) {
    used += strlen(YES[i]) + 1;
  }
  TEST_ASSERT_EQUAL_UINT32(used, cat.pool_used);

  // Clear empties it.
  playlist_catalog_clear(&cat);
  TEST_ASSERT_EQUAL_UINT32(0, cat.count);
  TEST_ASSERT_EQUAL_UINT32(0, cat.pool_used);
  TEST_ASSERT_FALSE(cat.truncated);
  assert_files(NULL, 0);
  playlist_catalog_clear(NULL);
  TEST_ASSERT_EQUAL_STRING("", playlist_catalog_file(NULL, 0));
}

void test_sorted_ignoring_case(void) {
  static const char *const FOUND[] = {
      "beta.m3u", "Alpha.m3u8", "ALPHA2.m3u", "alpha b.m3u",
      "1999.m3u", "zed.m3u",    "Beta.m3u",   "\xC3\x84rger.m3u"};
  // By name, not by file name: "Alpha.m3u8" before "alpha b.m3u" although
  // '.' comes after ' '. "Beta" before "beta": equal but for case, then by
  // the bytes. A name in UTF-8 last, as in the library.
  static const char *const SORTED[] = {
      "1999.m3u", "Alpha.m3u8", "alpha b.m3u", "ALPHA2.m3u",
      "Beta.m3u", "beta.m3u",   "zed.m3u",     "\xC3\x84rger.m3u"};
  add_all(FOUND, 8);
  playlist_catalog_sort(&cat);
  assert_files(SORTED, 8);
  assert_sorted();
  char groups[9];
  for (uint32_t i = 0; i < 8; i++) {
    groups[i] = menu_group(name_of(i));
  }
  groups[8] = '\0';
  TEST_ASSERT_EQUAL_STRING("#AAABBZ~", groups);

  // Sorting again changes nothing; an empty catalog and one entry sort too.
  playlist_catalog_sort(&cat);
  assert_files(SORTED, 8);
  playlist_catalog_clear(&cat);
  playlist_catalog_sort(&cat);
  assert_files(NULL, 0);
  TEST_ASSERT_TRUE(playlist_catalog_add(&cat, "One.m3u"));
  playlist_catalog_sort(&cat);
  TEST_ASSERT_EQUAL_STRING("One.m3u", playlist_catalog_file(&cat, 0));
  playlist_catalog_sort(NULL);

  // A full catalog in a scrambled order, with ties in all but case.
  playlist_catalog_clear(&cat);
  char file[32];
  for (uint32_t i = 0; i < PLAYLIST_CATALOG_MAX; i++) {
    uint32_t k = (i * 7919u) % PLAYLIST_CATALOG_MAX;
    snprintf(file, sizeof(file), "%c%c list %u.m3u%s",
             (k % 3 == 0) ? 'A' + (char)(k % 26) : 'a' + (char)(k % 26),
             'a' + (char)(k / 26), (unsigned)(k % 5), (k & 1) ? "8" : "");
    TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  }
  playlist_catalog_sort(&cat);
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_CATALOG_MAX, cat.count);
  assert_sorted();
}

void test_name_drops_the_ending(void) {
  static const char *const FILES[] = {"Rock.m3u", "chill.M3U8", "a.b.m3u",
                                      "x.m3u.m3u8"};
  add_all(FILES, 4);
  TEST_ASSERT_EQUAL_STRING("Rock", name_of(0));
  TEST_ASSERT_EQUAL_STRING("chill", name_of(1));
  TEST_ASSERT_EQUAL_STRING("a.b", name_of(2));
  TEST_ASSERT_EQUAL_STRING("x.m3u", name_of(3));
  TEST_ASSERT_EQUAL_STRING("", name_of(4));
  TEST_ASSERT_EQUAL_STRING("", name_of(UINT32_MAX));

  // Cut to the buffer, always terminated; no buffer, nothing written.
  char out[8];
  memset(out, '!', sizeof(out));
  playlist_catalog_name(&cat, 0, out, 3);
  TEST_ASSERT_EQUAL_STRING("Ro", out);
  TEST_ASSERT_EQUAL_CHAR('!', out[3]);
  memset(out, '!', sizeof(out));
  playlist_catalog_name(&cat, 0, out, 5);
  TEST_ASSERT_EQUAL_STRING("Rock", out);
  TEST_ASSERT_EQUAL_CHAR('!', out[5]);
  memset(out, '!', sizeof(out));
  playlist_catalog_name(&cat, 0, out, 0);
  TEST_ASSERT_EQUAL_CHAR('!', out[0]);
  playlist_catalog_name(&cat, 0, NULL, 5);
  memset(out, '!', sizeof(out));
  playlist_catalog_name(NULL, 0, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_find_ignores_case_and_ending(void) {
  static const char *const FILES[] = {"rock.m3u8", "Jazz.m3u", "Jazz Club.M3U",
                                      "ROCK.m3u.m3u", "1999.m3u"};
  add_all(FILES, 5);
  playlist_catalog_sort(&cat);
  // 1999, Jazz, Jazz Club, rock, ROCK.m3u
  uint32_t i = 99;
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "ROCK", &i));
  TEST_ASSERT_EQUAL_STRING("rock.m3u8", playlist_catalog_file(&cat, i));
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "jazz", &i));
  TEST_ASSERT_EQUAL_STRING("Jazz.m3u", playlist_catalog_file(&cat, i));
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "JAZZ CLUB", &i));
  TEST_ASSERT_EQUAL_STRING("Jazz Club.M3U", playlist_catalog_file(&cat, i));
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "1999", &i));
  TEST_ASSERT_EQUAL_UINT32(0, i);
  // A typed name is a whole name: "ROCK.M3U" is the playlist of that name,
  // not "rock" with an ending.
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "rock.M3U", &i));
  TEST_ASSERT_EQUAL_STRING("ROCK.m3u.m3u", playlist_catalog_file(&cat, i));

  i = 99;
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "ROC", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "ROCKS", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "rock.m3u8", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "Jazz ", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "ZZZ", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "0", &i));
  TEST_ASSERT_EQUAL_UINT32(99, i); // not written when not found
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, NULL, &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(NULL, "ROCK", &i));
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "ROCK", NULL));

  // Every one of a full catalog is found, at its own place.
  playlist_catalog_clear(&cat);
  char file[32], typed[32];
  for (uint32_t k = 0; k < PLAYLIST_CATALOG_MAX; k++) {
    snprintf(file, sizeof(file), "list %03u.m3u", (unsigned)((k * 37) % 200));
    TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  }
  playlist_catalog_sort(&cat);
  for (uint32_t k = 0; k < PLAYLIST_CATALOG_MAX; k++) {
    snprintf(typed, sizeof(typed), "LIST %03u", (unsigned)k);
    TEST_ASSERT_TRUE(playlist_catalog_find(&cat, typed, &i));
    TEST_ASSERT_EQUAL_UINT32(k, i);
  }
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "LIST 200", &i));
  TEST_ASSERT_FALSE(playlist_catalog_find(&cat, "LIST", &i));
}

void test_full_catalog_sets_truncated(void) {
  // By count.
  char file[300];
  for (uint32_t k = 0; k < PLAYLIST_CATALOG_MAX; k++) {
    snprintf(file, sizeof(file), "%u.m3u", (unsigned)k);
    TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  }
  TEST_ASSERT_FALSE(cat.truncated);
  TEST_ASSERT_FALSE(playlist_catalog_add(&cat, "one more.m3u"));
  TEST_ASSERT_TRUE(cat.truncated);
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_CATALOG_MAX, cat.count);
  // A file that is not a playlist is still just not a playlist.
  TEST_ASSERT_FALSE(playlist_catalog_add(&cat, "song.mp3"));

  // By bytes: names of 255 characters, 256 bytes each with the terminator.
  playlist_catalog_clear(&cat);
  memset(file, 'A', 251);
  memcpy(file + 251, ".m3u", 5);
  uint32_t fit = PLAYLIST_CATALOG_POOL / 256; // 64
  for (uint32_t k = 0; k < fit; k++) {
    file[0] = (char)('A' + k % 26);
    file[1] = (char)('A' + k / 26);
    TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  }
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_CATALOG_POOL, cat.pool_used);
  TEST_ASSERT_FALSE(cat.truncated);
  TEST_ASSERT_FALSE(playlist_catalog_add(&cat, "x.m3u"));
  TEST_ASSERT_TRUE(cat.truncated);
  TEST_ASSERT_EQUAL_UINT32(fit, cat.count);
  playlist_catalog_sort(&cat);
  assert_sorted();

  // A name that fills the pool to the last byte fits; one byte more does not.
  playlist_catalog_clear(&cat);
  memset(file, 'B', sizeof(file));
  for (uint32_t k = 0; k + 1 < fit; k++) {
    memcpy(file + 251, ".m3u", 5);
    TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  }
  memset(file, 'C', sizeof(file));
  memcpy(file + 252, ".m3u", 5); // 257 bytes
  TEST_ASSERT_FALSE(playlist_catalog_add(&cat, file));
  TEST_ASSERT_TRUE(cat.truncated);
  memcpy(file + 251, ".m3u", 5); // 256 bytes: exactly what is left
  TEST_ASSERT_TRUE(playlist_catalog_add(&cat, file));
  TEST_ASSERT_EQUAL_UINT32(PLAYLIST_CATALOG_POOL, cat.pool_used);
  TEST_ASSERT_EQUAL_STRING(file, playlist_catalog_file(&cat, fit - 1));
}

// --- Names -------------------------------------------------------------------

static void assert_check(const char *typed, playlist_name_t want,
                         const char *use) {
  char out[PLAYLIST_NAME_MAX + 1 + 8];
  memset(out, '?', sizeof(out));
  TEST_ASSERT_EQUAL_INT_MESSAGE(want, playlist_name_check(typed, out), typed);
  // Never more than the size the header promises.
  TEST_ASSERT_NOT_NULL(memchr(out, '\0', PLAYLIST_NAME_MAX + 1));
  TEST_ASSERT_EQUAL_CHAR('?', out[PLAYLIST_NAME_MAX + 1]);
  if (use != NULL) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(use, out, typed);
  }
}

void test_name_check_each_rule(void) {
  // Every character allowed.
  assert_check("ROCK", PLAYLIST_NAME_OK, "ROCK");
  assert_check("AZ 09-'&.X", PLAYLIST_NAME_OK, "AZ 09-'&.X");
  assert_check("A", PLAYLIST_NAME_OK, "A");
  assert_check("1999", PLAYLIST_NAME_OK, "1999");
  assert_check("ABCDEFGHIJKLMNOP", PLAYLIST_NAME_OK, "ABCDEFGHIJKLMNOP");

  assert_check("", PLAYLIST_NAME_EMPTY, "");
  assert_check("    ", PLAYLIST_NAME_EMPTY, "");
  assert_check(NULL, PLAYLIST_NAME_EMPTY, "");

  assert_check("ABCDEFGHIJKLMNOPQ", PLAYLIST_NAME_TOO_LONG, "");
  assert_check("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", PLAYLIST_NAME_TOO_LONG,
               "");

  assert_check("rock", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("ROCk", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("A/B", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("A_B", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("A\\B", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("A:B", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("A\tB", PLAYLIST_NAME_BAD_CHAR, NULL);
  assert_check("\xC3\x84", PLAYLIST_NAME_BAD_CHAR, NULL);

  assert_check("ROCK.", PLAYLIST_NAME_BAD_END, NULL);
  assert_check(".", PLAYLIST_NAME_BAD_END, NULL);
  assert_check("ROCK. ", PLAYLIST_NAME_BAD_END, NULL); // after trimming
  // A file name starting with '.' is hidden and never listed, so a name may
  // not start with one. Trimming removes only spaces, so ". A" starts with
  // '.'; "  .A" is ".A" once trimmed.
  assert_check(".ROCK", PLAYLIST_NAME_BAD_START, NULL);
  assert_check("...A", PLAYLIST_NAME_BAD_START, NULL);
  assert_check(". A", PLAYLIST_NAME_BAD_START, NULL);
  assert_check("  .A", PLAYLIST_NAME_BAD_START, NULL);
  assert_check("A.B", PLAYLIST_NAME_OK, "A.B");
  // Both ends: the end is checked first (the order of playlist_name_t).
  assert_check(".A.", PLAYLIST_NAME_BAD_END, NULL);

  // Where more than one rule is broken, the first in the list above wins.
  assert_check("abcdefghijklmnopq", PLAYLIST_NAME_TOO_LONG, NULL);
  assert_check("rock.", PLAYLIST_NAME_BAD_CHAR, NULL);
}

void test_name_check_trims_spaces(void) {
  assert_check("  ROCK  ", PLAYLIST_NAME_OK, "ROCK");
  assert_check(" A B ", PLAYLIST_NAME_OK, "A B");
  assert_check("A  B", PLAYLIST_NAME_OK, "A  B");
  // 16 after trimming fits however many spaces there were.
  assert_check("   ABCDEFGHIJKLMNOP   ", PLAYLIST_NAME_OK, "ABCDEFGHIJKLMNOP");
  assert_check(" ABCDEFGHIJKLMNOPQ ", PLAYLIST_NAME_TOO_LONG, NULL);
  // Only spaces are trimmed.
  assert_check("\tROCK", PLAYLIST_NAME_BAD_CHAR, NULL);
}

// Every name the device accepts makes a file the catalog lists and finds:
// a name that could be typed but never listed would vanish, and "Name in
// use" could not catch a second one.
void test_every_accepted_name_is_listed(void) {
  static const char *const TYPED[] = {
      "A",
      "0",
      "&",
      "'",
      "-",
      " Z ",
      "ABCDEFGHIJKLMNOP",
      "0123456789-'& .X",
      "A.B",
      "A..B",
      "A. .B",
      "M3U",
      "X.M3U",
      "X.M3U8",
      "ROCK & ROLL",
      "-'&",
      "   ABCDEFGHIJKLMNOP   ",
      "A          .   B",
  };
  enum { N = sizeof(TYPED) / sizeof(TYPED[0]) };
  static char names[N][PLAYLIST_NAME_MAX + 1];
  char file[PLAYLIST_NAME_MAX + 8];
  for (uint32_t k = 0; k < N; k++) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        PLAYLIST_NAME_OK, playlist_name_check(TYPED[k], names[k]), TYPED[k]);
    playlist_file_for(names[k], file, sizeof(file));
    TEST_ASSERT_TRUE_MESSAGE(playlist_catalog_add(&cat, file), file);
  }
  // Every allowed character, one name each, inside a name.
  static const char ALLOWED[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -'&.";
  static char single[sizeof(ALLOWED)][PLAYLIST_NAME_MAX + 1];
  uint32_t singles = 0;
  for (size_t k = 0; ALLOWED[k] != '\0'; k++) {
    char typed[4] = {'A', ALLOWED[k], 'Z', '\0'};
    TEST_ASSERT_EQUAL_INT(PLAYLIST_NAME_OK,
                          playlist_name_check(typed, single[singles]));
    playlist_file_for(single[singles], file, sizeof(file));
    TEST_ASSERT_TRUE_MESSAGE(playlist_catalog_add(&cat, file), file);
    singles++;
  }
  playlist_catalog_sort(&cat);
  uint32_t i;
  for (uint32_t k = 0; k < N; k++) {
    TEST_ASSERT_TRUE_MESSAGE(playlist_catalog_find(&cat, names[k], &i),
                             names[k]);
    char shown[PLAYLIST_NAME_MAX + 8];
    playlist_catalog_name(&cat, i, shown, sizeof(shown));
    TEST_ASSERT_EQUAL_STRING(names[k], shown);
  }
  for (uint32_t k = 0; k < singles; k++) {
    TEST_ASSERT_TRUE_MESSAGE(playlist_catalog_find(&cat, single[k], &i),
                             single[k]);
  }
}

void test_file_for_a_name(void) {
  char out[32];
  memset(out, '!', sizeof(out));
  playlist_file_for("ROCK", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ROCK.m3u", out);
  playlist_file_for("ABCDEFGHIJKLMNOP", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP.m3u", out);
  playlist_file_for("A.B", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A.B.m3u", out);

  // What a new playlist is called is what the catalog lists and finds.
  playlist_file_for("JAZZ CLUB", out, sizeof(out));
  TEST_ASSERT_TRUE(playlist_catalog_add(&cat, out));
  TEST_ASSERT_EQUAL_STRING("JAZZ CLUB", name_of(0));
  uint32_t i;
  TEST_ASSERT_TRUE(playlist_catalog_find(&cat, "JAZZ CLUB", &i));

  // Cut to the buffer, always terminated.
  memset(out, '!', sizeof(out));
  playlist_file_for("ROCK", out, 7);
  TEST_ASSERT_EQUAL_STRING("ROCK.m", out);
  TEST_ASSERT_EQUAL_CHAR('!', out[7]);
  memset(out, '!', sizeof(out));
  playlist_file_for("ROCK", out, 0);
  TEST_ASSERT_EQUAL_CHAR('!', out[0]);
  playlist_file_for("ROCK", NULL, 8);
  // No name: no file.
  playlist_file_for(NULL, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_add_only_playlist_files);
  RUN_TEST(test_sorted_ignoring_case);
  RUN_TEST(test_name_drops_the_ending);
  RUN_TEST(test_find_ignores_case_and_ending);
  RUN_TEST(test_full_catalog_sets_truncated);
  RUN_TEST(test_name_check_each_rule);
  RUN_TEST(test_name_check_trims_spaces);
  RUN_TEST(test_every_accepted_name_is_listed);
  RUN_TEST(test_file_for_a_name);
  return UNITY_END();
}
