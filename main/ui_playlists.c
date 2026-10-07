#include "esp_log.h"
#include "esp_random.h"
#include "library_task.h"
#include "storage.h"
#include "ui_state.h"
#include <inttypes.h>
#include <string.h>
#include <strings.h>

// The playlist side of the UI: the Playlists screen and a playlist's
// entries (what browse shows of them), the picker ("Add to playlist"), the
// delete confirmation, the options menu's playlist actions, and what the
// playlist task's answers do. Part of the UI task: called from its loop
// (ui.c), and it changes ui->pl and the parts of ui_t named below. Nothing
// here touches the card: every reading and writing of /playlists/ is a
// request to the playlist task (playlist_task.h), which answers with an
// event that the loop takes from its queue set like a key.
//
// WHAT IS SHOWN
//
// browse (browse.h) draws the playlist screens from ui->pl->view: the
// catalog (the newest listing's copy) and the open playlist's entries. Both
// are copies the UI owns; the playlist task's results are copied into them
// when their events arrive (playlist_task_copy_catalog / _copy_load), once
// per answer, in PSRAM.
//
//   - Entering the Playlists screen (or coming back to it from a playlist)
//     asks for a listing. Until the first one ever arrives there is no
//     catalog and the screen says "Loading" (ui.c draws that from
//     ui_pl_loading); after that, the copy in hand is shown at once and
//     replaced when the new listing comes.
//   - Opening a playlist asks for its entries. The playlist is known by its
//     FILE NAME (open_file), not by its row: a listing can move it. Every
//     listing finds it again (find_file) and renumbers the playlist level;
//     a listing that no longer has it goes back to the Playlists screen.
//     Every listing also keeps the Playlists screen's cursor on the
//     playlist it was on, found by file name; after a create or a rename,
//     on the playlist just made (`follow`).
//
// REQUESTS AND ANSWERS
//
// A request call returns its number at once, or 0 if the task's queue had
// no room: the UI then says "Busy" and nothing changes. Each answer is
// matched to what waits for it by number:
//
//   list_request   the listing the screens or a name check wait for; an
//                  older listing's answer is ignored (the newer one follows
//                  it in the queue)
//   load_request   the open playlist's entries
//   play_request   "Play"/"Shuffle play" on a playlist row: its entries,
//                  then the queue is loaded from them and they play
//   edits[]        everything that changes a file (EDITS below)
//
// A result that came for a number no longer waited for is dropped. A copy
// that answers false (a newer result of the same kind has replaced it, see
// playlist_task.h) is asked for again, so a load in hand is never lost.
//
// EDITS
//
// Create, append, rename, delete, move and remove are sent and noted in
// edits[] (number, kind, flags, the open playlist's serial); their answer
// says how it went, on the bottom row for 1.5 s ("Saved", "Added",
// "Renamed", "Deleted", or what went wrong). After a create, a rename or a
// delete the folder is listed again.
//
// A move or a remove changes the UI's copy of the entries at once, as soon
// as the request is accepted, so that ten Move ups pressed in a row are ten
// moves on the screen without waiting for the card. Should one fail, the
// playlist is loaded again from the card: what it holds is the truth. Until
// that load's entries are in hand the copy's entry numbers may name other
// entries in the file, so moves and removes are refused ("Busy"); if the
// task refused the load itself, it is asked for again after every key and
// event (ui_pl_retry_reload) until it is taken. A load
// that was served before edits the UI has sent since would undo them on
// the screen, so it is not used: it is asked for again (one more load, after
// those edits). Request numbers of one caller increase (playlist_task.c),
// which is what "since" compares. A playlist with more than 1,000 entries is
// loaded again after every move or remove too, since the UI's copy lacks the
// entries past 1,000 that such an edit can bring in.
//
// THE INDEX
//
// Track numbers mean something only within one index. The tracks to add
// (the picker's, "New...", "Save queue") are made when the picker or the
// text entry opens, inside one take of the index, and carry its checksum;
// a load says which index its entries are of. When the index goes (a scan
// begins) or is replaced, ui_pl_index_changed closes the picker, drops a
// pending play and cancels a text entry that holds tracks ("Try again"), so
// no old track number reaches a request; the playlist task's checksum check
// is the second line, not the first. Entries are played only if the load's
// checksum is the index's.
//
// MEMORY
//
// ui_pl_t is one block of PSRAM, allocated once by ui_start: the catalog
// copy (16,796 bytes), two loads (the open playlist's and a play's, 2,012
// each), the tracks to add (2,000), three file names (256 each) and small
// state; about 23.8 KB (sizeof is logged at start-up). Nothing here
// allocates.

static const char *TAG = "dap";

static const char ROOT[] = STORAGE_MOUNT_POINT "/";
#define ROOT_LEN (sizeof(ROOT) - 1)

static const char NEW_ROW[] = "New...";

// --- names -------------------------------------------------------------------

ui_overlay_t ui_name_overlay(playlist_name_t why) {
  switch (why) {
  case PLAYLIST_NAME_EMPTY:
    return UI_OVERLAY_NAME_EMPTY;
  case PLAYLIST_NAME_TOO_LONG:
    return UI_OVERLAY_NAME_TOO_LONG;
  case PLAYLIST_NAME_BAD_CHAR:
    return UI_OVERLAY_BAD_CHARACTER;
  case PLAYLIST_NAME_BAD_END:
    return UI_OVERLAY_BAD_END;
  case PLAYLIST_NAME_BAD_START:
    return UI_OVERLAY_BAD_START;
  case PLAYLIST_NAME_RESERVED:
    return UI_OVERLAY_NAME_RESERVED;
  case PLAYLIST_NAME_OK:
    break;
  }
  return UI_OVERLAY_BAD_CHARACTER; // not given for OK
}

// The name of a playlist's file: without its .m3u or .m3u8 ending (any
// case), as the catalog shows it.
static void name_of_file(const char *file, char *out, size_t size) {
  strlcpy(out, file, size);
  size_t len = strlen(out);
  if (len >= 5 && strcasecmp(out + len - 5, ".m3u8") == 0) {
    out[len - 5] = '\0';
  } else if (len >= 4 && strcasecmp(out + len - 4, ".m3u") == 0) {
    out[len - 4] = '\0';
  }
}

// Which playlist of the catalog is the file `file`, exactly. The catalog
// finds the first playlist with the same name, ignoring case and ending, in
// O(log n); files whose names differ only so are next to it, and are few.
static bool find_file(const playlist_catalog_t *c, const char *file,
                      uint32_t *at) {
  char name[PLAYLIST_FILE_MAX + 1];
  name_of_file(file, name, sizeof(name));
  uint32_t i;
  if (!playlist_catalog_find(c, name, &i)) {
    return false;
  }
  char other[PLAYLIST_FILE_MAX + 1];
  for (; i < c->count; i++) {
    if (strcmp(playlist_catalog_file(c, i), file) == 0) {
      *at = i;
      return true;
    }
    playlist_catalog_name(c, i, other, sizeof(other));
    if (strcasecmp(other, name) != 0) {
      break; // past the run of that name
    }
  }
  return false;
}

// --- what browse is shown ----------------------------------------------------

static void update_view(ui_pl_t *pl) {
  pl->view.catalog = pl->catalog_ready ? &pl->catalog : NULL;
  pl->view.tracks = pl->open_loaded ? pl->open.tracks : NULL;
  pl->view.count = pl->open_loaded ? pl->open.count : 0;
}

void ui_pl_init(ui_t *ui, ui_pl_t *pl) {
  ui->pl = pl;
  update_view(pl);
  browse_set_playlists(&ui->browse, &pl->view);
}

// The level on the stack that is `screen`, or NULL. A playlist's level is
// only ever the top one (nothing opens from it), the Playlists level the
// one under it or the top.
static browse_level_t *level_of(ui_t *ui, browse_screen_t screen) {
  for (int d = ui->browse.depth; d >= 0; d--) {
    if (ui->browse.levels[d].screen == screen) {
      return &ui->browse.levels[d];
    }
  }
  return NULL;
}

bool ui_pl_loading(const ui_t *ui) {
  const browse_level_t *level = &ui->browse.levels[ui->browse.depth];
  if (level->screen == BROWSE_PLAYLISTS) {
    return !ui->pl->catalog_ready;
  }
  if (level->screen == BROWSE_PLAYLIST) {
    return !ui->pl->open_loaded;
  }
  return false;
}

static void say(ui_t *ui, ui_overlay_t what, int64_t now_us) {
  ui_overlay_show(ui, what, now_us);
}

// What an answer that is not DONE says.
static void say_outcome(ui_t *ui, const playlist_event_t *ev, int64_t now_us) {
  switch ((playlist_outcome_t)ev->outcome) {
  case PLAYLIST_TASK_NAME_IN_USE:
    say(ui, UI_OVERLAY_NAME_IN_USE, now_us);
    break;
  case PLAYLIST_TASK_BAD_NAME:
    say(ui, ui_name_overlay((playlist_name_t)ev->name), now_us);
    break;
  case PLAYLIST_TASK_FULL:
    say(ui, UI_OVERLAY_PLAYLIST_FULL, now_us);
    break;
  case PLAYLIST_TASK_BAD_FILE:
    say(ui, UI_OVERLAY_BAD_FILE, now_us);
    break;
  case PLAYLIST_TASK_CARD_ERROR:
    say(ui, UI_OVERLAY_CARD_ERROR, now_us);
    break;
  case PLAYLIST_TASK_LIBRARY_BUSY:
    say(ui, UI_OVERLAY_LIBRARY_BUSY, now_us);
    break;
  case PLAYLIST_TASK_STALE_INDEX:
    say(ui, UI_OVERLAY_TRY_AGAIN, now_us);
    break;
  case PLAYLIST_TASK_NO_ENTRY:
    // The playlist is not there any more (a computer or the console took
    // it), or an entry moved past an end: the listing or the load that
    // follows shows what is there. Nothing to say here (but on_edit says
    // "Not found" for an append or a create, which nothing would show).
  case PLAYLIST_TASK_DONE:
    break;
  }
}

// --- requests ----------------------------------------------------------------

// A listing that failed (or could not be asked for) before any listing has
// arrived: the Playlists screen shows why in place of "Loading", and 5 or 6
// asks again (ui_pl_retry_listing), so it is never stuck.
static void listing_failed(ui_t *ui, ui_overlay_t why, int64_t now_us) {
  say(ui, why, now_us);
  if (!ui->pl->catalog_ready) {
    ui->pl->list_failed = true;
    ui->pl->list_why = why;
    ui->list_dirty = true;
  }
}

void ui_pl_list(ui_t *ui, int64_t now_us) {
  uint32_t request = playlist_task_list(PLAYLIST_CLIENT_UI);
  if (request == 0) {
    listing_failed(ui, UI_OVERLAY_BUSY, now_us);
    return;
  }
  ui->pl->list_request = request;
  ui->pl->list_failed = false; // "Loading" again until it comes
  ui->list_dirty = true;
}

bool ui_pl_retry_listing(ui_t *ui, int64_t now_us) {
  const browse_level_t *level = &ui->browse.levels[ui->browse.depth];
  if (level->screen != BROWSE_PLAYLISTS || ui->pl->catalog_ready ||
      !ui->pl->list_failed) {
    return false;
  }
  ui_pl_list(ui, now_us);
  return true;
}

// Ask for the open playlist's entries. False if refused.
static bool send_load(ui_t *ui) {
  ui_pl_t *pl = ui->pl;
  pl->load_request = playlist_task_load(PLAYLIST_CLIENT_UI, pl->open_file);
  return pl->load_request != 0;
}

// The open playlist again, from the card: its entries replace the copy
// when they come.
static void reload(ui_t *ui, int64_t now_us) {
  if (ui->pl->open_file[0] != '\0' && !send_load(ui)) {
    say(ui, UI_OVERLAY_BUSY, now_us);
  }
}

// The copy is known to differ from the file (an edit failed) and the load
// that puts it right was refused: ask again, quietly.
void ui_pl_retry_reload(ui_t *ui) {
  ui_pl_t *pl = ui->pl;
  if (pl->reload_failed && pl->load_request == 0 && pl->open_file[0] != '\0') {
    send_load(ui);
  }
}

// Whether a move or a remove must wait: the copy is known to differ from the
// file, so its entry numbers may name other entries there. "Busy", and the
// load asked for again if it was refused.
static bool copy_wrong(ui_t *ui, int64_t now_us) {
  if (!ui->pl->reload_failed) {
    return false;
  }
  ui_pl_retry_reload(ui);
  say(ui, UI_OVERLAY_BUSY, now_us);
  return true;
}

// A free slot for an edit, or NULL (none: the edit is refused as "Busy";
// the playlist task's queue would be nearly as full).
static ui_edit_t *edit_slot(ui_t *ui) {
  for (uint32_t i = 0; i < UI_EDITS_MAX; i++) {
    if (ui->pl->edits[i].request == 0) {
      return &ui->pl->edits[i];
    }
  }
  return NULL;
}

static void edit_note(ui_t *ui, ui_edit_t *slot, uint32_t request,
                      playlist_request_t kind, uint8_t flags) {
  slot->request = request;
  slot->kind = (uint8_t)kind;
  slot->flags = flags;
  slot->serial = ui->pl->open_serial;
}

// Whether an edit of the open playlist was sent after request `request`.
static bool edited_since(const ui_t *ui, uint32_t request) {
  const ui_pl_t *pl = ui->pl;
  for (uint32_t i = 0; i < UI_EDITS_MAX; i++) {
    const ui_edit_t *e = &pl->edits[i];
    if (e->request > request && (e->flags & UI_EDIT_ON_OPEN) &&
        e->serial == pl->open_serial) {
      return true;
    }
  }
  return false;
}

// --- the open playlist -------------------------------------------------------

static void close_open(ui_t *ui) {
  ui_pl_t *pl = ui->pl;
  pl->open_file[0] = '\0';
  pl->open_loaded = false;
  pl->load_request = 0;
  pl->reload_failed = false;
  pl->open_serial++;
  update_view(pl);
}

// Leave the open playlist for the Playlists screen: it has gone, or cannot
// be read. Options open on one of its entries go with it.
static void leave_open(ui_t *ui) {
  if (ui->layer == UI_LAYER_OPTIONS &&
      ui->options_target.kind == BROWSE_TARGET_ENTRY) {
    ui_options_close(ui);
  }
  if (ui->browse.levels[ui->browse.depth].screen == BROWSE_PLAYLIST) {
    browse_back(&ui->browse);
  }
  close_open(ui);
  ui->list_dirty = true;
  ui->list_restart = true;
}

void ui_pl_after_list_key(ui_t *ui, int depth_before, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  if (pl->open_file[0] != '\0' && level_of(ui, BROWSE_PLAYLIST) == NULL) {
    close_open(ui); // gone back from it
  }
  // Entering the Playlists screen asks for a listing, and so does coming
  // back to it from a playlist: a computer or the console may have changed
  // the folder meanwhile.
  if (ui->browse.depth != depth_before &&
      ui->browse.levels[ui->browse.depth].screen == BROWSE_PLAYLISTS) {
    ui_pl_list(ui, now_us);
  }
}

void ui_pl_open_playlist(ui_t *ui, uint32_t index, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  const char *file = playlist_catalog_file(pl->view.catalog, index);
  close_open(ui);
  if (file[0] == '\0') {
    browse_back(&ui->browse); // not expected: browse opened a row it has
    return;
  }
  strlcpy(pl->open_file, file, sizeof(pl->open_file));
  pl->view.playlist = index;
  if (!send_load(ui)) {
    say(ui, UI_OVERLAY_BUSY, now_us);
    browse_back(&ui->browse);
    close_open(ui);
  }
}

bool ui_pl_play_entries(ui_t *ui, const library_t *lib, uint32_t entry,
                        int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  if (lib == NULL || !pl->open_loaded) {
    return false;
  }
  if (pl->open.index != library_checksum(lib)) {
    // Entries of an index that has gone (not expected: a new index closes
    // the playlist). Read them again against this one.
    say(ui, UI_OVERLAY_TRY_AGAIN, now_us);
    reload(ui, now_us);
    return false;
  }
  if (entry >= pl->open.count ||
      pl->open.tracks[entry] >= library_track_count(lib)) {
    say(ui, UI_OVERLAY_FILE_MISSING, now_us);
    return false;
  }
  return ui_play_load_tracks(ui, lib, pl->open.tracks, pl->open.count, entry,
                             false, now_us);
}

// Move entry `entry` of the open playlist by `step` (-1 up, +1 down), in
// the file and in the copy. The options menu stays open on it.
static void move_entry(ui_t *ui, int step, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  uint32_t entry = ui->options_target.index;
  uint32_t count = pl->open.count;
  bool cut = pl->open.truncated;
  // A playlist cut at 1,000 has entries past the copy: the menu can be on
  // one of them (moved down from the last entry shown), and a move there
  // is the file's to make.
  if (!pl->open_loaded || (entry >= count && !cut) ||
      copy_wrong(ui, now_us)) {
    return;
  }
  // At either end the move does nothing.
  if ((step < 0 && entry == 0) || (step > 0 && entry + 1 >= count && !cut)) {
    return;
  }
  ui_edit_t *slot = edit_slot(ui);
  uint32_t request = 0;
  if (slot != NULL) {
    request = step < 0 ? playlist_task_move_up(PLAYLIST_CLIENT_UI,
                                               pl->open_file, entry)
                       : playlist_task_move_down(PLAYLIST_CLIENT_UI,
                                                 pl->open_file, entry);
  }
  if (request == 0) {
    say(ui, UI_OVERLAY_BUSY, now_us);
    return;
  }
  uint8_t flags = UI_EDIT_ON_OPEN;
  if (pl->open.truncated) {
    flags |= UI_EDIT_RELOAD;
  }
  edit_note(ui, slot, request,
            step < 0 ? PLAYLIST_REQUEST_MOVE_UP : PLAYLIST_REQUEST_MOVE_DOWN,
            flags);
  uint32_t other = step < 0 ? entry - 1 : entry + 1;
  if (entry < count && other < count) {
    uint16_t track = pl->open.tracks[entry];
    pl->open.tracks[entry] = pl->open.tracks[other];
    pl->open.tracks[other] = track;
  }
  // Otherwise one of the two is past the copy, and the reload that follows
  // every edit of a cut playlist (UI_EDIT_RELOAD) shows them.
  // The menu stays on the moved entry, and the list under it follows (as
  // far as the copy goes).
  ui->options_target.index = other;
  browse_menu(&ui->browse)->cursor = other < count ? other : count - 1;
  ui->list_dirty = true;
  ui->list_restart = true;
}

static void remove_entry(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  uint32_t entry = ui->options_target.index;
  if (!pl->open_loaded || (entry >= pl->open.count && !pl->open.truncated) ||
      copy_wrong(ui, now_us)) {
    return;
  }
  ui_edit_t *slot = edit_slot(ui);
  uint32_t request =
      slot != NULL
          ? playlist_task_remove(PLAYLIST_CLIENT_UI, pl->open_file, entry)
          : 0;
  if (request == 0) {
    say(ui, UI_OVERLAY_BUSY, now_us);
    return;
  }
  edit_note(ui, slot, request, PLAYLIST_REQUEST_REMOVE,
            UI_EDIT_ON_OPEN | (pl->open.truncated ? UI_EDIT_RELOAD : 0));
  // One memmove of the entries after it: O(n), at most 2 KB. (An entry past
  // a cut playlist's copy is not in it: the reload shows the change.)
  if (entry < pl->open.count) {
    memmove(&pl->open.tracks[entry], &pl->open.tracks[entry + 1],
            (pl->open.count - entry - 1) * sizeof(uint16_t));
    pl->open.count--;
    update_view(pl);
  }
  // The cursor stays at the same position, or on the new last entry: the
  // count is put right by browse_refresh, which keeps the cursor if it can.
  ui_options_close(ui);
}

// --- options -----------------------------------------------------------------

void ui_pl_options_opened(ui_t *ui) {
  ui_pl_t *pl = ui->pl;
  if (ui->options_target.kind == BROWSE_TARGET_PLAYLIST) {
    const char *file =
        playlist_catalog_file(pl->view.catalog, ui->options_target.index);
    strlcpy(pl->target_file, file, sizeof(pl->target_file));
    name_of_file(file, pl->target_name, sizeof(pl->target_name));
  }
}

// The tracks the picker will add: the options target's, as numbers of
// `lib`. False, with the reason shown, if there are none to add or too many.
static bool gather(ui_t *ui, const library_t *lib, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  const browse_target_t *target = &ui->options_target;
  uint32_t n = 0;
  switch (target->kind) {
  case BROWSE_TARGET_TRACK: {
    uint32_t track = tracklist_track(lib, target->list, target->index);
    if (track != UINT32_MAX) {
      pl->tracks[n++] = (uint16_t)track;
    }
    break;
  }
  case BROWSE_TARGET_ALBUM:
  case BROWSE_TARGET_ARTIST: {
    // A contiguous run of the index: one lookup for the count, one O(1)
    // lookup per track.
    uint32_t count = tracklist_count(lib, target->list);
    if (count > PLAYLIST_ENTRIES_MAX) {
      say(ui, UI_OVERLAY_PLAYLIST_FULL, now_us);
      return false;
    }
    for (uint32_t i = 0; i < count; i++) {
      uint32_t track = tracklist_track(lib, target->list, i);
      if (track != UINT32_MAX) {
        pl->tracks[n++] = (uint16_t)track;
      }
    }
    break;
  }
  case BROWSE_TARGET_NOW_PLAYING: {
    // The track on Now Playing, by its path: it may be the console's, which
    // is not in the queue. One binary search.
    uint32_t track;
    const char *path = ui->play.path;
    if (strncmp(path, ROOT, ROOT_LEN) == 0 &&
        library_find_path(lib, path + ROOT_LEN, &track)) {
      pl->tracks[n++] = (uint16_t)track;
    } else {
      say(ui, UI_OVERLAY_NOTHING_PLAYING, now_us);
      return false;
    }
    break;
  }
  default:
    break;
  }
  if (n == 0) {
    return false; // not with a target browse found in this index
  }
  pl->track_count = n;
  pl->tracks_index = library_checksum(lib);
  pl->tracks_cut = false;
  return true;
}

static uint32_t picker_rows(const ui_pl_t *pl) {
  return 1 + (pl->catalog_ready ? pl->catalog.count : 0);
}

static void open_picker(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  ui_options_close(ui);
  ui->layer = UI_LAYER_PICKER;
  menu_reset(&pl->picker_menu, picker_rows(pl));
  pl->picker_since_us = now_us;
  ui_pl_list(ui, now_us); // the copy in hand meanwhile
}

// "Save queue": the queue in the order its tracks were added, the first
// 1,000. Its numbers are the queue's index's.
static void save_queue(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  const play_queue_t *q = &ui->queue;
  if (q->count == 0) {
    say(ui, UI_OVERLAY_QUEUE_EMPTY, now_us);
    return;
  }
  uint32_t n =
      q->count < PLAYLIST_ENTRIES_MAX ? q->count : PLAYLIST_ENTRIES_MAX;
  memcpy(pl->tracks, q->original, n * sizeof(uint16_t));
  pl->track_count = n;
  pl->tracks_index = ui->queue_index;
  pl->tracks_cut = q->count > PLAYLIST_ENTRIES_MAX;
  ui_options_close(ui);
  ui_text_open(ui, UI_TEXT_SAVE_QUEUE, NULL, now_us);
}

bool ui_pl_option(ui_t *ui, const library_t *lib, option_t option,
                  int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  const browse_target_t *target = &ui->options_target;
  switch (option) {
  case OPTION_ADD_TO_PLAYLIST:
    if (gather(ui, lib, now_us)) {
      open_picker(ui, now_us);
    }
    return false;
  case OPTION_SAVE_QUEUE:
    save_queue(ui, now_us);
    return false;
  case OPTION_RENAME:
    ui_options_close(ui);
    ui_text_open(ui, UI_TEXT_RENAME, pl->target_name, now_us);
    return false;
  case OPTION_DELETE:
    ui_options_close(ui);
    ui->layer = UI_LAYER_CONFIRM;
    menu_reset(&pl->confirm_menu, 2); // "No" first
    pl->confirm_since_us = now_us;
    return false;
  case OPTION_MOVE_UP:
  case OPTION_MOVE_DOWN:
    move_entry(ui, option == OPTION_MOVE_UP ? -1 : 1, now_us);
    return false;
  case OPTION_REMOVE:
    remove_entry(ui, now_us);
    return false;
  case OPTION_PLAY:
  case OPTION_SHUFFLE_PLAY:
    if (target->kind == BROWSE_TARGET_ENTRY) {
      return ui_pl_play_entries(ui, lib, target->index, now_us);
    }
    if (target->kind == BROWSE_TARGET_PLAYLIST) {
      // Its entries first; they play when they come (on_load).
      uint32_t request =
          playlist_task_load(PLAYLIST_CLIENT_UI, pl->target_file);
      if (request == 0) {
        say(ui, UI_OVERLAY_BUSY, now_us);
        return false;
      }
      pl->play_request = request;
      pl->play_shuffle = (option == OPTION_SHUFFLE_PLAY);
      ui_options_close(ui);
    }
    return false;
  default:
    return false;
  }
}

// --- the picker and the confirmation -----------------------------------------

void ui_pl_row_name(const ui_t *ui, uint32_t row, char *out, size_t size) {
  if (row == 0) {
    strlcpy(out, NEW_ROW, size);
  } else {
    playlist_catalog_name(ui->pl->view.catalog, row - 1, out, size);
  }
}

// The picker or the confirmation closes. An answer said after this (the
// request's "Busy", or the playlist task's later) is shown as usual.
static void close_layer(ui_t *ui) {
  ui->layer = UI_LAYER_NONE;
  ui->list_dirty = true;
  ui->list_restart = true;
  ui_overlay_end_words(ui);
}

static void picker_choose(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  uint32_t row = pl->picker_menu.cursor;
  close_layer(ui);
  if (row == 0) {
    ui_text_open(ui, UI_TEXT_NEW_TRACKS, NULL, now_us); // with the tracks
    return;
  }
  const char *file = playlist_catalog_file(pl->view.catalog, row - 1);
  ui_edit_t *slot = edit_slot(ui);
  uint32_t request =
      slot != NULL && file[0] != '\0'
          ? playlist_task_append(PLAYLIST_CLIENT_UI, file, pl->tracks,
                                 pl->track_count, pl->tracks_index)
          : 0;
  if (request == 0) {
    say(ui, UI_OVERLAY_BUSY, now_us);
    return;
  }
  // Added to the open playlist (from Now Playing, with it open under the
  // menus): it is loaded again when the append is done.
  bool on_open = strcmp(file, pl->open_file) == 0;
  edit_note(ui, slot, request, PLAYLIST_REQUEST_APPEND,
            on_open ? UI_EDIT_ON_OPEN | UI_EDIT_RELOAD : 0);
}

void ui_pl_picker_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  bool press = (event->type == KEY_PRESS);
  switch (event->key) {
  case '2':
  case '8':
    if (press || event->type == KEY_REPEAT) {
      uint32_t before = pl->picker_menu.cursor;
      menu_move(&pl->picker_menu, event->key == '8' ? 1 : -1, press);
      if (pl->picker_menu.cursor != before) {
        pl->picker_since_us = now_us;
      }
    }
    break;
  case '5':
  case '6':
    if (press) {
      picker_choose(ui, now_us);
    }
    break;
  case '4':
  case '*':
  case '#':
    if (press) {
      close_layer(ui);
    }
    break;
  default:
    break;
  }
}

void ui_pl_confirm_key(ui_t *ui, const key_event_t *event, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  bool press = (event->type == KEY_PRESS);
  switch (event->key) {
  case '2':
  case '8':
    if (press) {
      menu_move(&pl->confirm_menu, 1, true); // two rows: either way
    }
    break;
  case '5':
  case '6':
    if (press) {
      bool yes = pl->confirm_menu.cursor == 1;
      close_layer(ui);
      if (yes) {
        ui_edit_t *slot = edit_slot(ui);
        uint32_t request =
            slot != NULL
                ? playlist_task_delete(PLAYLIST_CLIENT_UI, pl->target_file)
                : 0;
        if (request == 0) {
          say(ui, UI_OVERLAY_BUSY, now_us);
        } else {
          edit_note(ui, slot, request, PLAYLIST_REQUEST_DELETE, 0);
        }
      }
    }
    break;
  case '4':
  case '*':
    if (press) {
      close_layer(ui);
    }
    break;
  default:
    break;
  }
}

// --- names from the text entry -----------------------------------------------

bool ui_pl_send_name(ui_t *ui, const char *name, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  ui_edit_t *slot = edit_slot(ui);
  uint32_t request = 0;
  playlist_request_t kind = PLAYLIST_REQUEST_CREATE;
  uint8_t flags = UI_EDIT_FOLLOW;
  if (slot != NULL) {
    switch (pl->text_purpose) {
    case UI_TEXT_NEW:
      request = playlist_task_create(PLAYLIST_CLIENT_UI, name, NULL, 0, 0);
      break;
    case UI_TEXT_NEW_TRACKS:
    case UI_TEXT_SAVE_QUEUE:
      request = playlist_task_create(PLAYLIST_CLIENT_UI, name, pl->tracks,
                                     pl->track_count, pl->tracks_index);
      if (pl->text_purpose == UI_TEXT_SAVE_QUEUE && pl->tracks_cut) {
        flags |= UI_EDIT_CUT;
      }
      break;
    case UI_TEXT_RENAME:
      kind = PLAYLIST_REQUEST_RENAME;
      request = playlist_task_rename(PLAYLIST_CLIENT_UI, pl->target_file, name);
      break;
    }
  }
  if (request == 0) {
    say(ui, UI_OVERLAY_BUSY, now_us);
    return false;
  }
  edit_note(ui, slot, request, kind, flags);
  strlcpy(pl->follow, name, sizeof(pl->follow)); // see on_listing
  return true;
}

// --- answers -----------------------------------------------------------------

// The cursor of a list of playlists (row 0 being "New playlist" or
// "New..."), as the file name of the row it is on; "" for row 0.
static void row_file(const ui_pl_t *pl, const menu_t *menu, char *out,
                     size_t size) {
  out[0] = '\0';
  if (pl->catalog_ready && menu->cursor >= 1) {
    strlcpy(out, playlist_catalog_file(&pl->catalog, menu->cursor - 1), size);
  }
}

// Put `menu` (a list of playlists after row 0) on the playlist `file`, or
// named `name`, in the new catalog. True if it found it.
static bool put_cursor(const ui_pl_t *pl, menu_t *menu, const char *file,
                       const char *name) {
  uint32_t i;
  menu_set_count(menu, 1 + pl->catalog.count);
  if ((name != NULL && name[0] != '\0' &&
       playlist_catalog_find(&pl->catalog, name, &i)) ||
      (file != NULL && file[0] != '\0' && find_file(&pl->catalog, file, &i))) {
    menu->cursor = i + 1;
    return true;
  }
  return false;
}

static void on_listing(ui_t *ui, const playlist_event_t *ev, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  if (ev->request != pl->list_request) {
    return; // an older listing: the newer one is behind it
  }
  pl->list_request = 0;
  if (ev->outcome != PLAYLIST_TASK_DONE) {
    // A listing ends DONE, CARD_ERROR or LIBRARY_BUSY.
    listing_failed(ui,
                   ev->outcome == PLAYLIST_TASK_LIBRARY_BUSY
                       ? UI_OVERLAY_LIBRARY_BUSY
                       : UI_OVERLAY_CARD_ERROR,
                   now_us);
    return;
  }
  // What the cursors are on, by file name, before the catalog changes.
  browse_level_t *level = level_of(ui, BROWSE_PLAYLISTS);
  char keep[PLAYLIST_FILE_MAX + 1] = "";
  char keep_picked[PLAYLIST_FILE_MAX + 1] = "";
  if (level != NULL) {
    row_file(pl, &level->menu, keep, sizeof(keep));
  }
  if (ui->layer == UI_LAYER_PICKER) {
    row_file(pl, &pl->picker_menu, keep_picked, sizeof(keep_picked));
  }
  if (!playlist_task_copy_catalog(PLAYLIST_CLIENT_UI, ev->request,
                                  &pl->catalog)) {
    // Replaced by a newer listing: not this side's (it waits for one at a
    // time), so the console's cannot be it. Ask again.
    ui_pl_list(ui, now_us);
    return;
  }
  pl->catalog_ready = true;
  pl->list_failed = false;
  update_view(pl);

  // The open playlist, by its file name.
  if (pl->open_file[0] != '\0') {
    uint32_t i;
    browse_level_t *open = level_of(ui, BROWSE_PLAYLIST);
    if (find_file(&pl->catalog, pl->open_file, &i) && open != NULL) {
      open->id = i;
      pl->view.playlist = i;
    } else {
      leave_open(ui);
      level = level_of(ui, BROWSE_PLAYLISTS);
    }
  }
  if (level != NULL) {
    uint32_t before = level->menu.cursor;
    // A listing asked for before the create or rename was done does not
    // have its name yet: only one after it is followed.
    put_cursor(pl, &level->menu, keep, pl->follow_done ? pl->follow : NULL);
    if (level->menu.cursor != before) {
      ui->list_restart = true;
    }
  }
  if (pl->follow_done) {
    pl->follow[0] = '\0';
    pl->follow_done = false;
  }
  if (ui->layer == UI_LAYER_PICKER) {
    uint32_t before = pl->picker_menu.cursor;
    put_cursor(pl, &pl->picker_menu, keep_picked, NULL);
    if (pl->picker_menu.cursor != before) {
      pl->picker_since_us = now_us;
    }
  }
  ui->list_dirty = true;
}

// Play what a "Play" or "Shuffle play" on a playlist row loaded: from the
// first playable entry, or with shuffle on from a random one.
static void play_loaded(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    say(ui, UI_OVERLAY_LIBRARY_BUSY, now_us);
    return;
  }
  bool ok = false;
  if (pl->play.index != library_checksum(lib)) {
    say(ui, UI_OVERLAY_TRY_AGAIN, now_us);
  } else {
    uint32_t tracks = library_track_count(lib);
    uint32_t count = pl->play.count;
    uint32_t playable = 0;
    for (uint32_t i = 0; i < count; i++) {
      playable += pl->play.tracks[i] < tracks;
    }
    if (playable == 0) {
      say(ui, UI_OVERLAY_NOTHING_TO_PLAY, now_us);
    } else {
      // The pick-th playable entry: 0 for Play, at random for Shuffle play
      // (a random 32-bit number scaled to the count: no division).
      uint32_t pick =
          pl->play_shuffle
              ? (uint32_t)(((uint64_t)esp_random() * playable) >> 32)
              : 0;
      uint32_t start = 0;
      for (uint32_t i = 0; i < count; i++) {
        if (pl->play.tracks[i] < tracks && pick-- == 0) {
          start = i;
          break;
        }
      }
      ok = ui_play_load_tracks(ui, lib, pl->play.tracks, count, start,
                               pl->play_shuffle, now_us);
    }
  }
  library_task_put();
  if (ok) {
    ui_close_layer(ui);
    ui_play_start(ui);
    ui_show_now_playing(ui, now_us);
  }
}

static void on_load(ui_t *ui, const playlist_event_t *ev, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  if (ev->request == pl->load_request && pl->open_file[0] != '\0') {
    if (ev->outcome != PLAYLIST_TASK_DONE) {
      pl->load_request = 0;
      say_outcome(ui, ev, now_us);
      leave_open(ui);
      if (ev->outcome == PLAYLIST_TASK_NO_ENTRY) {
        ui_pl_list(ui, now_us); // it has gone: the listing shows that
      }
      return;
    }
    if (edited_since(ui, ev->request)) {
      // Served before edits this side made since: it would undo them on the
      // screen. One more load, served after them.
      reload(ui, now_us);
      return;
    }
    bool again = pl->open_loaded;
    if (!playlist_task_copy_load(PLAYLIST_CLIENT_UI, ev->request, &pl->open)) {
      reload(ui, now_us); // replaced by a play's load: ask again
      return;
    }
    if (pl->open.count > PLAYLIST_ENTRIES_MAX) {
      pl->open.count = PLAYLIST_ENTRIES_MAX; // not expected
    }
    pl->load_request = 0;
    pl->open_loaded = true;
    update_view(pl);
    if (again && pl->reload_failed && ui->layer == UI_LAYER_OPTIONS &&
        ui->options_target.kind == BROWSE_TARGET_ENTRY) {
      // Read again because an edit failed: the entry the menu is about may
      // be elsewhere now. (A reload after this side's own edits that worked,
      // as every edit of a playlist cut at 1,000 has, leaves the menu where
      // it is, on the entry it moved.)
      ui_options_close(ui);
    }
    pl->reload_failed = false;
    ui->list_dirty = true;
    return;
  }
  if (ev->request == pl->play_request) {
    pl->play_request = 0;
    if (ev->outcome != PLAYLIST_TASK_DONE) {
      say_outcome(ui, ev, now_us);
      return;
    }
    if (!playlist_task_copy_load(PLAYLIST_CLIENT_UI, ev->request, &pl->play)) {
      say(ui, UI_OVERLAY_TRY_AGAIN, now_us); // replaced: not expected
      return;
    }
    if (pl->play.count > PLAYLIST_ENTRIES_MAX) {
      pl->play.count = PLAYLIST_ENTRIES_MAX;
    }
    play_loaded(ui, now_us);
  }
}

static void on_edit(ui_t *ui, const playlist_event_t *ev, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  ui_edit_t edit = {0};
  for (uint32_t i = 0; i < UI_EDITS_MAX; i++) {
    if (pl->edits[i].request == ev->request) {
      edit = pl->edits[i];
      pl->edits[i].request = 0;
      break;
    }
  }
  if (edit.request == 0) {
    return; // not one this side noted (not expected)
  }
  bool on_open = (edit.flags & UI_EDIT_ON_OPEN) &&
                 edit.serial == pl->open_serial && pl->open_file[0] != '\0';
  if (ev->outcome == PLAYLIST_TASK_DONE) {
    switch ((playlist_request_t)edit.kind) {
    case PLAYLIST_REQUEST_CREATE:
      say(ui, (edit.flags & UI_EDIT_CUT) ? UI_OVERLAY_SAVED_FIRST_1000
                                         : UI_OVERLAY_SAVED,
          now_us);
      pl->follow_done = (edit.flags & UI_EDIT_FOLLOW) != 0;
      ui_pl_list(ui, now_us);
      break;
    case PLAYLIST_REQUEST_RENAME:
      say(ui, UI_OVERLAY_RENAMED, now_us);
      pl->follow_done = (edit.flags & UI_EDIT_FOLLOW) != 0;
      ui_pl_list(ui, now_us);
      break;
    case PLAYLIST_REQUEST_DELETE:
      say(ui, UI_OVERLAY_DELETED, now_us);
      ui_pl_list(ui, now_us);
      break;
    case PLAYLIST_REQUEST_APPEND:
      say(ui, UI_OVERLAY_ADDED, now_us);
      if (on_open) {
        reload(ui, now_us);
      }
      break;
    default: // a move or a remove: the screen has shown it already
      if (on_open && (edit.flags & UI_EDIT_RELOAD)) {
        reload(ui, now_us);
      }
      break;
    }
    return;
  }
  ESP_LOGW(TAG, "playlist %s: %s",
           playlist_task_request_name((playlist_request_t)edit.kind),
           playlist_task_outcome_name((playlist_outcome_t)ev->outcome));
  say_outcome(ui, ev, now_us);

  switch ((playlist_request_t)edit.kind) {
  case PLAYLIST_REQUEST_MOVE_UP:
  case PLAYLIST_REQUEST_MOVE_DOWN:
  case PLAYLIST_REQUEST_REMOVE:
    if (on_open) {
      // The copy has a change the file has not: read it again. If the task
      // refuses the load, the message stays the edit's ("Card error", not
      // "Busy"): ui_pl_retry_reload asks again after the next key or event,
      // and moves and removes wait for it (copy_wrong).
      pl->reload_failed = true;
      send_load(ui);
    }
    break;
  case PLAYLIST_REQUEST_CREATE:
  case PLAYLIST_REQUEST_RENAME:
  case PLAYLIST_REQUEST_DELETE:
  case PLAYLIST_REQUEST_APPEND:
    // An append (from the picker) to a playlist deleted meanwhile, or a
    // create whose tracks the index no longer has: the screen shows no
    // change that would say so, so it is said. (A rename or a delete of a
    // playlist that has gone is shown by the listing below.)
    if (ev->outcome == PLAYLIST_TASK_NO_ENTRY &&
        (edit.kind == PLAYLIST_REQUEST_APPEND ||
         edit.kind == PLAYLIST_REQUEST_CREATE)) {
      say(ui, UI_OVERLAY_NOT_FOUND, now_us);
    }
    // It has gone, or the folder has a name the catalog in hand did not
    // (made meanwhile, or past the 200 a catalog holds): list it again.
    if (ev->outcome == PLAYLIST_TASK_NO_ENTRY ||
        ev->outcome == PLAYLIST_TASK_NAME_IN_USE) {
      ui_pl_list(ui, now_us);
    }
    break;
  default:
    break;
  }
}

void ui_pl_on_event(ui_t *ui, const playlist_event_t *event, int64_t now_us) {
  switch ((playlist_request_t)event->kind) {
  case PLAYLIST_REQUEST_LIST:
    on_listing(ui, event, now_us);
    break;
  case PLAYLIST_REQUEST_LOAD:
    on_load(ui, event, now_us);
    break;
  case PLAYLIST_REQUEST_READ:
    break; // the console's only
  case PLAYLIST_REQUEST_SPACE:
    break; // About's: ui.c takes it
  default:
    on_edit(ui, event, now_us);
    break;
  }
}

void ui_pl_index_changed(ui_t *ui, int64_t now_us) {
  ui_pl_t *pl = ui->pl;
  switch (ui->layer) {
  case UI_LAYER_TEXT:
    if (pl->text_purpose == UI_TEXT_NEW_TRACKS ||
        pl->text_purpose == UI_TEXT_SAVE_QUEUE) {
      say(ui, UI_OVERLAY_TRY_AGAIN, now_us); // its tracks are the old index's
    }
    ui_text_cancel(ui);
    break;
  case UI_LAYER_PICKER:
  case UI_LAYER_CONFIRM:
    close_layer(ui);
    break;
  case UI_LAYER_OPTIONS:
    ui_options_close(ui);
    break;
  case UI_LAYER_NONE:
    break;
  }
  pl->play_request = 0; // its entries would be the old index's
  pl->track_count = 0;
  // The open playlist's entries are the old index's too. browse has gone
  // back to the main menu (or will, when the new index comes); until then a
  // scan covers the screen.
  if (level_of(ui, BROWSE_PLAYLIST) == NULL && pl->open_file[0] != '\0') {
    close_open(ui);
  }
}
