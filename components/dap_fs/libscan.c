#include "libscan.h"

#include "tags.h"
#include "translit.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Design notes
//
// A scan's time goes on the card, so what counts is the number of card
// operations per file, and the walk is arranged to make that as small as
// the POSIX calls allow:
//
// - A folder is read once, front to back, and stays open while the folders
//   inside it are walked: its entries are taken as they come, a file being
//   read and a folder being entered on the spot. Nothing is listed twice
//   and no position is ever looked up again. That means one open folder per
//   level, at most LIBSCAN_MAX_DEPTH + 1 of them, and one open file. (On
//   the device an open folder is a few hundred bytes of heap and does not
//   use one of the file system's file slots; see library_task.c.)
// - What an entry is comes from the directory entry itself (d_type). `stat`
//   is asked only where the file system does not say, which FAT on the
//   device always does; on the host that is a link or an unusual file
//   system.
// - The name is tested before anything is opened: an entry that is not a
//   folder and has no music extension costs nothing beyond having been
//   listed. Nor does anything whose name starts with a dot.
// - Each music file is opened once, and tags_read fetches one or two of its
//   sectors (see tags.c). It is closed before the next entry is looked at.
// - The file is given a buffer of exactly one sector. tags_read asks for a
//   sector at a time, and with the C library's own buffer (4096 bytes on
//   the device) each such request fetched eight sectors from the card to
//   deliver one. Measured on the board: 3.0 ms per file for the tags with
//   the default buffer, 1.5 ms with this one. The buffer is part of the
//   scan's state, so nothing is allocated per file for it either.
// - One buffer holds the path. Going down a level writes the folder's name
//   after its parent's; coming back up only remembers the shorter length.
//   The index is handed the part after the root.
//
// The walk is a loop with its own short stack of open folders, not a
// function that calls itself: its depth is a number that is checked, and
// its memory is fixed whatever the card holds.
//
// The order in which tracks are found is whatever order the folders list
// their entries in. That does not matter: the index comes out the same for
// the same tracks in any order (see library.c).
//
// Nothing here allocates; what opendir and fopen allocate, closedir and
// fclose give back, and every way out of the loop goes through the code
// that closes whatever is open.

typedef enum {
  KIND_FOLDER,
  KIND_FILE,
  KIND_OTHER, // a device, a pipe: nothing to do with it
  KIND_ASK,   // the directory entry does not say
} kind_t;

typedef struct {
  // The root, a '/', and the path below it.
  char path[LIBSCAN_PATH_MAX + LIBSCAN_PATH_MAX];
  size_t rel; // where in `path` the part below the root starts

  DIR *open[LIBSCAN_MAX_DEPTH + 1]; // the root and each folder on the way
  size_t end[LIBSCAN_MAX_DEPTH + 1]; // length of `path` up to each of them
  int depth;                         // index of the folder being read

  library_builder_t *builder;
  libscan_progress_fn progress;
  void *ctx;
  libscan_stats_t *stats;

  // The buffer of the file being read: one sector, as tags_read reads.
  char io[512];

  // One file's tags, and its text as the index wants it. Converting never
  // makes text longer (see translit.c), so each fits where it came from.
  tags_t tags;
  char title[LIBSCAN_PATH_MAX]; // may be a whole file name
  char artist[TAGS_TEXT_MAX];
  char album[TAGS_TEXT_MAX];
  char group[TAGS_TEXT_MAX];
} scan_t;

typedef enum {
  STEP_ON,      // carry on with the next entry
  STEP_FULL,    // the index is full: stop, keeping what there is
  STEP_ABANDON, // the caller asked to stop, or the builder refused
} step_t;

static kind_t kind_from_entry(const struct dirent *entry) {
#if defined(DT_DIR) && defined(DT_REG)
  if (entry->d_type == DT_DIR) {
    return KIND_FOLDER;
  }
  if (entry->d_type == DT_REG) {
    return KIND_FILE;
  }
#else
  (void)entry;
#endif
  return KIND_ASK;
}

// The slow way, through the path. Something that cannot be asked about (a
// link to nothing) is called a file, so that failing to open it is counted.
static kind_t kind_from_path(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return KIND_FILE;
  }
  if (S_ISDIR(st.st_mode)) {
    return KIND_FOLDER;
  }
  return S_ISREG(st.st_mode) ? KIND_FILE : KIND_OTHER;
}

// Names that are passed over whatever they belong to: anything starting
// with a dot. For folders that covers "." and "..", which some file systems
// list, and the player's own "/.dap"; for files, the "._Name.flac"
// companions macOS writes beside each file it copies to a FAT card, which
// have a music extension and are not music.
static bool name_is_hidden(const char *name) { return name[0] == '.'; }

// A folder that is never entered, besides the hidden ones.
static bool folder_is_skipped(const char *name) {
  return strcasecmp(name, "System Volume Information") == 0;
}

static bool is_blank(const char *text) {
  while (*text == ' ') {
    text++;
  }
  return *text == '\0';
}

// Convert `utf8` for the LCD; `fallback` if that leaves nothing to show.
static void text_or(const char *utf8, const char *fallback, char *out,
                    size_t out_size) {
  translit(utf8, out, out_size);
  if (is_blank(out)) {
    translit(fallback, out, out_size);
  }
}

// Write `name` after the folder being read. False if the path below the
// root would not fit in LIBSCAN_PATH_MAX; the buffer then holds a cut-off
// path that must not be used.
static bool path_append(scan_t *s, const char *name, size_t *len_out) {
  size_t start = s->end[s->depth] + 1; // after the folder and a '/'
  size_t name_len = strlen(name);
  if (start + name_len - s->rel >= LIBSCAN_PATH_MAX) {
    return false;
  }
  s->path[start - 1] = '/';
  memcpy(s->path + start, name, name_len + 1);
  *len_out = start + name_len;
  return true;
}

// Read the file at s->path and add it to the index.
static step_t add_file(scan_t *s, const char *name, tags_format_t format) {
  FILE *f = fopen(s->path, "rb");
  if (f == NULL) {
    s->stats->unreadable++;
    return STEP_ON;
  }
  // If this is refused the file keeps the buffer it has: slower, not wrong.
  setvbuf(f, s->io, _IOFBF, sizeof(s->io));
  bool has_tags = tags_read(f, format, &s->tags);
  bool failed = !has_tags && ferror(f) != 0;
  fclose(f);
  if (failed) {
    // The card did not deliver it. That is not the same as a file that is
    // not music: this one is left out.
    s->stats->unreadable++;
    return STEP_ON;
  }
  if (!has_tags) {
    // Not what its name says (or empty). It is still listed, under its file
    // name; whatever tags_read left half-filled is not trusted.
    memset(&s->tags, 0, sizeof(s->tags));
  }

  translit_title(s->tags.title, name, s->title, sizeof(s->title));
  text_or(s->tags.artist, "Unknown Artist", s->artist, sizeof(s->artist));
  text_or(s->tags.album, "Unknown Album", s->album, sizeof(s->album));
  text_or(s->tags.album_artist, s->artist, s->group, sizeof(s->group));

  const library_track_in_t track = {
      .path = s->path + s->rel,
      .title = s->title,
      .artist = s->artist,
      .album = s->album,
      .group_artist = s->group,
      .track_no = s->tags.track_no,
      .disc_no = s->tags.disc_no,
      .duration_ms = s->tags.duration_ms,
      .format = (uint8_t)format,
  };
  library_result_t result = library_builder_add(s->builder, &track);
  if (result == LIBRARY_FULL) {
    s->stats->full = true;
    return STEP_FULL;
  }
  if (result != LIBRARY_OK) {
    return STEP_ABANDON; // a builder that takes nothing: no use going on
  }
  s->stats->tracks++;
  if (s->progress != NULL && !s->progress(s->ctx, s->stats)) {
    return STEP_ABANDON;
  }
  return STEP_ON;
}

// Deal with one entry of the folder being read.
static step_t visit(scan_t *s, const struct dirent *entry) {
  const char *name = entry->d_name;
  // Most entries are settled by their name alone.
  if (name_is_hidden(name)) {
    return STEP_ON;
  }
  kind_t kind = kind_from_entry(entry);
  tags_format_t format = TAGS_FORMAT_FLAC;
  bool music = tags_format_from_name(name, &format);
  if (kind == KIND_FOLDER ? folder_is_skipped(name)
                          : (kind == KIND_FILE && !music)) {
    return STEP_ON;
  }

  size_t len;
  if (!path_append(s, name, &len)) {
    // Too long to name. What it is cannot be asked without its path, so an
    // entry of unknown kind counts only if its name says it is music.
    if (kind == KIND_FOLDER || music) {
      s->stats->skipped++;
    }
    return STEP_ON;
  }
  if (kind == KIND_ASK) {
    kind = kind_from_path(s->path);
    if (kind == KIND_FOLDER && folder_is_skipped(name)) {
      return STEP_ON;
    }
  }

  if (kind == KIND_FILE) {
    return music ? add_file(s, s->path + len - strlen(name), format) : STEP_ON;
  }
  if (kind != KIND_FOLDER) {
    return STEP_ON;
  }
  if (s->depth >= LIBSCAN_MAX_DEPTH) {
    s->stats->skipped++;
    return STEP_ON;
  }
  DIR *dir = opendir(s->path);
  if (dir == NULL) {
    s->stats->skipped++;
    return STEP_ON;
  }
  s->depth++;
  s->open[s->depth] = dir;
  s->end[s->depth] = len;
  return STEP_ON;
}

bool libscan_run(const char *root, library_builder_t *builder,
                 libscan_progress_fn progress, void *ctx,
                 libscan_stats_t *stats) {
  libscan_stats_t own_stats;
  if (stats == NULL) {
    stats = &own_stats;
  }
  memset(stats, 0, sizeof(*stats));
  if (root == NULL || builder == NULL) {
    return false;
  }
  size_t root_len = strlen(root);
  if (root_len >= LIBSCAN_PATH_MAX) {
    return false;
  }

  scan_t s;
  s.builder = builder;
  s.progress = progress;
  s.ctx = ctx;
  s.stats = stats;
  // "music/" and "music" are the same folder; "/" alone becomes "".
  while (root_len > 0 && root[root_len - 1] == '/') {
    root_len--;
  }
  memcpy(s.path, root, root_len);
  s.path[root_len] = '/';
  s.path[root_len + 1] = '\0';
  s.rel = root_len + 1;

  s.depth = 0;
  s.end[0] = root_len;
  s.open[0] = opendir(root);
  if (s.open[0] == NULL) {
    return false;
  }

  step_t step = STEP_ON;
  while (s.depth >= 0 && step == STEP_ON) {
    const struct dirent *entry = readdir(s.open[s.depth]);
    if (entry == NULL) {
      // The end of this folder (or the card stopped listing it): back to
      // the one it is in, which carries on where it was.
      closedir(s.open[s.depth]);
      s.depth--;
      continue;
    }
    step = visit(&s, entry);
  }
  // Stopped early: close what is still open, innermost first.
  while (s.depth >= 0) {
    closedir(s.open[s.depth]);
    s.depth--;
  }
  return step != STEP_ABANDON;
}
