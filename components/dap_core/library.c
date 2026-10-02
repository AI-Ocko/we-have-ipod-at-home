#include "library.h"

#include <string.h>

// Design notes
//
// The index is one block of bytes that is at once the file on the card and
// the thing the menus query. Loading it is one read and one pass of checks;
// nothing is unpacked, and nothing in it is a pointer.
//
// THE FILE (version 1; all integers little-endian; every table starts at a
// multiple of four from the start of the file)
//
//   header, 72 bytes, 18 fields of 32 bits:
//      0 magic "DAPL"      4 version            8 total bytes
//     12 CRC-32 of bytes [16, total)
//     16 tracks  20 albums  24 artists  28 folders  32 string pool bytes
//     36.. the offsets of the nine tables below, in this order
//   tracks       24 bytes each, in track order:
//                title, artist, file name (pool offsets, 32 bits each),
//                length in ms (32), album, folder, track number, disc
//                number (16 each)
//   albums       8 bytes each, in album order:
//                name (pool offset, 32), first track (16), artist (16)
//   artists      8 bytes each, A to Z:
//                name (pool offset, 32), first album (16), first track (16)
//   folders      8 bytes each, in string order:
//                path with its trailing '/' (pool offset, 32), first entry
//                in the path order (16), zero (16)
//   song order   track numbers (16 bits each), A to Z by title
//   path order   track numbers (16 bits each), by folder, then file name
//   album order  album numbers (16 bits each), A to Z by name
//   formats      one byte per track
//   string pool  every string once, NUL-terminated
//   Each of the last five is padded with zeros to a multiple of four.
//
// Tracks are numbered by artist, album, disc, track number, file name; so an
// album's tracks, an artist's albums and an artist's tracks are each a run
// of consecutive numbers, and a row stores only where its run starts: the
// run ends where the next row's starts (or at the count, for the last row).
// The artist a track is filed under is its album's artist and is not stored
// again.
//
// A path is split after its last '/'. The folder part ("Music/Abba/Gold/",
// or "" for a file at the top) is a string like any other, stored once and
// shared by every file in the folder; a track stores its folder's number
// and its own file name. The path order lists a folder's files together, so
// a folder row needs only where its files start.
//
// The tables are in a fixed order with nothing between them, so the nine
// offsets in the header say nothing the five counts do not; they are stored
// so that a query reads one field instead of adding up the tables before
// the one it wants, and the loader insists that they are exactly what the
// counts imply.
//
// The version must change if anything above does: a field, a width, the
// order of tables, the padding, what the checksum covers, or any of the
// orderings below (a file sorted by other rules would load and then give
// wrong answers from library_find_path, which searches by these rules).
//
// Limits that come from the format: 65535 tracks (numbers are 16 bits),
// and an index under 4 GB. Neither is in reach of the 1.5 MB arena.
//
// ORDER
//
// One order is used for all text: compare the two strings byte by byte with
// A-Z folded to a-z; if that finds no difference, compare the bytes as they
// are. So digits and most punctuation come before letters, "The Beatles" is
// under T, bytes above 127 (UTF-8 in paths) come last in byte order, and two
// strings are equal only if they are the same bytes. "Angel" comes before
// "angel".
//
//   artists          name
//   albums           artist, then name
//   tracks           album, disc number, track number, file name; then, so
//                    that two tracks tie only if they are the same in every
//                    respect: folder, title, artist, length, format
//   album order      name, then album number
//   song order       title, then track number
//   folders          the folder string
//   path order       folder, file name, then track number
//
// Two artists (or two albums of one artist) always differ in more than
// case, so which spelling of a name is kept never decides an order. For
// these names the sort key is the position of the name's case-folded class,
// not of the name itself, so that holds for the album order too.
//
// Nothing here, and nothing in GROUPING, depends on the order tracks were
// added in: the same tracks give the same bytes, so a rescan of the same
// card gives the same file however the card lists its folders.
//
// Because text that differs only in case is next to each other in this
// order, library_find_path can ignore case: folders that differ only in
// case are neighbouring rows, and so are such file names within a folder.
//
// GROUPING
//
// An artist is found by its name without leading and trailing spaces and
// ignoring case; an album by its artist and its name, likewise. The hash
// and the comparison fold and trim as they go, so no folded copy of any
// string is ever stored.
//
// Of the spellings of one name the one kept is the first in the text order
// above, trimmed: "ABBA" and "Abba" keep "ABBA" whichever came first. A
// track whose spelling comes before the one kept so far replaces it, there
// and then, with one comparison against the kept name (which the lookup
// has just read anyway); there is no pass over the tracks for it. The
// spelling replaced stays in the log, unused unless something else refers
// to it, and is not copied to the pool.
//
// Titles, track artists and paths are kept exactly as given.
//
// BUILDING
//
// While tracks are added the arena looks like this:
//
//   | builder | bucket tables |      free      | log, growing down |
//   0                        low            log_bot               cap
//
// The log holds everything about the tracks: each distinct string once (5
// bytes of bookkeeping, then the text), a 16-byte node per artist, a
// 20-byte node per album and a 36-byte record per track. Nothing in it
// moves until the build is over.
//
// Three chained hash tables find things in it: every string by its exact
// bytes (which is what stores a string once however many tracks use it),
// artists, and albums. Their bucket arrays are sized once from the arena
// (one string bucket per 32 to 64 bytes of arena, one artist and one album
// bucket per 256 to 512) and never grow. They need not: every string costs
// at least 22 bytes of arena (6 in the log, 16 of scratch when finishing),
// so even an arena full of one-letter strings has about three to a bucket,
// and a real one has fewer than one. And they cost nothing: the space they
// stand in is where the finished index will go, which nothing else could
// use meanwhile. The hash is FNV-1a with a final mix.
//
// Finishing writes the index at the bottom while the log stays where it is:
//
//   | tables | scratch, then the pool |   free   | log |
//   0                                             top   cap
//
//   1. Every string is put in order (16 bytes of scratch each: the first 12
//      letters, folded, as three integers, and where the string is; the
//      strings themselves are read only when 12 letters do not decide).
//      Each string's rank is written into its log entry. From here on every
//      comparison of text is a comparison of two integers.
//   2. Artists, then albums, are sorted by rank and numbered; then the
//      tracks (16 bytes of scratch each: album and disc, track number, rank
//      of the file name, where the record is), then the song order and the
//      path order. Each table row is written at its final place as soon as
//      it is known, holding log offsets where pool offsets will go.
//   3. The tables are walked once more in a fixed order, and each string is
//      copied to the pool the first time something refers to it: artist
//      names A to Z, album names, then for each track its artist and title,
//      the folders, and the file names folder by folder. So the pool's
//      order follows from the index's, not from the order of adding, and
//      what a menu shows next is usually in the cache line it just read.
//
// The sort is written here because qsort takes no context (the comparison
// would need a global to reach the arena) and glibc's allocates. It is a
// quicksort (median of three, insertion sort for short runs, an explicit
// stack) that hands a run to heapsort if it has split too many times, so it
// is O(n log n) whatever the input; quicksort's two scans are sequential,
// which is what the PSRAM cache wants.
//
// The log and the index never overlap, which is why each can be read while
// the other is written, and why the builder can say exactly when it is
// full. After every track it works out the finished index's size from its
// counts (tables T, pool P) and what finishing would need. (P is counted
// as strings are first referred to and never goes down, so a replaced
// spelling leaves it a few bytes too high: an upper bound, which is all the
// conditions need. The pool's real size is known when it has been written.)
//
//     max(low, T) + scratch <= top   and   T + P <= top
//     scratch = 16 * max(strings + albums, tracks)
//
// where top is the bottom of the log. If either fails the track is taken
// out again (the few words it changed outside the log are put back from an
// undo list) and LIBRARY_FULL is returned; what was there before is exactly
// as it was and can be finished. So a build of n tracks needs
//
//     log(n) + max(index(n), max(low, tables(n)) + scratch(n))
//
// bytes, and touches no others. For a real library (see the 5,000-track
// test) that is about 115 bytes of log and 97 of index per track, the
// second term being the index: 1.06 MB for 5,000 tracks, and room for about
// 7,400 in 1.5 MB.
//
// Keeping the strings twice at the end of a build (once in the log, once in
// the pool) is what this costs. Reordering them in place would save the
// second copy, about 60 of those 212 bytes per track, at the price of moving
// strings of different lengths through one another; with 5,000 tracks wanted
// and 7,400 fitting, the simple way wins.
//
// MEMORY ACCESS
//
// Everything in the arena is copied into and out of local variables, never
// reached through a pointer to a struct or an integer: that is right
// whatever the arena was declared as, and the layout cannot depend on the
// compiler's padding (the row structs have none, which is asserted) or on
// the size of a pointer (there are none).
//
// The index starts at a multiple of four: if the arena does not, its first
// one to three bytes are skipped. With every table at a multiple of four
// inside the file (the loader checks), every 16- and 32-bit field of the
// index is aligned in memory, and the compiler is told so, so that on the
// ESP32 these copies are single loads and stores. The log is not aligned
// and is read without that promise, a byte at a time.
//
// TRUST
//
// A loaded file is checked before any query sees it; see library_load. The
// queries then rely on exactly what was checked and on nothing else.

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the index is little-endian and is used as it lies in the file"
#endif
#endif

// ALIGNED tells the compiler what the layout guarantees. COPY is memcpy for
// a few bytes: ESP-IDF builds Xtensa code with -fno-builtin-memcpy, which
// turns every memcpy, even of one aligned word, into a call; asking for the
// builtin by name gets the load or store back.
#if defined(__GNUC__)
#define ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#define COPY(to, from, n) __builtin_memcpy((to), (from), (n))
#else
#define ALIGNED(p, n) (p)
#define COPY(to, from, n) memcpy((to), (from), (n))
#endif

#define INDEX_MAGIC 0x4C504144u   // "DAPL"
#define BUILDER_MAGIC 0x42504144u // "DAPB": an arena still being built in
#define INDEX_VERSION 1u
#define MAX_TRACKS 65535u

// What a query answers when asked about a number that does not exist.
#define NO_NUMBER UINT32_MAX
static const char EMPTY[] = "";

// --- The file ----------------------------------------------------------------

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t total; // bytes in the whole index, header included
  uint32_t crc;   // of everything after this field
  uint32_t n_tracks;
  uint32_t n_albums;
  uint32_t n_artists;
  uint32_t n_dirs;
  uint32_t pool_bytes;
  uint32_t off_tracks;
  uint32_t off_albums;
  uint32_t off_artists;
  uint32_t off_dirs;
  uint32_t off_songs;
  uint32_t off_paths;
  uint32_t off_album_names;
  uint32_t off_formats;
  uint32_t off_pool;
} header_t;

#define CRC_FROM offsetof(header_t, n_tracks)

typedef struct {
  uint32_t title;
  uint32_t artist;
  uint32_t name;
  uint32_t duration_ms;
  uint16_t album;
  uint16_t dir;
  uint16_t track_no;
  uint16_t disc_no;
} track_row_t;

typedef struct {
  uint32_t name;
  uint16_t first_track;
  uint16_t artist;
} album_row_t;

typedef struct {
  uint32_t name;
  uint16_t first_album;
  uint16_t first_track;
} artist_row_t;

typedef struct {
  uint32_t name;
  uint16_t first; // in the path order
  uint16_t reserved;
} dir_row_t;

_Static_assert(sizeof(header_t) == 72, "header layout");
_Static_assert(offsetof(header_t, off_pool) == 68, "header layout");
_Static_assert(CRC_FROM == 16, "header layout");
_Static_assert(sizeof(track_row_t) == 24, "track row layout");
_Static_assert(offsetof(track_row_t, album) == 16, "track row layout");
_Static_assert(offsetof(track_row_t, disc_no) == 22, "track row layout");
_Static_assert(sizeof(album_row_t) == 8, "album row layout");
_Static_assert(offsetof(album_row_t, artist) == 6, "album row layout");
_Static_assert(sizeof(artist_row_t) == 8, "artist row layout");
_Static_assert(offsetof(artist_row_t, first_track) == 6, "artist row layout");
_Static_assert(sizeof(dir_row_t) == 8, "folder row layout");

// Aligned fields of the index, and unaligned ones of the log.
static inline uint32_t row32(const uint8_t *p) {
  uint32_t v;
  COPY(&v, ALIGNED(p, 4), sizeof(v));
  return v;
}

static inline uint16_t row16(const uint8_t *p) {
  uint16_t v;
  COPY(&v, ALIGNED(p, 2), sizeof(v));
  return v;
}

static inline void set_row16(uint8_t *p, uint32_t v) {
  uint16_t v16 = (uint16_t)v;
  COPY(ALIGNED(p, 2), &v16, sizeof(v16));
}

static inline uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static inline void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

#define LOAD_ROW(row, p) COPY(&(row), ALIGNED((p), 4), sizeof(row))
#define STORE_ROW(p, row) COPY(ALIGNED((p), 4), &(row), sizeof(row))

static inline uint64_t pad4(uint64_t n) { return (n + 3u) & ~(uint64_t)3u; }

// Where each table goes and how long the index is, from the five counts in
// `h`. False if it would not fit in 32 bits.
static bool layout(header_t *h) {
  uint64_t at = sizeof(header_t);
  uint64_t off[9];
  off[0] = at;
  at += (uint64_t)h->n_tracks * sizeof(track_row_t);
  off[1] = at;
  at += (uint64_t)h->n_albums * sizeof(album_row_t);
  off[2] = at;
  at += (uint64_t)h->n_artists * sizeof(artist_row_t);
  off[3] = at;
  at += (uint64_t)h->n_dirs * sizeof(dir_row_t);
  off[4] = at;
  at += pad4((uint64_t)h->n_tracks * 2u);
  off[5] = at;
  at += pad4((uint64_t)h->n_tracks * 2u);
  off[6] = at;
  at += pad4((uint64_t)h->n_albums * 2u);
  off[7] = at;
  at += pad4(h->n_tracks);
  off[8] = at;
  at += pad4(h->pool_bytes);
  if (at > UINT32_MAX) {
    return false;
  }
  h->off_tracks = (uint32_t)off[0];
  h->off_albums = (uint32_t)off[1];
  h->off_artists = (uint32_t)off[2];
  h->off_dirs = (uint32_t)off[3];
  h->off_songs = (uint32_t)off[4];
  h->off_paths = (uint32_t)off[5];
  h->off_album_names = (uint32_t)off[6];
  h->off_formats = (uint32_t)off[7];
  h->off_pool = (uint32_t)off[8];
  h->total = (uint32_t)at;
  return true;
}

// The part of an arena that is used: from the first multiple of four, a
// whole number of words, and no more than 32-bit offsets can reach.
static bool arena_usable(void *arena, size_t arena_bytes, uint8_t **base,
                         uint32_t *cap) {
  if (arena == NULL) {
    return false;
  }
  size_t skip = (4u - ((uintptr_t)arena & 3u)) & 3u;
  size_t usable = arena_bytes > skip ? arena_bytes - skip : 0;
  if (usable > 0xFFFFFFFCu) {
    usable = 0xFFFFFFFCu;
  }
  *base = (uint8_t *)arena + skip;
  *cap = (uint32_t)usable & ~3u;
  return true;
}

// CRC-32 as in zlib, PNG and Ethernet: polynomial 0x04C11DB7 reflected
// (0xEDB88320), start value and final XOR 0xFFFFFFFF, bytes taken low bit
// first. The check value for "123456789" is 0xCBF43926.
static const uint32_t CRC_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u,
    0x706AF48Fu, 0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u,
    0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u,
    0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu,
    0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u, 0x136C9856u,
    0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u,
    0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u,
    0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u, 0x26D930ACu, 0x51DE003Au,
    0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u,
    0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u,
    0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu,
    0x9FBFE4A5u, 0xE8B8D433u, 0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu,
    0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu,
    0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u,
    0xFBD44C65u, 0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u,
    0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au,
    0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u,
    0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu, 0xBE0B1010u,
    0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u,
    0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u,
    0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u,
    0x73DC1683u, 0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
    0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u, 0xF00F9344u,
    0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au,
    0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u,
    0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu, 0xD80D2BDAu, 0xAF0A1B4Cu,
    0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu,
    0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu,
    0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u,
    0x2CD99E8Bu, 0x5BDEAE1Du, 0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu,
    0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu,
    0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u,
    0x18B74777u, 0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu,
    0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u, 0xA00AE278u,
    0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u,
    0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u,
    0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u,
    0xCDD70693u, 0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u,
    0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu,
    0x2D02EF8Du,
};

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc = CRC_TABLE[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

// --- Text ------------------------------------------------------------------

static inline uint8_t fold(uint8_t c) {
  return (uint8_t)(c - 'A') < 26u ? (uint8_t)(c + ('a' - 'A')) : c;
}

#define TO_NUL SIZE_MAX

// The order of all text (see ORDER above). `a` is `an` bytes, or runs to its
// NUL if `an` is TO_NUL; `b` runs to its NUL. Never reads past either's end.
static int text_cmp(const uint8_t *a, size_t an, const uint8_t *b) {
  int as_written = 0;
  for (size_t i = 0;; i++) {
    uint8_t ca = i < an ? a[i] : 0;
    uint8_t cb = b[i];
    if (ca != cb) {
      uint8_t fa = fold(ca);
      uint8_t fb = fold(cb);
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

// The same order without its second half: 0 if the two differ in nothing
// but the case of ASCII letters.
static int folded_cmp(const uint8_t *a, size_t an, const uint8_t *b) {
  for (size_t i = 0;; i++) {
    uint8_t fa = i < an ? fold(a[i]) : 0;
    uint8_t fb = fold(b[i]);
    if (fa != fb) {
      return fa < fb ? -1 : 1;
    }
    if (fa == 0) {
      return 0;
    }
  }
}

// `s` (n bytes, none of them NUL) is the string at `z`: exactly, or ignoring
// case. Stops at z's NUL.
static bool same_bytes(const uint8_t *s, size_t n, const uint8_t *z) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] != z[i]) {
      return false;
    }
  }
  return z[n] == 0;
}

static bool same_folded(const uint8_t *s, size_t n, const uint8_t *z) {
  for (size_t i = 0; i < n; i++) {
    if (fold(s[i]) != fold(z[i])) {
      return false;
    }
  }
  return z[n] == 0;
}

// FNV-1a, then the final mix of MurmurHash3 so that the low bits (all that a
// small table uses) depend on every byte.
static uint32_t hash_text(const uint8_t *s, size_t n, uint32_t seed,
                          bool folded) {
  uint32_t h = 2166136261u ^ seed;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (folded ? fold(s[i]) : s[i])) * 16777619u;
  }
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  h ^= h >> 16;
  return h;
}

// --- Sorting ---------------------------------------------------------------
//
// Everything is sorted as 16-byte entries: three keys compared in turn as
// unsigned integers, and a fourth word saying what the entry stands for. If
// all three keys are equal a tie function decides from that word (or the
// entries are equal, if there is none).

typedef struct {
  uint32_t k0, k1, k2;
  uint32_t id;
} ent_t;

typedef int (*tie_fn)(const uint8_t *base, uint32_t id_a, uint32_t id_b);

// Runs this short are finished by insertion sort.
#define SORT_SMALL 12

// How many times quicksort may split, per doubling of the count, before a
// run is handed to heapsort. Random input needs about 1.4. (0 sends
// everything to heapsort; one of the test programs is built that way.)
#ifndef LIBRARY_SORT_QUICK_BUDGET
#define LIBRARY_SORT_QUICK_BUDGET 3
#endif

typedef struct {
  uint8_t *ents;       // multiple of four
  const uint8_t *base; // for the tie function
  tie_fn tie;
} sort_t;

static inline ent_t ent_get(const uint8_t *ents, int32_t i) {
  ent_t e;
  LOAD_ROW(e, ents + (size_t)i * sizeof(ent_t));
  return e;
}

static inline void ent_put(uint8_t *ents, int32_t i, ent_t e) {
  STORE_ROW(ents + (size_t)i * sizeof(ent_t), e);
}

static inline bool ent_less(const sort_t *s, const ent_t *a, const ent_t *b) {
  if (a->k0 != b->k0) {
    return a->k0 < b->k0;
  }
  if (a->k1 != b->k1) {
    return a->k1 < b->k1;
  }
  if (a->k2 != b->k2) {
    return a->k2 < b->k2;
  }
  return s->tie != NULL && s->tie(s->base, a->id, b->id) < 0;
}

// Put the entry `x` where it belongs in the heap of `n` entries starting at
// `lo`, starting from position `root`, which is free.
static void sift_down(const sort_t *s, int32_t lo, int32_t root, int32_t n,
                      ent_t x) {
  for (;;) {
    int32_t child = 2 * root + 1;
    if (child >= n) {
      break;
    }
    ent_t c = ent_get(s->ents, lo + child);
    if (child + 1 < n) {
      ent_t c2 = ent_get(s->ents, lo + child + 1);
      if (ent_less(s, &c, &c2)) {
        c = c2;
        child++;
      }
    }
    if (!ent_less(s, &x, &c)) {
      break;
    }
    ent_put(s->ents, lo + root, c);
    root = child;
  }
  ent_put(s->ents, lo + root, x);
}

static void heap_sort(const sort_t *s, int32_t lo, int32_t n) {
  for (int32_t i = n / 2 - 1; i >= 0; i--) {
    sift_down(s, lo, i, n, ent_get(s->ents, lo + i));
  }
  for (int32_t end = n - 1; end > 0; end--) {
    ent_t last = ent_get(s->ents, lo + end);
    ent_put(s->ents, lo + end, ent_get(s->ents, lo));
    sift_down(s, lo, 0, end, last);
  }
}

static void insertion_sort(const sort_t *s, int32_t lo, int32_t hi) {
  for (int32_t i = lo + 1; i <= hi; i++) {
    ent_t x = ent_get(s->ents, i);
    int32_t j = i;
    while (j > lo) {
      ent_t before = ent_get(s->ents, j - 1);
      if (!ent_less(s, &x, &before)) {
        break;
      }
      ent_put(s->ents, j, before);
      j--;
    }
    if (j != i) {
      ent_put(s->ents, j, x);
    }
  }
}

// Sort `count` entries at `ents`. No recursion: the runs still to do are
// kept on a small stack, the longer of each pair, so it never holds more
// than log2(count) of them; and entries are 16 bytes in a 32-bit space, so
// there are fewer than 2^28.
static void sort_ents(uint8_t *ents, uint32_t count, tie_fn tie,
                      const uint8_t *base) {
  const sort_t s = {.ents = ents, .base = base, .tie = tie};
  struct {
    int32_t lo, hi;
    uint32_t budget;
  } todo[32];
  int depth = 0;
  int32_t lo = 0;
  int32_t hi = (int32_t)count - 1;
  uint32_t budget = 0;
  for (uint32_t c = count; c > 0; c >>= 1) {
    budget += LIBRARY_SORT_QUICK_BUDGET;
  }
  for (;;) {
    while (hi - lo >= SORT_SMALL) {
      if (budget == 0) {
        heap_sort(&s, lo, hi - lo + 1);
        lo = hi;
        break;
      }
      budget--;
      // The median of the first, middle and last is the pivot, and those
      // three are left in order, so that each scan below stops at one of
      // them at the latest.
      int32_t mid = lo + (hi - lo) / 2;
      ent_t a = ent_get(ents, lo);
      ent_t b = ent_get(ents, mid);
      ent_t c = ent_get(ents, hi);
      ent_t swap;
      if (ent_less(&s, &b, &a)) {
        swap = a, a = b, b = swap;
      }
      if (ent_less(&s, &c, &b)) {
        swap = b, b = c, c = swap;
        if (ent_less(&s, &b, &a)) {
          swap = a, a = b, b = swap;
        }
      }
      ent_put(ents, lo, a);
      ent_put(ents, mid, b);
      ent_put(ents, hi, c);
      const ent_t pivot = b;
      int32_t i = lo + 1;
      int32_t j = hi - 1;
      do {
        ent_t ei = ent_get(ents, i);
        while (ent_less(&s, &ei, &pivot)) {
          ei = ent_get(ents, ++i);
        }
        ent_t ej = ent_get(ents, j);
        while (ent_less(&s, &pivot, &ej)) {
          ej = ent_get(ents, --j);
        }
        if (i <= j) {
          if (i < j) {
            ent_put(ents, i, ej);
            ent_put(ents, j, ei);
          }
          i++;
          j--;
        }
      } while (i <= j);
      // Now [lo, j] <= pivot <= [i, hi]. Keep the longer for later.
      if (j - lo < hi - i) {
        todo[depth].lo = i;
        todo[depth].hi = hi;
        todo[depth].budget = budget;
        hi = j;
      } else {
        todo[depth].lo = lo;
        todo[depth].hi = j;
        todo[depth].budget = budget;
        lo = i;
      }
      depth++;
    }
    insertion_sort(&s, lo, hi);
    if (depth == 0) {
      break;
    }
    depth--;
    lo = todo[depth].lo;
    hi = todo[depth].hi;
    budget = todo[depth].budget;
  }
}

// --- The builder -----------------------------------------------------------

// Offset 0 is the builder itself, so 0 can mean "none" for anything in the
// log.
typedef struct {
  uint32_t magic;
  uint32_t cap;     // usable bytes of the arena
  uint32_t low;     // end of the bucket tables
  uint32_t log_bot; // lowest byte of the log
  uint32_t str_buckets, str_mask;
  uint32_t artist_buckets, artist_mask;
  uint32_t album_buckets, album_mask;
  uint32_t n_tracks, n_albums, n_artists;
  uint32_t n_dirs;     // strings used as a folder
  uint32_t n_strings;  // strings in the log
  uint32_t pool_bytes; // of those the index may hold, with their NULs
  uint32_t last_track, last_album, last_artist; // heads of the three lists
} builder_t;

// A string in the log: a word (the next string in its bucket; later its
// rank; later its place in the pool), a byte of flags, the text, a NUL.
enum { STR_WORD = 0, STR_FLAGS = 4, STR_TEXT = 5 };
enum {
  STR_IN_POOL = 1, // the index will hold it: counted in pool_bytes
  STR_IS_DIR = 2,  // some track's folder: counted in n_dirs
  STR_PLACED = 4,  // copied to the pool; the word is its offset there
};

// An artist and an album in the log. `name` is the string kept.
enum { ARTIST_NEXT = 0, ARTIST_NAME = 4, ARTIST_PREV = 8, ARTIST_NUMBER = 12 };
enum { ARTIST_NODE = 16 };
enum {
  ALBUM_NEXT = 0,
  ALBUM_NAME = 4,
  ALBUM_ARTIST = 8,
  ALBUM_PREV = 12,
  ALBUM_NUMBER = 16
};
enum { ALBUM_NODE = 20 };

// A track in the log.
typedef struct {
  uint32_t prev;                     // the track added before this one
  uint32_t title, artist, name, dir; // strings
  uint32_t album;                    // album node
  uint32_t duration_ms;
  uint16_t track_no, disc_no;
  uint16_t number; // its track number, once sorted
  uint8_t format;
  uint8_t unused;
} record_t;

_Static_assert(sizeof(builder_t) % 4 == 0, "builder layout");
_Static_assert(sizeof(record_t) == 36, "record layout");
_Static_assert(sizeof(ent_t) == 16, "sort entry layout");

static const uint8_t *str_text(const uint8_t *base, uint32_t node) {
  return base + node + STR_TEXT;
}

// The largest power of two not above `n` (1 for 0).
static uint32_t pow2_floor(uint32_t n) {
  uint32_t p = 1;
  while (p <= n / 2) {
    p *= 2;
  }
  return p;
}

library_builder_t *library_builder_begin(void *arena, size_t arena_bytes) {
  uint8_t *base;
  uint32_t cap;
  if (!arena_usable(arena, arena_bytes, &base, &cap)) {
    return NULL;
  }
  builder_t b;
  memset(&b, 0, sizeof(b));
  uint32_t str_n = pow2_floor(cap / 32);
  uint32_t group_n = pow2_floor(cap / 256);
  uint64_t low = sizeof(b) + 4ull * str_n + 8ull * group_n;
  // With `low` at least the builder's own size an empty index (a header)
  // fits too.
  _Static_assert(sizeof(builder_t) >= sizeof(header_t), "empty index fits");
  if (low > cap) {
    return NULL;
  }
  b.magic = BUILDER_MAGIC;
  b.cap = cap;
  b.low = (uint32_t)low;
  b.log_bot = cap;
  b.str_buckets = (uint32_t)sizeof(b);
  b.str_mask = str_n - 1;
  b.artist_buckets = b.str_buckets + 4 * str_n;
  b.artist_mask = group_n - 1;
  b.album_buckets = b.artist_buckets + 4 * group_n;
  b.album_mask = group_n - 1;
  memset(base + sizeof(b), 0, (size_t)low - sizeof(b));
  STORE_ROW(base, b);
  return (library_builder_t *)base;
}

// Would everything added so far still finish? See BUILDING above.
static bool fits(const builder_t *b) {
  header_t h;
  h.n_tracks = b->n_tracks;
  h.n_albums = b->n_albums;
  h.n_artists = b->n_artists;
  h.n_dirs = b->n_dirs;
  h.pool_bytes = b->pool_bytes;
  if (!layout(&h)) {
    return false;
  }
  uint64_t top = b->log_bot & ~3u;
  uint64_t ents = (uint64_t)b->n_strings + b->n_albums;
  if (ents < b->n_tracks) {
    ents = b->n_tracks;
  }
  uint64_t tables = h.off_pool > b->low ? h.off_pool : b->low;
  return tables + ents * sizeof(ent_t) <= top && h.total <= top;
}

// One add: a working copy of the builder, and what to put back in the arena
// if the track turns out not to fit. A track changes at most six bucket
// heads and flag bytes of strings, and for each of its artist and album
// either a bucket head (a new one) or the name kept (a better spelling).
typedef struct {
  uint8_t *base;
  builder_t b;
  bool failed;
  unsigned n_undo;
  struct {
    uint32_t at;
    uint32_t old;
    bool word; // a 32-bit word, or else one byte
  } undo[16];
} add_t;

static void remember32(add_t *a, uint32_t at) {
  a->undo[a->n_undo].at = at;
  a->undo[a->n_undo].old = get32(a->base + at);
  a->undo[a->n_undo].word = true;
  a->n_undo++;
}

static void remember8(add_t *a, uint32_t at) {
  a->undo[a->n_undo].at = at;
  a->undo[a->n_undo].old = a->base[at];
  a->undo[a->n_undo].word = false;
  a->n_undo++;
}

static void undo_all(add_t *a) {
  while (a->n_undo > 0) {
    a->n_undo--;
    if (a->undo[a->n_undo].word) {
      put32(a->base + a->undo[a->n_undo].at, a->undo[a->n_undo].old);
    } else {
      a->base[a->undo[a->n_undo].at] = (uint8_t)a->undo[a->n_undo].old;
    }
  }
}

// Room for `bytes` more at the bottom of the log; 0 if there is none.
static uint32_t log_push(add_t *a, size_t bytes) {
  if (a->failed || bytes > a->b.log_bot - a->b.low) {
    a->failed = true;
    return 0;
  }
  a->b.log_bot -= (uint32_t)bytes;
  return a->b.log_bot;
}

// The log's one copy of the string `s` (n bytes), added if it is new, and
// marked with `flags`.
static uint32_t intern(add_t *a, const uint8_t *s, size_t n, uint8_t flags) {
  if (a->failed) {
    return 0;
  }
  uint8_t *base = a->base;
  uint32_t bucket =
      a->b.str_buckets + 4 * (hash_text(s, n, 0, false) & a->b.str_mask);
  uint32_t head = get32(base + bucket);
  uint32_t node = head;
  while (node != 0 && !same_bytes(s, n, str_text(base, node))) {
    node = get32(base + node + STR_WORD);
  }
  uint8_t had = 0;
  if (node == 0) {
    node = log_push(a, STR_TEXT + n + 1);
    if (node == 0) {
      return 0;
    }
    put32(base + node + STR_WORD, head);
    memcpy(base + node + STR_TEXT, s, n);
    base[node + STR_TEXT + n] = 0;
    remember32(a, bucket);
    put32(base + bucket, node);
    a->b.n_strings++;
  } else {
    had = base[node + STR_FLAGS];
    if ((flags & ~had) != 0) {
      remember8(a, node + STR_FLAGS);
    }
  }
  base[node + STR_FLAGS] = had | flags;
  if ((flags & ~had & STR_IN_POOL) != 0) {
    a->b.pool_bytes += (uint32_t)n + 1;
  }
  if ((flags & ~had & STR_IS_DIR) != 0) {
    a->b.n_dirs++;
  }
  return node;
}

// The name kept at `field` (of an artist or album node) is the first, in
// text order, of the spellings seen: `s` takes its place if it comes before
// it.
static void prefer_spelling(add_t *a, uint32_t field, const uint8_t *s,
                            size_t n) {
  uint8_t *base = a->base;
  if (text_cmp(s, n, str_text(base, get32(base + field))) >= 0) {
    return;
  }
  uint32_t better = intern(a, s, n, STR_IN_POOL);
  if (better != 0) {
    remember32(a, field);
    put32(base + field, better);
  }
}

// `s` without the spaces around it.
static const uint8_t *trimmed(const char *s, size_t *n) {
  while (*s == ' ') {
    s++;
  }
  size_t len = strlen(s);
  while (len > 0 && s[len - 1] == ' ') {
    len--;
  }
  *n = len;
  return (const uint8_t *)s;
}

static uint32_t find_or_add_artist(add_t *a, const char *name) {
  if (a->failed) {
    return 0;
  }
  uint8_t *base = a->base;
  size_t n;
  const uint8_t *s = trimmed(name, &n);
  uint32_t bucket =
      a->b.artist_buckets + 4 * (hash_text(s, n, 0, true) & a->b.artist_mask);
  uint32_t head = get32(base + bucket);
  for (uint32_t node = head; node != 0;
       node = get32(base + node + ARTIST_NEXT)) {
    if (same_folded(s, n, str_text(base, get32(base + node + ARTIST_NAME)))) {
      prefer_spelling(a, node + ARTIST_NAME, s, n);
      return node;
    }
  }
  uint32_t kept = intern(a, s, n, STR_IN_POOL);
  uint32_t node = log_push(a, ARTIST_NODE);
  if (node == 0) {
    return 0;
  }
  put32(base + node + ARTIST_NEXT, head);
  put32(base + node + ARTIST_NAME, kept);
  put32(base + node + ARTIST_PREV, a->b.last_artist);
  put32(base + node + ARTIST_NUMBER, 0);
  remember32(a, bucket);
  put32(base + bucket, node);
  a->b.last_artist = node;
  a->b.n_artists++;
  return node;
}

static uint32_t find_or_add_album(add_t *a, uint32_t artist, const char *name) {
  if (a->failed) {
    return 0;
  }
  uint8_t *base = a->base;
  size_t n;
  const uint8_t *s = trimmed(name, &n);
  uint32_t bucket = a->b.album_buckets +
                    4 * (hash_text(s, n, artist, true) & a->b.album_mask);
  uint32_t head = get32(base + bucket);
  for (uint32_t node = head; node != 0;
       node = get32(base + node + ALBUM_NEXT)) {
    if (get32(base + node + ALBUM_ARTIST) == artist &&
        same_folded(s, n, str_text(base, get32(base + node + ALBUM_NAME)))) {
      prefer_spelling(a, node + ALBUM_NAME, s, n);
      return node;
    }
  }
  uint32_t kept = intern(a, s, n, STR_IN_POOL);
  uint32_t node = log_push(a, ALBUM_NODE);
  if (node == 0) {
    return 0;
  }
  put32(base + node + ALBUM_NEXT, head);
  put32(base + node + ALBUM_NAME, kept);
  put32(base + node + ALBUM_ARTIST, artist);
  put32(base + node + ALBUM_PREV, a->b.last_album);
  put32(base + node + ALBUM_NUMBER, 0);
  remember32(a, bucket);
  put32(base + bucket, node);
  a->b.last_album = node;
  a->b.n_albums++;
  return node;
}

static const char *or_empty(const char *s) { return s != NULL ? s : EMPTY; }

library_result_t library_builder_add(library_builder_t *builder,
                                     const library_track_in_t *track) {
  if (builder == NULL || track == NULL) {
    return LIBRARY_FULL;
  }
  add_t a;
  a.base = (uint8_t *)builder;
  a.failed = false;
  a.n_undo = 0;
  LOAD_ROW(a.b, a.base);
  if (a.b.magic != BUILDER_MAGIC || a.b.n_tracks >= MAX_TRACKS) {
    return LIBRARY_FULL;
  }

  const char *path = or_empty(track->path);
  const char *title = or_empty(track->title);
  const char *artist = or_empty(track->artist);
  size_t path_len = strlen(path);
  size_t dir_len = path_len; // up to and including the last '/'
  while (dir_len > 0 && path[dir_len - 1] != '/') {
    dir_len--;
  }

  record_t rec;
  memset(&rec, 0, sizeof(rec));
  rec.title = intern(&a, (const uint8_t *)title, strlen(title), STR_IN_POOL);
  rec.artist = intern(&a, (const uint8_t *)artist, strlen(artist), STR_IN_POOL);
  rec.dir =
      intern(&a, (const uint8_t *)path, dir_len, STR_IN_POOL | STR_IS_DIR);
  rec.name = intern(&a, (const uint8_t *)path + dir_len, path_len - dir_len,
                    STR_IN_POOL);
  rec.album = find_or_add_album(
      &a, find_or_add_artist(&a, or_empty(track->group_artist)),
      or_empty(track->album));
  rec.duration_ms = track->duration_ms;
  rec.track_no = track->track_no;
  rec.disc_no = track->disc_no;
  rec.format = track->format;
  rec.prev = a.b.last_track;
  uint32_t at = log_push(&a, sizeof(rec));
  a.b.last_track = at;
  a.b.n_tracks++;
  if (a.failed || !fits(&a.b)) {
    // The log entries just written are below the builder's log_bot, which
    // has not been stored: they are free space again.
    undo_all(&a);
    return LIBRARY_FULL;
  }
  memcpy(a.base + at, &rec, sizeof(rec));
  STORE_ROW(a.base, a.b);
  return LIBRARY_OK;
}

// --- Finishing -------------------------------------------------------------

static int tie_text(const uint8_t *base, uint32_t a, uint32_t b) {
  return text_cmp(str_text(base, a), TO_NUL, str_text(base, b));
}

static inline uint32_t rank_of(const uint8_t *base, uint32_t str) {
  return get32(base + str + STR_WORD);
}

// Two tracks of one album with the same disc, number and file name: decide
// by everything else, so that only identical tracks are equal.
static int tie_track(const uint8_t *base, uint32_t a, uint32_t b) {
  record_t ra, rb;
  memcpy(&ra, base + a, sizeof(ra));
  memcpy(&rb, base + b, sizeof(rb));
  uint32_t ka[5] = {rank_of(base, ra.dir), rank_of(base, ra.title),
                    rank_of(base, ra.artist), ra.duration_ms, ra.format};
  uint32_t kb[5] = {rank_of(base, rb.dir), rank_of(base, rb.title),
                    rank_of(base, rb.artist), rb.duration_ms, rb.format};
  for (int i = 0; i < 5; i++) {
    if (ka[i] != kb[i]) {
      return ka[i] < kb[i] ? -1 : 1;
    }
  }
  return 0;
}

// The string `node` is wanted in the pool: copy it there if this is the
// first time, and say where it is.
static uint32_t place(uint8_t *base, uint8_t *pool, uint32_t *pool_used,
                      uint32_t node) {
  if ((base[node + STR_FLAGS] & STR_PLACED) == 0) {
    const uint8_t *text = str_text(base, node);
    size_t bytes = strlen((const char *)text) + 1;
    memcpy(pool + *pool_used, text, bytes);
    put32(base + node + STR_WORD, *pool_used);
    base[node + STR_FLAGS] |= STR_PLACED;
    *pool_used += (uint32_t)bytes;
  }
  return get32(base + node + STR_WORD);
}

const library_t *library_builder_finish(library_builder_t *builder) {
  if (builder == NULL) {
    return NULL;
  }
  uint8_t *base = (uint8_t *)builder;
  builder_t b;
  LOAD_ROW(b, base);
  header_t h;
  memset(&h, 0, sizeof(h));
  h.n_tracks = b.n_tracks;
  h.n_albums = b.n_albums;
  h.n_artists = b.n_artists;
  h.n_dirs = b.n_dirs;
  h.pool_bytes = b.pool_bytes;
  // Every add left the builder in a state that fits (see fits), so the
  // second test cannot fail for a builder that begin made.
  if (b.magic != BUILDER_MAGIC || !layout(&h) || !fits(&b)) {
    return NULL;
  }
  uint8_t *tracks = base + h.off_tracks;
  uint8_t *albums = base + h.off_albums;
  uint8_t *artists = base + h.off_artists;
  uint8_t *dirs = base + h.off_dirs;
  uint8_t *songs = base + h.off_songs;
  uint8_t *paths = base + h.off_paths;
  uint8_t *album_names = base + h.off_album_names;
  uint8_t *formats = base + h.off_formats;
  uint8_t *pool = base + h.off_pool;
  // Scratch for the sorts: above the tables (and above the buckets, which
  // step 1 reads), where the pool will go once the sorting is done.
  uint8_t *scratch_base = base + (h.off_pool > b.low ? h.off_pool : b.low);
  ent_t e;

  // 1. Rank every string. The buckets are still intact and list all the
  //    strings there are. After this the builder at offset 0 and the
  //    buckets are dead, and the tables may be written over them.
  uint8_t *ranked = scratch_base;
  uint32_t n_ranked = 0;
  for (uint32_t i = 0; i <= b.str_mask; i++) {
    for (uint32_t node = get32(base + b.str_buckets + 4 * i); node != 0;
         node = get32(base + node + STR_WORD)) {
      const uint8_t *text = str_text(base, node);
      e.k0 = e.k1 = e.k2 = 0;
      e.id = node;
      uint32_t *key = &e.k0;
      for (unsigned c = 0; c < 12 && text[c] != 0; c++) {
        if (c == 4) {
          key = &e.k1;
        } else if (c == 8) {
          key = &e.k2;
        }
        *key |= (uint32_t)fold(text[c]) << (24 - 8 * (c % 4));
      }
      ent_put(ranked, (int32_t)n_ranked++, e);
    }
  }
  sort_ents(ranked, n_ranked, tie_text, base);
  // A string's rank goes into its log entry. Its sort entry gets the rank
  // of the first string that differs from it only in case: the key for a
  // name whose spelling must not matter.
  ent_t before = {0, 0, 0, 0};
  uint32_t class_rank = 0;
  for (uint32_t i = 0; i < n_ranked; i++) {
    e = ent_get(ranked, (int32_t)i);
    bool same_class = i > 0 && e.k0 == before.k0 && e.k1 == before.k1 &&
                      e.k2 == before.k2 &&
                      same_folded(str_text(base, e.id),
                                  strlen((const char *)str_text(base, e.id)),
                                  str_text(base, before.id));
    if (!same_class) {
      class_rank = i;
    }
    put32(base + e.id + STR_WORD, i);
    before = e;
    e.k0 = class_rank;
    ent_put(ranked, (int32_t)i, e);
  }
#define CLASS_OF(str) (ent_get(ranked, (int32_t)rank_of(base, (str))).k0)

  // 2. Artists A to Z.
  uint8_t *scratch = ranked + (size_t)b.n_strings * sizeof(ent_t);
  uint32_t n = 0;
  for (uint32_t node = b.last_artist; node != 0;
       node = get32(base + node + ARTIST_PREV)) {
    e.k0 = CLASS_OF(get32(base + node + ARTIST_NAME));
    e.k1 = e.k2 = 0;
    e.id = node;
    ent_put(scratch, (int32_t)n++, e);
  }
  sort_ents(scratch, n, NULL, base);
  for (uint32_t i = 0; i < n; i++) {
    e = ent_get(scratch, (int32_t)i);
    put32(base + e.id + ARTIST_NUMBER, i);
    artist_row_t row = {.name = get32(base + e.id + ARTIST_NAME)};
    STORE_ROW(artists + i * sizeof(row), row);
  }

  // 3. Albums by artist, then name; and the album order.
  n = 0;
  for (uint32_t node = b.last_album; node != 0;
       node = get32(base + node + ALBUM_PREV)) {
    e.k0 = get32(base + get32(base + node + ALBUM_ARTIST) + ARTIST_NUMBER);
    e.k1 = CLASS_OF(get32(base + node + ALBUM_NAME));
    e.k2 = 0;
    e.id = node;
    ent_put(scratch, (int32_t)n++, e);
  }
  sort_ents(scratch, n, NULL, base);
  for (uint32_t i = 0; i < n; i++) {
    e = ent_get(scratch, (int32_t)i);
    put32(base + e.id + ALBUM_NUMBER, i);
    album_row_t row = {.name = get32(base + e.id + ALBUM_NAME),
                       .artist = (uint16_t)e.k0};
    STORE_ROW(albums + i * sizeof(row), row);
    if (i == 0 || e.k0 != before.k0) {
      set_row16(artists + e.k0 * sizeof(artist_row_t) +
                    offsetof(artist_row_t, first_album),
                i);
    }
    before = e;
    e.k0 = e.k1;
    e.k1 = i;
    e.id = i;
    ent_put(scratch, (int32_t)i, e);
  }
  sort_ents(scratch, n, NULL, base);
  for (uint32_t i = 0; i < n; i++) {
    set_row16(album_names + 2 * i, ent_get(scratch, (int32_t)i).id);
  }
#undef CLASS_OF

  // 4. Tracks. The string ranks are no longer needed as sort entries, only
  //    as the ranks in the log, so the scratch starts again.
  scratch = scratch_base;
  record_t rec;
  n = 0;
  for (uint32_t at = b.last_track; at != 0; at = rec.prev) {
    memcpy(&rec, base + at, sizeof(rec));
    e.k0 = get32(base + rec.album + ALBUM_NUMBER) << 16 | rec.disc_no;
    e.k1 = rec.track_no;
    e.k2 = rank_of(base, rec.name);
    e.id = at;
    ent_put(scratch, (int32_t)n++, e);
  }
  sort_ents(scratch, n, tie_track, base);
  for (uint32_t i = 0; i < n; i++) {
    e = ent_get(scratch, (int32_t)i);
    memcpy(&rec, base + e.id, sizeof(rec));
    rec.number = (uint16_t)i;
    memcpy(base + e.id, &rec, sizeof(rec));
    uint32_t album = e.k0 >> 16;
    track_row_t row = {.title = rec.title,
                       .artist = rec.artist,
                       .name = rec.name,
                       .duration_ms = rec.duration_ms,
                       .album = (uint16_t)album,
                       .track_no = rec.track_no,
                       .disc_no = rec.disc_no};
    STORE_ROW(tracks + i * sizeof(row), row);
    formats[i] = rec.format;
    if (i == 0 || album != before.k0 >> 16) {
      set_row16(albums + album * sizeof(album_row_t) +
                    offsetof(album_row_t, first_track),
                i);
    }
    before = e;
  }
  for (uint32_t i = 0; i < b.n_artists; i++) {
    uint8_t *row = artists + i * sizeof(artist_row_t);
    uint32_t first = row16(row + offsetof(artist_row_t, first_album));
    set_row16(row + offsetof(artist_row_t, first_track),
              row16(albums + first * sizeof(album_row_t) +
                    offsetof(album_row_t, first_track)));
  }

  // 5. Songs A to Z.
  for (uint32_t i = 0; i < n; i++) {
    e.k0 = rank_of(base, row32(tracks + i * sizeof(track_row_t) +
                               offsetof(track_row_t, title)));
    e.k1 = i;
    e.k2 = 0;
    e.id = i;
    ent_put(scratch, (int32_t)i, e);
  }
  sort_ents(scratch, n, NULL, base);
  for (uint32_t i = 0; i < n; i++) {
    set_row16(songs + 2 * i, ent_get(scratch, (int32_t)i).id);
  }

  // 6. The path order, and the folders it falls into.
  n = 0;
  for (uint32_t at = b.last_track; at != 0; at = rec.prev) {
    memcpy(&rec, base + at, sizeof(rec));
    e.k0 = rank_of(base, rec.dir);
    e.k1 = rank_of(base, rec.name);
    e.k2 = rec.number;
    e.id = rec.dir;
    ent_put(scratch, (int32_t)n++, e);
  }
  sort_ents(scratch, n, NULL, base);
  uint32_t n_dirs = 0;
  for (uint32_t i = 0; i < n; i++) {
    e = ent_get(scratch, (int32_t)i);
    if (i == 0 || e.k0 != before.k0) {
      dir_row_t row = {.name = e.id, .first = (uint16_t)i};
      STORE_ROW(dirs + n_dirs * sizeof(row), row);
      n_dirs++;
    }
    before = e;
    set_row16(paths + 2 * i, e.k2);
    set_row16(tracks + e.k2 * sizeof(track_row_t) + offsetof(track_row_t, dir),
              n_dirs - 1);
  }

  // 7. The pool: each string copied when it is first referred to, and the
  //    reference changed from where the string is in the log to where it is
  //    in the pool. The scratch is dead, and the pool may run over it.
  uint32_t used = 0;
#define PLACE(field_at)                                                        \
  do {                                                                         \
    uint8_t *field_ = (field_at);                                              \
    uint32_t where_ = place(base, pool, &used, row32(field_));                 \
    COPY(ALIGNED(field_, 4), &where_, sizeof(where_));                         \
  } while (0)
  for (uint32_t i = 0; i < b.n_artists; i++) {
    PLACE(artists + i * sizeof(artist_row_t) + offsetof(artist_row_t, name));
  }
  for (uint32_t i = 0; i < b.n_albums; i++) {
    PLACE(albums + i * sizeof(album_row_t) + offsetof(album_row_t, name));
  }
  for (uint32_t i = 0; i < b.n_tracks; i++) {
    PLACE(tracks + i * sizeof(track_row_t) + offsetof(track_row_t, artist));
    PLACE(tracks + i * sizeof(track_row_t) + offsetof(track_row_t, title));
  }
  for (uint32_t i = 0; i < b.n_dirs; i++) {
    PLACE(dirs + i * sizeof(dir_row_t) + offsetof(dir_row_t, name));
  }
  for (uint32_t i = 0; i < b.n_tracks; i++) {
    PLACE(tracks + row16(paths + 2 * i) * sizeof(track_row_t) +
          offsetof(track_row_t, name));
  }
#undef PLACE

  // The counts kept while adding are what the tables were laid out by; if
  // they were wrong, so is the index. The pool may have come out smaller
  // than was allowed for (see GROUPING); it is the last thing in the index,
  // so only the total changes.
  if (n_dirs != b.n_dirs || used > b.pool_bytes) {
    return NULL;
  }
  h.pool_bytes = used;
  if (!layout(&h)) {
    return NULL;
  }

  // 8. Zeros in the gaps, so that the same library is the same bytes; then
  //    the header.
  memset(songs + 2 * (size_t)b.n_tracks, 0,
         h.off_paths - h.off_songs - 2 * b.n_tracks);
  memset(paths + 2 * (size_t)b.n_tracks, 0,
         h.off_album_names - h.off_paths - 2 * b.n_tracks);
  memset(album_names + 2 * (size_t)b.n_albums, 0,
         h.off_formats - h.off_album_names - 2 * b.n_albums);
  memset(formats + b.n_tracks, 0, h.off_pool - h.off_formats - b.n_tracks);
  memset(pool + used, 0, h.total - h.off_pool - used);
  h.magic = INDEX_MAGIC;
  h.version = INDEX_VERSION;
  STORE_ROW(base, h);
  h.crc = crc32(base + CRC_FROM, h.total - CRC_FROM);
  STORE_ROW(base, h);
  return (const library_t *)base;
}

// --- Saving and loading ----------------------------------------------------

#define FIELD(lib, field)                                                      \
  row32((const uint8_t *)(lib) + offsetof(header_t, field))

library_result_t library_save(const library_t *lib, FILE *f) {
  if (lib == NULL || f == NULL) {
    return LIBRARY_IO;
  }
  uint32_t total = FIELD(lib, total);
  // One write of the whole index; the flush is so that a card that is full
  // or gone is reported here and not lost in the caller's fclose.
  if (fwrite(lib, 1, total, f) != total || fflush(f) != 0) {
    return LIBRARY_IO;
  }
  return LIBRARY_OK;
}

// Is the index at `base` (its header is `h`, already checked against the
// counts) one that the queries can be let loose on?
//
// The queries follow these and nothing else, and each is checked here:
//
//   pool offsets (track title, artist, file name; album, artist and folder
//   names): below pool_bytes; and the pool's last byte is NUL, so a string
//   starting anywhere in the pool ends inside it.
//   album, artist and folder numbers in rows; track and album numbers in
//   the three orders: below their counts.
//   run starts (album.first_track, artist.first_album, artist.first_track,
//   folder.first): the first row's is 0, each is above the row before's and
//   below the count. So every run is non-empty, inside its table, and "next
//   row's start minus this row's" cannot wrap.
//
// It also checks that the runs agree with the rows they cover (a track's
// album is the album whose run it is in, an album's artist likewise, and an
// artist's first track is its first album's), so that going down from an
// artist and up from a track give the same answer.
//
// It does not check that the three orders are permutations or sorted: that
// would cost a string comparison per track at every start-up, and a wrong
// order can only make a list look wrong or library_find_path miss; the
// searches are bounded by the run they search whatever is in it.
static bool structure_ok(const uint8_t *base, const header_t *h) {
  uint32_t n_tracks = h->n_tracks;
  uint32_t n_albums = h->n_albums;
  uint32_t n_artists = h->n_artists;
  uint32_t n_dirs = h->n_dirs;
  uint32_t pool_bytes = h->pool_bytes;
  if (n_tracks == 0) {
    return n_albums == 0 && n_artists == 0 && n_dirs == 0;
  }
  if (n_tracks > MAX_TRACKS || n_albums == 0 || n_albums > n_tracks ||
      n_artists == 0 || n_artists > n_albums || n_dirs == 0 ||
      n_dirs > n_tracks) {
    return false;
  }
  if (pool_bytes == 0 || base[h->off_pool + pool_bytes - 1] != 0) {
    return false;
  }

  const uint8_t *albums = base + h->off_albums;
  uint32_t before = 0;
  for (uint32_t i = 0; i < n_artists; i++) {
    artist_row_t row;
    LOAD_ROW(row, base + h->off_artists + i * sizeof(row));
    if (row.name >= pool_bytes || row.first_album >= n_albums ||
        (i == 0 ? row.first_album != 0 : row.first_album <= before) ||
        row.first_track !=
            row16(albums + row.first_album * sizeof(album_row_t) +
                  offsetof(album_row_t, first_track))) {
      return false;
    }
    before = row.first_album;
  }

  uint32_t artist = 0;            // the artist whose run album i is in
  uint32_t next_start = n_albums; // where the next artist's run starts
  if (n_artists > 1) {
    next_start = row16(base + h->off_artists + sizeof(artist_row_t) +
                       offsetof(artist_row_t, first_album));
  }
  for (uint32_t i = 0; i < n_albums; i++) {
    album_row_t row;
    LOAD_ROW(row, albums + i * sizeof(row));
    if (i == next_start) {
      artist++;
      next_start = artist + 1 < n_artists
                       ? row16(base + h->off_artists +
                               (artist + 1) * sizeof(artist_row_t) +
                               offsetof(artist_row_t, first_album))
                       : n_albums;
    }
    if (row.name >= pool_bytes || row.artist != artist ||
        row.first_track >= n_tracks ||
        (i == 0 ? row.first_track != 0 : row.first_track <= before)) {
      return false;
    }
    before = row.first_track;
  }

  uint32_t album = 0;
  next_start = n_tracks;
  if (n_albums > 1) {
    next_start = row16(albums + sizeof(album_row_t) +
                       offsetof(album_row_t, first_track));
  }
  for (uint32_t i = 0; i < n_tracks; i++) {
    track_row_t row;
    LOAD_ROW(row, base + h->off_tracks + i * sizeof(row));
    if (i == next_start) {
      album++;
      next_start = album + 1 < n_albums
                       ? row16(albums + (album + 1) * sizeof(album_row_t) +
                               offsetof(album_row_t, first_track))
                       : n_tracks;
    }
    if (row.title >= pool_bytes || row.artist >= pool_bytes ||
        row.name >= pool_bytes || row.album != album || row.dir >= n_dirs) {
      return false;
    }
  }

  for (uint32_t i = 0; i < n_dirs; i++) {
    dir_row_t row;
    LOAD_ROW(row, base + h->off_dirs + i * sizeof(row));
    if (row.name >= pool_bytes || row.first >= n_tracks ||
        (i == 0 ? row.first != 0 : row.first <= before)) {
      return false;
    }
    before = row.first;
  }

  for (uint32_t i = 0; i < n_tracks; i++) {
    if (row16(base + h->off_songs + 2 * i) >= n_tracks ||
        row16(base + h->off_paths + 2 * i) >= n_tracks) {
      return false;
    }
  }
  for (uint32_t i = 0; i < n_albums; i++) {
    if (row16(base + h->off_album_names + 2 * i) >= n_albums) {
      return false;
    }
  }
  return true;
}

// The checks, in order; the file is trusted only after the last:
//
//   1. the header is all there, with the right magic and version;
//   2. the total it states is at least a header, a multiple of four, and no
//      more than the arena (LIBRARY_FULL: the file may be good, but not for
//      this arena);
//   3. its counts, laid out, give exactly the table offsets and the total
//      it states (so every table lies inside the total, in order, with no
//      overlap, at a multiple of four);
//   4. the file holds exactly that many bytes: no fewer, and none after;
//   5. the CRC-32 of everything after the checksum field matches;
//   6. everything a query follows is in range (structure_ok).
//
// Nothing is written to the arena until 3 has passed, and never beyond the
// stated total. Steps 1 to 5 catch damage; 3 and 6 are what make a file
// safe even if someone built it wrong on purpose and gave it a good
// checksum.
library_result_t library_load(void *arena, size_t arena_bytes, FILE *f,
                              const library_t **out) {
  if (out == NULL) {
    return LIBRARY_BAD_FILE;
  }
  *out = NULL;
  if (f == NULL) {
    return LIBRARY_IO;
  }
  uint8_t *base;
  uint32_t cap;
  if (!arena_usable(arena, arena_bytes, &base, &cap)) {
    return LIBRARY_FULL;
  }
  header_t h;
  if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
    return ferror(f) ? LIBRARY_IO : LIBRARY_BAD_FILE;
  }
  if (h.magic != INDEX_MAGIC || h.version != INDEX_VERSION ||
      h.total < sizeof(h) || h.total % 4 != 0) {
    return LIBRARY_BAD_FILE;
  }
  if (h.total > cap) {
    return LIBRARY_FULL;
  }
  header_t want = h;
  if (!layout(&want) || memcmp(&want, &h, sizeof(h)) != 0) {
    return LIBRARY_BAD_FILE;
  }
  STORE_ROW(base, h);
  size_t body = h.total - sizeof(h);
  if (body > 0 && fread(base + sizeof(h), 1, body, f) != body) {
    return ferror(f) ? LIBRARY_IO : LIBRARY_BAD_FILE;
  }
  if (fgetc(f) != EOF) {
    return LIBRARY_BAD_FILE;
  }
  if (ferror(f)) {
    return LIBRARY_IO;
  }
  if (crc32(base + CRC_FROM, h.total - CRC_FROM) != h.crc ||
      !structure_ok(base, &h)) {
    return LIBRARY_BAD_FILE;
  }
  *out = (const library_t *)base;
  return LIBRARY_OK;
}

// --- Queries ---------------------------------------------------------------
//
// Each reads a field or two of the header (its first three cache lines,
// which every query shares) and then only what is said at each function.

static inline const uint8_t *table(const library_t *lib, uint32_t offset) {
  return (const uint8_t *)lib + offset;
}

static inline const char *pool_text(const library_t *lib, uint32_t at) {
  return (const char *)lib + FIELD(lib, off_pool) + at;
}

// Where run `i` of `count` runs starts and how long it is, the starts being
// 16-bit fields `stride` bytes apart, the last run ending at `end`. Reads
// two neighbouring rows.
static void run_of(const uint8_t *starts, size_t stride, uint32_t i,
                   uint32_t count, uint32_t end, uint32_t *first,
                   uint32_t *length) {
  *first = 0;
  *length = 0;
  if (i >= count) {
    return;
  }
  uint32_t start = row16(starts + i * stride);
  if (i + 1 < count) {
    end = row16(starts + (i + 1) * stride);
  }
  *first = start;
  *length = end - start;
}

uint32_t library_bytes(const library_t *lib) {
  return lib != NULL ? FIELD(lib, total) : 0;
}

uint32_t library_track_count(const library_t *lib) {
  return lib != NULL ? FIELD(lib, n_tracks) : 0;
}

uint32_t library_artist_count(const library_t *lib) {
  return lib != NULL ? FIELD(lib, n_artists) : 0;
}

uint32_t library_album_count(const library_t *lib) {
  return lib != NULL ? FIELD(lib, n_albums) : 0;
}

// One artist row; the name is read when the caller reads it.
const char *library_artist_name(const library_t *lib, uint32_t artist) {
  if (artist >= library_artist_count(lib)) {
    return EMPTY;
  }
  return pool_text(lib, row32(table(lib, FIELD(lib, off_artists)) +
                              artist * sizeof(artist_row_t)));
}

// Two neighbouring artist rows.
void library_artist_albums(const library_t *lib, uint32_t artist,
                           uint32_t *first_album, uint32_t *count) {
  uint32_t n = library_artist_count(lib);
  run_of(n != 0 ? table(lib, FIELD(lib, off_artists)) +
                      offsetof(artist_row_t, first_album)
                : NULL,
         sizeof(artist_row_t), artist, n, library_album_count(lib), first_album,
         count);
}

// Two neighbouring artist rows.
void library_artist_tracks(const library_t *lib, uint32_t artist,
                           uint32_t *first_track, uint32_t *count) {
  uint32_t n = library_artist_count(lib);
  run_of(n != 0 ? table(lib, FIELD(lib, off_artists)) +
                      offsetof(artist_row_t, first_track)
                : NULL,
         sizeof(artist_row_t), artist, n, library_track_count(lib), first_track,
         count);
}

// One album row.
const char *library_album_name(const library_t *lib, uint32_t album) {
  if (album >= library_album_count(lib)) {
    return EMPTY;
  }
  return pool_text(lib, row32(table(lib, FIELD(lib, off_albums)) +
                              album * sizeof(album_row_t)));
}

// One album row.
uint32_t library_album_artist(const library_t *lib, uint32_t album) {
  if (album >= library_album_count(lib)) {
    return NO_NUMBER;
  }
  return row16(table(lib, FIELD(lib, off_albums)) +
               album * sizeof(album_row_t) + offsetof(album_row_t, artist));
}

// Two neighbouring album rows.
void library_album_tracks(const library_t *lib, uint32_t album,
                          uint32_t *first_track, uint32_t *count) {
  uint32_t n = library_album_count(lib);
  run_of(n != 0 ? table(lib, FIELD(lib, off_albums)) +
                      offsetof(album_row_t, first_track)
                : NULL,
         sizeof(album_row_t), album, n, library_track_count(lib), first_track,
         count);
}

// One 16-bit entry of the album order.
uint32_t library_album_by_name(const library_t *lib, uint32_t n) {
  if (n >= library_album_count(lib)) {
    return NO_NUMBER;
  }
  return row16(table(lib, FIELD(lib, off_album_names)) + 2 * n);
}

// One 16-bit entry of the song order.
uint32_t library_song_by_title(const library_t *lib, uint32_t n) {
  if (n >= library_track_count(lib)) {
    return NO_NUMBER;
  }
  return row16(table(lib, FIELD(lib, off_songs)) + 2 * n);
}

// The track's row, its format byte, and its album's row for the artist.
bool library_track(const library_t *lib, uint32_t track, library_track_t *out) {
  if (out == NULL) {
    return false;
  }
  if (track >= library_track_count(lib)) {
    memset(out, 0, sizeof(*out));
    out->title = EMPTY;
    out->artist = EMPTY;
    out->album = NO_NUMBER;
    out->group_artist = NO_NUMBER;
    return false;
  }
  track_row_t row;
  LOAD_ROW(row, table(lib, FIELD(lib, off_tracks)) + track * sizeof(row));
  out->title = pool_text(lib, row.title);
  out->artist = pool_text(lib, row.artist);
  out->album = row.album;
  out->group_artist = library_album_artist(lib, row.album);
  out->track_no = row.track_no;
  out->disc_no = row.disc_no;
  out->duration_ms = row.duration_ms;
  out->format = *(table(lib, FIELD(lib, off_formats)) + track);
  return true;
}

// The track's row, its folder's row, and the two strings.
size_t library_track_path(const library_t *lib, uint32_t track, char *out,
                          size_t out_size) {
  if (out == NULL) {
    out_size = 0;
  }
  if (out_size > 0) {
    out[0] = '\0';
  }
  if (track >= library_track_count(lib)) {
    return 0;
  }
  track_row_t row;
  LOAD_ROW(row, table(lib, FIELD(lib, off_tracks)) + track * sizeof(row));
  const char *dir = pool_text(lib, row32(table(lib, FIELD(lib, off_dirs)) +
                                         row.dir * sizeof(dir_row_t)));
  const char *name = pool_text(lib, row.name);
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  if (out_size > 0) {
    size_t room = out_size - 1;
    size_t from_dir = dir_len < room ? dir_len : room;
    size_t from_name = name_len < room - from_dir ? name_len : room - from_dir;
    memcpy(out, dir, from_dir);
    memcpy(out + from_dir, name, from_name);
    out[from_dir + from_name] = '\0';
  }
  return dir_len + name_len;
}

typedef int (*text_cmp_fn)(const uint8_t *a, size_t an, const uint8_t *b);

// The first folder row that is not before the folder `dir` (dir_len bytes)
// by `cmp`; the number of folders if there is none. A binary search: each
// step reads a row and compares its string.
static uint32_t first_dir_not_before(const library_t *lib, const uint8_t *dir,
                                     size_t dir_len, text_cmp_fn cmp) {
  const uint8_t *dirs = table(lib, FIELD(lib, off_dirs));
  uint32_t lo = 0;
  uint32_t hi = FIELD(lib, n_dirs);
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const char *have = pool_text(lib, row32(dirs + mid * sizeof(dir_row_t)));
    if (cmp(dir, dir_len, (const uint8_t *)have) <= 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

static bool dir_is(const library_t *lib, uint32_t dir, const uint8_t *want,
                   size_t want_len, text_cmp_fn cmp) {
  const uint8_t *row =
      table(lib, FIELD(lib, off_dirs)) + dir * sizeof(dir_row_t);
  return cmp(want, want_len, (const uint8_t *)pool_text(lib, row32(row))) == 0;
}

// The first track of folder `dir`, in path order, whose file name is `name`
// by `cmp`. A binary search of the folder's run of the path order: each
// step reads an entry, that track's row and its file name.
static bool find_in_dir(const library_t *lib, uint32_t dir, const uint8_t *name,
                        text_cmp_fn cmp, uint32_t *track) {
  const uint8_t *paths = table(lib, FIELD(lib, off_paths));
  const uint8_t *tracks = table(lib, FIELD(lib, off_tracks));
  uint32_t first, count;
  run_of(table(lib, FIELD(lib, off_dirs)) + offsetof(dir_row_t, first),
         sizeof(dir_row_t), dir, FIELD(lib, n_dirs), library_track_count(lib),
         &first, &count);
  uint32_t lo = first;
  uint32_t hi = first + count;
  uint32_t t = 0;
  int c = 1; // how `name` compares with the entry at `hi`
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t t_mid = row16(paths + 2 * mid);
    const char *have =
        pool_text(lib, row32(tracks + t_mid * sizeof(track_row_t) +
                             offsetof(track_row_t, name)));
    int c_mid = cmp(name, TO_NUL, (const uint8_t *)have);
    if (c_mid <= 0) {
      hi = mid;
      t = t_mid;
      c = c_mid;
    } else {
      lo = mid + 1;
    }
  }
  if (c != 0) {
    return false;
  }
  *track = t;
  return true;
}

// First the path as written: a binary search for the folder, then one for
// the file name in that folder's run. About log2(folders) + log2(files in
// the folder) steps: 9 + 4 for 5,000 tracks in 500 folders. That finds
// every path spelled as it is on the card, and is where an exact match gets
// its preference.
//
// If there is none, the same two searches ignoring the case of ASCII
// letters. Text that differs only in case is together in the order
// everything is sorted by, so the first search lands on the first folder
// that differs from the one asked for only in case, and the second on the
// first such file name in it: the first match in path order. On the card
// (FAT keeps no two names that differ only in case) there is one such
// folder; on a file system that has several, each is searched in turn until
// one holds the file.
bool library_find_path(const library_t *lib, const char *path,
                       uint32_t *track) {
  if (path == NULL || library_track_count(lib) == 0) {
    return false;
  }
  size_t dir_len = strlen(path);
  while (dir_len > 0 && path[dir_len - 1] != '/') {
    dir_len--;
  }
  const uint8_t *dir = (const uint8_t *)path;
  const uint8_t *name = dir + dir_len;
  uint32_t n_dirs = FIELD(lib, n_dirs);
  uint32_t found = 0;

  uint32_t d = first_dir_not_before(lib, dir, dir_len, text_cmp);
  bool ok = d < n_dirs && dir_is(lib, d, dir, dir_len, text_cmp) &&
            find_in_dir(lib, d, name, text_cmp, &found);
  if (!ok) {
    for (d = first_dir_not_before(lib, dir, dir_len, folded_cmp);
         !ok && d < n_dirs && dir_is(lib, d, dir, dir_len, folded_cmp); d++) {
      ok = find_in_dir(lib, d, name, folded_cmp, &found);
    }
  }
  if (ok && track != NULL) {
    *track = found;
  }
  return ok;
}
