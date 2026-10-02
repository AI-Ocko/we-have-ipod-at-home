#pragma once

#include "library.h"
#include <stdbool.h>
#include <stdint.h>

// The library scan: find every music file under a folder and hand it, with
// its tags, to the index builder. How it walks the tree, and what each file
// costs in card operations, is described at the top of libscan.c.

typedef struct {
  uint32_t tracks;     // added to the index
  uint32_t unreadable; // audio files that could not be opened or read
  uint32_t skipped;    // paths too long or folders too deep
  bool full;           // stopped because the index was full
} libscan_stats_t;

// Called after each track is added. Return false to abandon the scan.
typedef bool (*libscan_progress_fn)(void *ctx, const libscan_stats_t *stats);

#define LIBSCAN_MAX_DEPTH 8  // folders below the root
#define LIBSCAN_PATH_MAX 256 // bytes, including the terminator

// Walk the tree under `root`, read each .flac and .mp3 file's tags, and add
// it to `builder` with its path relative to `root`. Skips folders whose names
// start with '.' and "System Volume Information". Returns false if `root`
// cannot be opened or the scan was abandoned.
//
// Files whose names start with '.' are passed over too, as if they were not
// there: they are not opened and not counted. (macOS writes a "._Name.flac"
// beside every "Name.flac" it copies to a FAT card; it has the extension
// and is not music.)
//
// What goes into the index:
// - The path is relative to `root`, with '/' between its parts and none at
//   the start: `root`, a '/' and the path open the file. LIBSCAN_PATH_MAX
//   is the limit on that relative path, so a buffer of strlen(root) + 1 +
//   LIBSCAN_PATH_MAX bytes holds any file the index names. A file or folder
//   whose path would be longer is left out and counted in `skipped`; a path
//   is never cut short. `root` itself may be at most LIBSCAN_PATH_MAX - 1
//   bytes (a longer one "cannot be opened").
// - The title is the tag's, converted for the LCD, or the file name without
//   its extension if there is none (translit_title's rule). The artist and
//   album are the tags', converted, or "Unknown Artist" / "Unknown Album" if
//   the file has none or only spaces. The artist the album is filed under is
//   the album artist if the file names one, otherwise the artist.
// - A file with a music extension whose contents are not that format (text,
//   nothing at all) is still added, under its file name and the Unknown
//   names: it is on the card under a music name, so it is listed, and
//   playing it gives the player's ordinary error. A file that cannot be
//   opened, or whose reading fails with an I/O error, is counted in
//   `unreadable` and left out.
//
// What is counted in `skipped`: each file whose path is too long; each
// folder whose path is too long, that lies more than LIBSCAN_MAX_DEPTH
// folders below `root`, or that cannot be opened (once per folder, whatever
// is inside it).
//
// When the builder reports LIBRARY_FULL the scan stops, `stats->full` is set
// and the result is true: what was added is intact and can be finished.
//
// `progress` may be NULL. `stats` may be NULL; otherwise it is zeroed at the
// start, kept up to date throughout (it is what `progress` is shown) and
// valid whatever the result. Every folder and file opened here is closed
// again before this returns, however it ends.
//
// Nothing here is static, so two scans may run at once. One uses about
// 2.7 KB of stack of its own (the path, one file's tags, their converted
// text and the file's buffer), plus what tags_read needs (1.2 KB) and
// whatever the C library and the file system use beneath. On the device a
// whole scan, saving and logging included, was measured to use 5.0 KB of
// its task's stack.
bool libscan_run(const char *root, library_builder_t *builder,
                 libscan_progress_fn progress, void *ctx,
                 libscan_stats_t *stats);
