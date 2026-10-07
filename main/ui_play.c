#include "esp_log.h"
#include "esp_random.h"
#include "library_task.h"
#include "player.h"
#include "player_status.h"
#include "storage.h"
#include "translit.h"
#include "ui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// The track that is playing and the queue it belongs to. Part of the UI
// task: every function here is called from its loop (ui.c) and changes
// ui->play and ui->queue.
//
// THE TRACK
//
// `path` is the track that is playing or, when nothing is, the last one that
// was: the "remembered" track that D starts. `title`, `duration_ms` and
// `elapsed_ms` are what Now Playing shows of it. They are copies, made when
// the track becomes current; nothing here points into the index. A
// remembered track is started from `elapsed_ms`, which is 0 unless the track
// was restored at power-on.
//
// WHICH EVENTS COUNT
//
// Every player event carries the number of the `play` it is about. The UI
// follows one play at a time, `play_id`, and `min_id` marks where old news
// ends:
//
//   number < min_id      an earlier track's event, read late: the track was
//                        replaced or stopped after the player posted it. It
//                        is logged and changes nothing. In particular its
//                        ENDED or ERROR does not start another track.
//   number == play_id    the current track's.
//   number > play_id     a play the UI did not ask for: the console's. The
//                        UI takes it up as the current track; it is not the
//                        queue's.
//
// Starting a track sets both to its number. A stop (for a scan, from the
// console, or Next at the end of the queue) is reported by no event, so
// ui_play_stop sets min_id past play_id: whatever the stopped track had
// already posted is old news too.
//
// THE QUEUE
//
// ui->queue (play_queue.h) holds what plays and in what order, as track
// numbers of one index: the one whose checksum is ui->queue_index. Its two
// arrays are in PSRAM, allocated once by ui_start. `from_queue` says that
// the track on Now Playing is the queue's current track, and these keep it
// true:
//
//   - whatever moves the queue's position (choosing a track, a track
//     ending, Next, Previous) copies the new current track's path and title
//     out of the index in the same call, and then starts it;
//   - what changes the queue without moving to another track (Play next,
//     Add to queue, shuffle on or off) leaves the current track current:
//     that is the queue's own rule;
//   - a track the console starts is not the queue's. The queue keeps its
//     place meanwhile. When that track ends nothing follows it, and 4 or 6
//     goes back to the queue: its current track is started.
//
// A queue entry is turned into a path only inside one take of the index,
// and only if that index has the queue's checksum. When a track ends, the
// index is taken once, play_queue_next says what follows, and its path,
// title and artist are copied before the index is given back and the player
// is called. If there is no index, or the queue says that playback is over,
// nothing is started and Now Playing shows the last track, stopped, at 0:00.
//
// A stop that is not the queue's doing (a scan, the console's `stop`) leaves
// a queue track where it was: Now Playing shows it paused at the time it
// had reached, and D goes on from there.
//
// Next and Previous work when nothing is playing too: they move in the
// queue and start the track. Next at the end of the queue with loop off (or
// loop one: play_queue.h) stops playback and stays on the last track.
// Previous is given the time the track has reached, as the events last
// reported it; the queue restarts the track or goes back one.
//
// A NEW INDEX
//
// Track numbers mean something only within one index, so when the library
// publishes one (ui_play_library_changed) its checksum is compared with the
// queue's. The same checksum is the same index, entry for entry (a scan of
// an unchanged card), and the queue stays. Any other empties the queue, and
// the track that was remembered from it is forgotten.
//
// A PLAY THE PLAYER REFUSES
//
// player_play fails when the player's command queue is full (eight
// commands waiting: the decoder task stuck on the card) or there is no
// memory for the path. Nothing will come of that play: Now Playing shows
// the new track stopped, and D starts it. The track the player had goes on
// playing unless it is stopped too, so a player_stop goes with the refusal.
// With the queue full that is refused as well; it is then owed
// (play.stop_owed) and sent again with each event of the old track that
// arrives (old news by then) until the player takes it: nothing is polled,
// and the old track's events are what shows that it still plays. A new
// play that the player takes, a console track the UI follows, or a stop
// that is taken settles what was owed. Two cases are left as they are: a
// paused old track posts no events, so it stays loaded, paused and silent,
// until the next play replaces it; and a console `play` taken while a stop
// is owed may be stopped by it, if an old event arrives before its own.
//
// A TRACK THAT WILL NOT PLAY
//
// An ERROR for the current track puts a notice on Now Playing's bottom row
// for 1.5 s ("File missing" if the file was not found, "Can't play"
// otherwise), and when that time is up the queue moves on as for Next (so a
// track that cannot play is skipped under loop one too, not tried again).
// The third failure in a row ends it there instead. "In a row" means with
// no track starting in between: a STARTED resets the count, and so does
// playback coming to rest (the queue ended, three failures, a stop).
// Choosing another track while a notice is up does not: that is the same
// run of failures going on.
// The wait is not a sleep: notice_until_us is one of the times the UI loop
// sleeps until (ui_next_change_ms), and ui_play_on_time acts on it.
//
// WHAT IS SAVED, AND WHEN
//
// The state task (state_task.h) does the saving; each call here copies what
// it is given and returns.
//
//   the queue      queue_changed, the one place that hands it over, and
//                  only when what the queue HOLDS has changed: it is
//                  replaced by a list, something is added, shuffle is
//                  turned on or off, it is emptied, and once after the
//                  power-on restore. Moving in it (next, previous, a track
//                  ending, a track repeating, D) hands nothing over: the
//                  hand-over costs a pass over the whole queue, and the
//                  place in the queue is the position's business. Never
//                  before the queue saved at power-off has been asked for
//                  (ui->queue_restored): the first hand-over makes the state
//                  task give that queue up.
//   the position   save_position: the queue's position, the track there and
//                  the time into it; flash is where these are believed. When
//                  a queue track is started or becomes current (at power-on
//                  too: nothing is written if flash has it already), when it
//                  is paused with D or 5, when it is stopped for a scan or
//                  by the console (the time it had reached, so that D goes
//                  on from there), when playback comes to rest (the time is
//                  then 0), when shuffle moves the current track to another
//                  place, and when a position event shows that
//                  UI_SAVE_EVERY_MS of the track have played since the last
//                  save.
//                  While a track the console started is on Now Playing its
//                  time is not saved, but the saved position must still be
//                  one the queue has, or a later power-on could take an old
//                  place and time for a new queue's: save_place writes the
//                  queue's place with the time 0 when the queue gets its
//                  first track or is shuffled under such a track, and a
//                  position no queue has when the queue is emptied.
//   the settings   shuffle and loop, when 1 or 3 changes them (and Shuffle
//                  play). The volume is ui.c's.
//
// FAST-FORWARD AND REWIND
//
// Holding 6 or 4 on Now Playing (ui.c, KEYS, routes the keys here). The long
// press starts `scrub` (scrub.h) from the time Now Playing shows; each
// repeat moves its target; Now Playing shows the target with `>>` or `<<`
// while it is held; the release jumps there. Only a track with a known
// length can be scrubbed: the player cannot seek in one without (it ignores
// the seek), so a long press on such a track does nothing. Nor while a
// notice is up, a scan runs or there is no card.
//
// The release, in two cases:
//
//   a track the player has (playing or paused)
//                  one player_seek_rel, by the target minus where playback
//                  is at that moment (the player's count; but the time it
//                  was started from until its STARTED has come, and the
//                  target of a seek not yet heard from, which is what the
//                  player counts from then). The player keeps a paused
//                  track paused and silent, and a resume (D) carries on from
//                  the new place (player.c, "seek"). So playing or paused it
//                  is the same call.
//   a remembered track (stopped, or waiting since power-on)
//                  nothing is loaded, so there is nothing to seek in: the
//                  target becomes the time Now Playing shows, which is the
//                  time D starts it from (player_play with a start).
//
// Either way the time shown becomes the target and the position is saved
// (save_position: flash, never the queue file, whose contents have not
// changed). A release that finds the target where the long press put it
// (a long press with no repeat) seeks nowhere, and so does a fast-forward
// whose target playback has meanwhile passed, or a rewind whose target it
// has not reached: neither key ever moves playback the other way.
//
// The scrub belongs to the track it began on. Whatever starts, stops, ends
// or replaces that track (start, come_to_rest, ui_play_stop, the sleep
// timer's stop included; an ERROR; a track the console starts; a new index)
// drops it, and so does any key press (ui.c): the release then does nothing.
// The track that follows a track ending is the queue's business, as usual.
//
// After a seek the next POSITION is the next whole 500 ms after the target,
// but the output may post one more from the old place between the release
// and the moment it is told to flush. Its play number is the same (a seek is
// not a new play), so seek_pending passes over POSITION events that are not
// within a second after the target; at most SEEK_SKIP_MAX of them, in case
// the player made no seek at all (a FLAC whose head was not found).
//
// THE TIME NEVER PASSES THE LENGTH
//
// A seek lands approximately (spec section 4), so playback can run on past
// the length the header gives before ENDED: 307 s of a 305 s track was seen.
// The time taken from a POSITION or a STARTED event is held to the length
// when the length is known, so neither Now Playing nor the saved position
// ever shows more.
//
// AT POWER-ON
//
// The first index the UI sees is the moment the saved queue is restored, in
// the order state_task.h gives (WHO IS BELIEVED): the queue as its file has
// it; then the position saved in flash, if the queue has that track at that
// place; then loop and shuffle from the settings, which are flash's. The
// saved position is looked at once more after that: a power cut between the
// flash save and the file save of a shuffle change leaves a file in the old
// order, and only with flash's shuffle applied is the track at the saved
// place. If neither places the saved track, it is looked for in the final
// play order and its first place is taken (the file's position is only as
// recent as the last change to what the queue holds, so it is the last
// resort). The saved time goes with the saved track wherever it is found;
// if the queue does not have that track, the file's position stands and its
// track starts from 0:00. If the queue then has a current track, it is put
// on Now Playing as a remembered track at that time, and nothing is
// started: D or 5 plays it from there.

// The player's events are logged here, by the task that reads them, under
// the player's name: the audio tasks themselves log nothing while they run.
static const char *PLAYER_TAG = "player";
static const char *TAG = "dap";

static const char ROOT[] = STORAGE_MOUNT_POINT "/";
#define ROOT_LEN (sizeof(ROOT) - 1)

// See FAST-FORWARD AND REWIND: the most POSITION events passed over while a
// seek is awaited, and how far after the target the first one may be.
#define SEEK_SKIP_MAX 2
#define SEEK_WINDOW_MS 1000

static const char *error_name(player_error_t error) {
  switch (error) {
  case PLAYER_ERROR_NOT_FOUND:
    return "not_found";
  case PLAYER_ERROR_UNSUPPORTED:
    return "unsupported";
  case PLAYER_ERROR_DECODE:
    return "decode";
  case PLAYER_ERROR_IO:
    return "io";
  case PLAYER_ERROR_NO_MEM:
    return "no_mem";
  }
  return "decode";
}

// The title is new words on the row: they are read from the beginning.
static void set_title(ui_play_t *play, const char *text, int64_t now_us) {
  if (strcmp(text, play->title) != 0) {
    strlcpy(play->title, text, sizeof(play->title));
    play->title_since_us = now_us;
  }
}

// "Title - Artist" for a track of the index. A track with no tags is in the
// index under the fallbacks, so it reads "<file name> - Unknown Artist".
static bool title_from_index(const library_t *lib, uint32_t track, char *out,
                             size_t out_size, uint32_t *duration_ms) {
  library_track_t info;
  if (!library_track(lib, track, &info) || info.title[0] == '\0') {
    return false;
  }
  if (info.artist[0] != '\0') {
    snprintf(out, out_size, "%s - %s", info.title, info.artist);
  } else {
    snprintf(out, out_size, "%s", info.title);
  }
  *duration_ms = info.duration_ms;
  return true;
}

// The file's name without folder or extension, for a track the index does
// not have.
static void title_from_path(const char *path, char *out, size_t out_size) {
  const char *slash = strrchr(path, '/');
  translit_title(NULL, slash ? slash + 1 : path, out, out_size);
}

// Before `path` is overwritten: if its play is still going, its last events
// are yet to come, and the log wants the path they are about.
static void keep_previous(ui_play_t *play) {
  if (play->active) {
    strcpy(play->prev_path, play->path);
    play->prev_id = play->play_id;
  }
}

// The time shown and saved never passes the length (see THE TIME NEVER
// PASSES THE LENGTH).
static void set_elapsed(ui_play_t *play, uint32_t ms) {
  if (play->duration_ms != 0 && ms > play->duration_ms) {
    ms = play->duration_ms;
  }
  play->elapsed_ms = ms;
}

// Make `track` of the index the current track. Copies; starts nothing.
static void load_track(ui_play_t *play, const library_t *lib, uint32_t track,
                       int64_t now_us) {
  char text[UI_TITLE_MAX];
  keep_previous(play);
  memcpy(play->path, ROOT, ROOT_LEN);
  library_track_path(lib, track, play->path + ROOT_LEN,
                     sizeof(play->path) - ROOT_LEN);
  play->duration_ms = 0;
  if (!title_from_index(lib, track, text, sizeof(text), &play->duration_ms)) {
    title_from_path(play->path, text, sizeof(text));
  }
  strlcpy(play->title, text, sizeof(play->title));
  play->title_since_us = now_us; // a new track, even under the same words
  play->elapsed_ms = 0;
}

// --- saving ------------------------------------------------------------------

// The queue's contents may have changed: hand it to the state task. The one
// place that does. See WHAT IS SAVED.
static void queue_changed(ui_t *ui) {
  if (ui->queue_restored) {
    state_task_save_queue(&ui->queue, ui->queue_index);
  }
}

// Hand over where playback is: the queue's position, the track there and
// the time into it.
static void save_position(ui_t *ui) {
  ui_play_t *play = &ui->play;
  if (!play->from_queue) {
    return;
  }
  const state_position_t position = {
      .position = ui->queue.position,
      .track = play_queue_current(&ui->queue),
      .elapsed_ms = play->elapsed_ms,
  };
  state_task_save_position(&position);
  play->saved_ms = play->elapsed_ms;
}

// The queue has changed under a track that is not its own, or has been
// emptied: make the saved position one this queue has, with no time, or one
// no queue has. See WHAT IS SAVED.
static void save_place(ui_t *ui) {
  // No queue is this long, so no restore can match it.
  state_position_t position = {
      .position = UINT16_MAX, .track = UINT16_MAX, .elapsed_ms = 0};
  if (ui->queue.count != 0) {
    position.position = ui->queue.position;
    position.track = play_queue_current(&ui->queue);
  }
  state_task_save_position(&position);
}

// --- the queue ---------------------------------------------------------------

void ui_play_init(ui_t *ui, uint16_t *orders) {
  // The seed is the hardware's random number, so that two power-ons do not
  // shuffle alike.
  play_queue_init(&ui->queue, orders, orders + UI_QUEUE_CAPACITY,
                  UI_QUEUE_CAPACITY, esp_random());
  play_queue_set_loop(&ui->queue, ui->settings.loop);
  play_queue_set_shuffle(&ui->queue, ui->settings.shuffle);
}

// Make the queue's current track the track on Now Playing. With the index
// held, and the queue not empty. Copies; starts nothing.
static void load_current(ui_t *ui, const library_t *lib, int64_t now_us) {
  ui_play_scrub_drop(ui, "another track");
  load_track(&ui->play, lib, play_queue_current(&ui->queue), now_us);
  ui->play.from_queue = true;
}

// No track is remembered: Now Playing says "Nothing playing".
static void forget(ui_play_t *play) {
  play->path[0] = '\0';
  play->title[0] = '\0';
  play->from_queue = false;
  play->elapsed_ms = 0;
  play->duration_ms = 0;
}

// Ask the player for the current track from `start_ms`.
static void start(ui_t *ui, uint32_t start_ms) {
  ui_play_t *play = &ui->play;
  uint32_t id;
  ui_play_scrub_drop(ui, "a track started");
  play->seek_pending = false;
  play->started = false;
  play->notice_until_us = 0;
  play->elapsed_ms = start_ms;
  if (player_play(play->path, start_ms, &id) == ESP_OK) {
    play->play_id = id;
    play->min_id = id;
    play->active = true;
    play->stop_owed = false; // the new play replaces whatever was playing
  } else {
    // See A PLAY THE PLAYER REFUSES.
    ESP_LOGW(PLAYER_TAG, "the player did not take %s", play->path);
    play->active = false;
    play->min_id = play->play_id + 1;
    play->stop_owed = (player_stop() != ESP_OK);
    if (play->stop_owed) {
      ESP_LOGW(PLAYER_TAG, "the player did not take the stop either: it is "
                           "sent again with its next event");
    }
  }
  // The place in the queue and the time, not the queue: it holds what it
  // held.
  save_position(ui);
}

void ui_play_start(ui_t *ui) { start(ui, 0); }

bool ui_play_load(ui_t *ui, const library_t *lib, tracklist_t list,
                  uint32_t start, bool shuffle, int64_t now_us) {
  play_queue_t *q = &ui->queue;
  if (shuffle) {
    // Any track of the list may come first: a random 32-bit number scaled
    // to the list's length (the top half of the product; no division).
    start = (uint32_t)(((uint64_t)esp_random() * tracklist_count(lib, list)) >>
                       32);
    if (!q->shuffle) {
      // Emptied first: turning shuffle on would shuffle the queue that is
      // about to be replaced.
      play_queue_clear(q);
      play_queue_set_shuffle(q, true);
      ui->settings.shuffle = true;
      state_task_save_settings(&ui->settings);
    }
  }
  play_queue_load(q, lib, list, start);
  ui->queue_index = library_checksum(lib);
  if (q->count == 0) {
    // Not with a sound index: a list on the screen has tracks. The queue is
    // empty now, so whatever is playing is not its track any more.
    ui->play.from_queue = false;
    queue_changed(ui);
    save_place(ui);
    return false;
  }
  load_current(ui, lib, now_us);
  queue_changed(ui);
  return true; // ui_play_start saves the position
}

bool ui_play_load_tracks(ui_t *ui, const library_t *lib, const uint16_t *tracks,
                         uint32_t count, uint32_t start, bool shuffle,
                         int64_t now_us) {
  play_queue_t *q = &ui->queue;
  if (start >= count || tracks[start] >= library_track_count(lib)) {
    return false; // checked before, so not expected: nothing changes
  }
  if (shuffle && !q->shuffle) {
    // As in ui_play_load: emptied first, so that turning shuffle on does not
    // shuffle the queue that is about to be replaced.
    play_queue_clear(q);
    play_queue_set_shuffle(q, true);
    ui->settings.shuffle = true;
    state_task_save_settings(&ui->settings);
  }
  if (play_queue_load_tracks(q, tracks, count, start,
                             library_track_count(lib)) == 0) {
    return false; // not with `start` checked above
  }
  ui->queue_index = library_checksum(lib);
  load_current(ui, lib, now_us);
  queue_changed(ui);
  return true; // ui_play_start saves the position
}

bool ui_play_add(ui_t *ui, const library_t *lib, const browse_target_t *target,
                 bool next, int64_t now_us) {
  play_queue_t *q = &ui->queue;
  ui_play_t *play = &ui->play;
  uint32_t checksum = library_checksum(lib);
  bool was_empty = (q->count == 0);
  if (was_empty) {
    ui->queue_index = checksum; // an empty queue belongs to no index
  } else if (checksum != ui->queue_index) {
    return false; // see A NEW INDEX: a queue of another index was emptied
  }

  bool added;
  if (target->kind == BROWSE_TARGET_TRACK) {
    uint32_t track = tracklist_track(lib, target->list, target->index);
    added = next ? play_queue_insert_next(q, track)
                 : play_queue_append(q, track);
  } else {
    added = play_queue_append_list(q, lib, target->list) != 0;
  }
  if (!added) {
    return false;
  }
  if (was_empty && !play->active && play->notice_until_us == 0) {
    // Nothing is playing and the queue has its first track: that is the
    // track D starts. It does not start by itself.
    load_current(ui, lib, now_us);
    save_position(ui);
  } else if (was_empty) {
    save_place(ui); // the console's track plays on; the queue has a place
  }
  queue_changed(ui);
  return true;
}

// Stop the player, and take none of the stopped track's events as news.
static void stop_player(ui_t *ui) {
  // After any `play` of ours in the player's queue, so that a track started
  // a moment ago (the one before it having just ended) is stopped too.
  if (player_stop() == ESP_OK) {
    ui->play.stop_owed = false;
  }
  ui->play.min_id = ui->play.play_id + 1;
}

// Playback is over, not just a track: the last track stays on show, stopped,
// and D plays it from its beginning. Whatever is chosen next starts a new
// run, with no failures against it.
static void come_to_rest(ui_t *ui) {
  ui_play_t *play = &ui->play;
  ui_play_scrub_drop(ui, "playback came to rest");
  play->active = false;
  play->notice_until_us = 0;
  play->elapsed_ms = 0;
  play->failures = 0;
  save_position(ui);
}

typedef enum {
  MOVE_ENDED,    // the current track played to its end
  MOVE_NEXT,     // the user asked for the next one, or the track cannot play
  MOVE_PREVIOUS, // the user asked to go back
} move_t;

typedef enum {
  MOVED_NOWHERE, // there is no index to look the queue's tracks up in
  MOVED_TO_END,  // the queue says playback is over
  MOVED,         // a track is current and ready to be started
} moved_t;

// Move in the queue and make the track it gives the current one. Takes the
// index once. The queue is not empty.
static moved_t move(ui_t *ui, move_t how, int64_t now_us) {
  play_queue_t *q = &ui->queue;
  ui_play_t *play = &ui->play;
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    return MOVED_NOWHERE;
  }
  moved_t moved = MOVED_NOWHERE;
  if (library_checksum(lib) == ui->queue_index) {
    uint32_t track;
    if (!play->from_queue) {
      // Back from a track the console started: the queue's own track.
      track = play_queue_current(q);
    } else if (how == MOVE_PREVIOUS) {
      track = play_queue_previous(q, play->elapsed_ms);
    } else {
      track = play_queue_next(q, how == MOVE_ENDED);
    }
    if (track == PLAY_QUEUE_NONE) {
      moved = MOVED_TO_END;
    } else {
      load_current(ui, lib, now_us);
      moved = MOVED;
    }
  }
  library_task_put();
  return moved;
}

// The current track is over (it ended, or it cannot play). Start what the
// queue says follows it, if anything does.
static void advance(ui_t *ui, move_t how, int64_t now_us) {
  if (ui->play.from_queue && ui->queue.count != 0 &&
      move(ui, how, now_us) == MOVED) {
    start(ui, 0);
  } else {
    come_to_rest(ui);
  }
}

// 4 and 6. Not during a scan, which wants the card to itself, nor without a
// card.
static void skip(ui_t *ui, move_t how, int64_t now_us) {
  if (ui->queue.count == 0 || ui->scanning || ui->no_card) {
    return;
  }
  switch (move(ui, how, now_us)) {
  case MOVED:
    start(ui, 0);
    break;
  case MOVED_TO_END:
    // The queue stays on its last track. If that track is not playing
    // there is nothing to stop, and it keeps the time it shows.
    if (ui->play.active || ui->play.notice_until_us != 0) {
      stop_player(ui);
      come_to_rest(ui);
    }
    break;
  case MOVED_NOWHERE:
    break;
  }
}

void ui_play_next(ui_t *ui, int64_t now_us) { skip(ui, MOVE_NEXT, now_us); }

void ui_play_previous(ui_t *ui, int64_t now_us) {
  skip(ui, MOVE_PREVIOUS, now_us);
}

void ui_play_toggle_shuffle(ui_t *ui, int64_t now_us) {
  bool on = !ui->queue.shuffle;
  // The current track stays current, at another place in the play order.
  play_queue_set_shuffle(&ui->queue, on);
  ui->settings.shuffle = on;
  state_task_save_settings(&ui->settings);
  if (ui->queue.count != 0) {
    queue_changed(ui);
    if (ui->play.from_queue) {
      save_position(ui);
    } else {
      save_place(ui);
    }
  }
  ui_overlay_show(ui, on ? UI_OVERLAY_SHUFFLE_ON : UI_OVERLAY_SHUFFLE_OFF,
                  now_us);
}

void ui_play_cycle_loop(ui_t *ui, int64_t now_us) {
  static const ui_overlay_t SAYS[] = {UI_OVERLAY_LOOP_OFF, UI_OVERLAY_LOOP_ALL,
                                      UI_OVERLAY_LOOP_ONE};
  play_loop_t loop = ui->queue.loop == PLAY_LOOP_OFF   ? PLAY_LOOP_ALL
                     : ui->queue.loop == PLAY_LOOP_ALL ? PLAY_LOOP_ONE
                                                       : PLAY_LOOP_OFF;
  // Flash is where loop is saved; the queue's file need not be told.
  play_queue_set_loop(&ui->queue, loop);
  ui->settings.loop = loop;
  state_task_save_settings(&ui->settings);
  ui_overlay_show(ui, SAYS[loop], now_us);
}

void ui_play_toggle(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  player_status_t status;
  player_get_status(&status);
  if (status.state == PLAYER_STATE_PLAYING) {
    player_pause();
    if (play->active) {
      save_position(ui); // the time the events last reported: within 0.5 s
    }
  } else if (status.state == PLAYER_STATE_PAUSED) {
    player_resume();
  } else if (!play->active && play->notice_until_us == 0 &&
             play->path[0] != '\0' && !ui->scanning && !ui->no_card) {
    // Nothing is loaded, none is on its way and a track is remembered: play
    // it, from the time it shows (0:00, or where it was at power-off). It
    // keeps its place in the queue, which follows on. (Not during a scan,
    // which wants the card to itself, nor without a card.)
    play->title_since_us = now_us;
    start(ui, play->elapsed_ms);
  }
}

// --- fast-forward and rewind (see FAST-FORWARD AND REWIND) -------------------

void ui_play_scrub_drop(ui_t *ui, const char *why) {
  if (ui->scrub.active) {
    ui->scrub.active = false;
    ESP_LOGI(TAG, "SCRUB dropped (%s)", why);
  }
}

void ui_play_scrub_start(ui_t *ui, int direction, int64_t now_us) {
  const ui_play_t *play = &ui->play;
  if (play->path[0] == '\0' || play->notice_until_us != 0 ||
      play->duration_ms == 0 || ui->scanning || ui->no_card) {
    return;
  }
  scrub_start(&ui->scrub, direction, play->elapsed_ms, play->duration_ms,
              now_us / 1000);
  ui->scrub_from_ms = ui->scrub.target_ms;
  ESP_LOGI(TAG, "SCRUB %s from %" PRIu32 " ms", direction > 0 ? ">>" : "<<",
           ui->scrub.target_ms);
}

void ui_play_scrub_repeat(ui_t *ui, int64_t now_us) {
  scrub_repeat(&ui->scrub, now_us / 1000); // nothing if it is not active
}

void ui_play_scrub_release(ui_t *ui) {
  ui_play_t *play = &ui->play;
  if (!ui->scrub.active) {
    return;
  }
  int direction = ui->scrub.direction;
  uint32_t target = scrub_finish(&ui->scrub);
  if (target == ui->scrub_from_ms) {
    ESP_LOGI(TAG, "SCRUB released at %" PRIu32 " ms, where it began: no seek",
             target);
    return;
  }
  // Where playback is now, as the player will count from when it takes the
  // seek: the time shown for a remembered track, and for one whose STARTED
  // has not come (that is the time it was started from, and the player's
  // count is still the track before's); the target of a seek not yet heard
  // from; otherwise the player's own count.
  uint32_t now_ms = play->elapsed_ms;
  if (play->active && play->started && play->seek_pending) {
    now_ms = play->seek_target_ms;
  } else if (play->active && play->started) {
    player_status_t status;
    player_get_status(&status);
    now_ms = status.position_ms;
  }
  int64_t delta = (int64_t)target - (int64_t)now_ms;
  if ((direction > 0 && delta <= 0) || (direction < 0 && delta >= 0)) {
    ESP_LOGI(TAG, "SCRUB released at %" PRIu32 " ms, playback at %" PRIu32
             " ms: no seek", target, now_ms);
    return;
  }
  if (play->active) {
    // |delta| is below the length of a track, which is far below 2^31 ms.
    if (player_seek_rel((int32_t)delta) != ESP_OK) {
      ESP_LOGW(TAG, "SCRUB the player did not take the seek");
      return;
    }
    play->seek_pending = true;
    play->seek_target_ms = target;
    play->seek_skipped = 0;
  }
  ESP_LOGI(TAG, "SCRUB to %" PRIu32 " ms (%s, by %+" PRId64 " ms)", target,
           play->active ? "seek" : "the time D starts from", delta);
  set_elapsed(play, target);
  save_position(ui); // flash only: what the queue holds has not changed
}

// Find the track on Now Playing in `lib` again: its title and artist may
// have changed, or it may have started while there was no index. One that
// is not there any more and is not playing is forgotten.
static void find_again(ui_t *ui, const library_t *lib, int64_t now_us) {
  ui_play_t *play = &ui->play;
  if (strncmp(play->path, ROOT, ROOT_LEN) != 0) {
    return; // no track, or one that is not on the card
  }
  // The index has paths relative to the card: "music/a.flac".
  char text[UI_TITLE_MAX];
  uint32_t track;
  uint32_t duration_ms;
  if (library_find_path(lib, play->path + ROOT_LEN, &track)) {
    if (title_from_index(lib, track, text, sizeof(text), &duration_ms)) {
      set_title(play, text, now_us);
      if (play->duration_ms == 0) {
        play->duration_ms = duration_ms;
      }
    }
  } else if (!play->active) {
    // There is nothing for D to start. (One that is playing stays, under
    // its file name.)
    forget(play);
  }
}

// An event of a play the UI did not start: the console's `play`. Take it up
// as the current track. It is not the queue's.
static void adopt(ui_t *ui, uint32_t play_id, int64_t now_us) {
  ui_play_t *play = &ui->play;
  char text[UI_TITLE_MAX];
  ui_play_scrub_drop(ui, "the console started a track");
  play->seek_pending = false;
  play->started = false;
  keep_previous(play);
  // The player's newest path. Right unless the console has asked for yet
  // another track since; that one's events then correct it.
  player_get_path(play->path, sizeof(play->path));
  title_from_path(play->path, text, sizeof(text));
  strlcpy(play->title, text, sizeof(play->title));
  play->title_since_us = now_us;
  play->elapsed_ms = 0;
  play->duration_ms = 0;
  play->play_id = play_id;
  play->min_id = play_id;
  play->active = true;
  play->stop_owed = false; // the console's track is the one to follow now
  play->from_queue = false;
  play->notice_until_us = 0;
  // Its title and artist, if it is in the index.
  const library_t *lib = ui_index_get(ui);
  if (lib != NULL) {
    find_again(ui, lib, now_us);
    library_task_put();
  }
}

void ui_play_on_event(ui_t *ui, const player_event_t *event, int64_t now_us) {
  ui_play_t *play = &ui->play;
  // Which `play` the event is about. A line of its own, ahead of the event's
  // line: the four below are a fixed format that scripts read.
  static const char *const TYPE[] = {"started", "position", "ended", "error"};
  ESP_LOGI(PLAYER_TAG, "EVENT id=%" PRIu32 " type=%s", event->play_id,
           TYPE[event->type]);

  // See WHICH EVENTS COUNT. An old event is logged like any other, so that
  // the log shows everything the player did, and that is all.
  bool old = event->play_id < play->min_id;
  if (old && play->stop_owed && player_stop() == ESP_OK) {
    // A PLAY THE PLAYER REFUSES: the track it plays on is stopped now.
    play->stop_owed = false;
    ESP_LOGI(PLAYER_TAG, "the stop owed since a refused play was taken");
  }
  if (!old && event->play_id != play->play_id) {
    adopt(ui, event->play_id, now_us);
  }

  switch (event->type) {
  case PLAYER_EVENT_STARTED: {
    // The event has no path. The current track's is the one asked for; an
    // old event is about the track before, if anything is known of it.
    const char *path = !old                             ? play->path
                       : event->play_id == play->prev_id ? play->prev_path
                                                         : "?";
    ESP_LOGI(PLAYER_TAG,
             "STARTED path=%s rate=%" PRIu32 " bits=%u ch=%u duration_ms=%"
             PRIu32,
             path, event->sample_rate, event->bits, event->channels,
             event->duration_ms);
    if (!old) {
      // The time stays where the track was started from (within the
      // length).
      play->duration_ms = event->duration_ms;
      set_elapsed(play, play->elapsed_ms);
      play->started = true;
      play->failures = 0;
    }
    break;
  }
  case PLAYER_EVENT_POSITION:
    ESP_LOGI(PLAYER_TAG, "POSITION ms=%" PRIu32, event->position_ms);
    if (!old && play->seek_pending &&
        (event->position_ms < play->seek_target_ms ||
         event->position_ms - play->seek_target_ms > SEEK_WINDOW_MS) &&
        play->seek_skipped < SEEK_SKIP_MAX) {
      // From before the seek (see FAST-FORWARD AND REWIND).
      play->seek_skipped++;
      ESP_LOGI(TAG, "SCRUB position %" PRIu32 " ms is from before the seek",
               event->position_ms);
      break;
    }
    if (!old) {
      play->seek_pending = false;
      set_elapsed(play, event->position_ms);
      // Every UI_SAVE_EVERY_MS of the track, and when it has been moved
      // back: a comparison per event, no timer.
      if (play->from_queue &&
          (play->elapsed_ms >= play->saved_ms + UI_SAVE_EVERY_MS ||
           play->elapsed_ms < play->saved_ms)) {
        save_position(ui);
      }
    }
    break;

  case PLAYER_EVENT_ENDED: {
    // The event has no room for frame counts; the player keeps those of the
    // last track that ended.
    player_status_t status;
    player_get_status(&status);
    ESP_LOGI(PLAYER_TAG, "ENDED frames=%" PRIu32 " expected=%" PRIu32,
             status.ended_frames, status.ended_expected);
    if (!old) {
      play->active = false;
      advance(ui, MOVE_ENDED, now_us);
    }
    break;
  }
  case PLAYER_EVENT_ERROR:
    ESP_LOGE(PLAYER_TAG, "ERROR reason=%s", error_name(event->error));
    if (!old) {
      ui_play_scrub_drop(ui, "the track failed");
      play->active = false;
      play->failures++;
      play->notice = event->error == PLAYER_ERROR_NOT_FOUND
                         ? UI_NOTICE_FILE_MISSING
                         : UI_NOTICE_CANT_PLAY;
      play->notice_until_us = now_us + UI_OVERLAY_MS * 1000;
    }
    break;
  }

  // The output task cannot log an underrun itself. It counts them, and the
  // count is looked at here, twice a second while a track plays.
  player_status_t status;
  player_get_status(&status);
  if (status.underruns != ui->underruns) {
    ESP_LOGW(PLAYER_TAG, "UNDERRUN count=%" PRIu32 " (the buffer ran dry)",
             status.underruns);
    ui->underruns = status.underruns;
  }
}

void ui_play_on_time(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  if (play->notice_until_us == 0 || now_us < play->notice_until_us) {
    return;
  }
  play->notice_until_us = 0;
  if (play->failures >= UI_FAILURES_MAX) {
    ESP_LOGW(PLAYER_TAG, "%" PRIu32 " tracks in a row would not play: stopping",
             play->failures);
    come_to_rest(ui);
    return;
  }
  advance(ui, MOVE_NEXT, now_us);
}

void ui_play_stop(ui_t *ui) {
  ui_play_t *play = &ui->play;
  ui_play_scrub_drop(ui, "stopped");
  stop_player(ui);
  play->active = false;
  play->notice_until_us = 0; // and no next track after a notice either
  play->failures = 0;
  if (play->from_queue) {
    // The queue's track stays where it had got to (or where it was waiting,
    // if it was never started): D goes on from there, and so does the next
    // power-on.
    save_position(ui);
  } else {
    play->elapsed_ms = 0; // the console's track: D plays it from 0:00
  }
}

// The first index: put the saved queue, position and settings together. See
// AT POWER-ON. True if a track now waits on Now Playing.
static bool restore(ui_t *ui, const library_t *lib, int64_t now_us) {
  play_queue_t *q = &ui->queue;
  ui_play_t *play = &ui->play;
  // The queue is empty until now (it is filled only from an index, and this
  // is the first), so no track can be its; make sure of it, because the
  // restore replaces whatever the queue holds.
  play->from_queue = false;
  bool restored = state_task_restore_queue(q, lib);
  ui->queue_restored = true;
  ui->queue_index = library_checksum(lib);

  // The saved time belongs to one track at one place in the queue. If the
  // queue is from another moment than the position, the saved track is
  // looked for (below); only if the queue does not have it does the file's
  // own position stand, and its track start from the beginning.
  uint32_t elapsed_ms = 0;
  state_position_t saved;
  bool has_saved = restored && state_task_loaded_position(&saved);
  bool placed = has_saved && saved.position < q->count &&
                q->play[saved.position] == saved.track &&
                play_queue_set_position(q, saved.position);
  // The settings are the UI's: what flash had, and any change since.
  play_queue_set_loop(q, ui->settings.loop);
  play_queue_set_shuffle(q, ui->settings.shuffle);
  // Flash can be one shuffle change ahead of the file: then the saved track
  // is at the saved place only now. See AT POWER-ON.
  bool timed = placed || (has_saved && q->count != 0 &&
                          q->position == saved.position &&
                          play_queue_current(q) == saved.track);
  // The file's own position is only as recent as the last change to what
  // the queue holds, so it is the last resort: if the saved track is in the
  // queue at all, its first place in the play order (now final) is taken.
  // Once per power-on, one pass over the queue.
  bool found = false;
  if (!timed && has_saved && saved.track != UINT16_MAX &&
      saved.track < library_track_count(lib)) {
    for (uint32_t i = 0; i < q->count; i++) {
      if (q->play[i] == saved.track) {
        found = play_queue_set_position(q, i);
        break;
      }
    }
  }
  if (timed || found) {
    elapsed_ms = saved.elapsed_ms;
  }
  ESP_LOGI(TAG,
           "queue %s: %" PRIu32 " tracks, position %" PRIu32 "%s, shuffle %d, "
           "loop %d",
           restored ? "restored" : "not restored", q->count, q->position,
           timed ? " (as saved)" : (found ? " (saved track found)" : ""),
           (int)q->shuffle, (int)q->loop);
  // From here on the queue is handed over when it changes. This first time
  // it is what the file holds, unless the settings above changed it.
  queue_changed(ui);

  if (q->count == 0 || play->active || play->notice_until_us != 0) {
    return false; // no queue, or the console has a track playing already
  }
  load_current(ui, lib, now_us);
  if (play->duration_ms != 0 && elapsed_ms >= play->duration_ms) {
    elapsed_ms = 0; // not a time this track has
  }
  play->elapsed_ms = elapsed_ms;
  // Flash has this already unless the queue and the position were out of
  // step, or a setting moved the track to another place: then the next
  // power-on finds them in step.
  save_position(ui);
  return true;
}

bool ui_play_library_changed(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  ui_play_scrub_drop(ui, "a new index");
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    return false;
  }
  bool waiting = false;
  if (!ui->queue_restored) {
    waiting = restore(ui, lib, now_us);
  } else if (library_checksum(lib) != ui->queue_index) {
    // See A NEW INDEX. The scan stopped whatever the queue was playing.
    ESP_LOGI(TAG, "the index has changed: the queue is emptied");
    play_queue_clear(&ui->queue);
    ui->queue_index = library_checksum(lib);
    if (play->from_queue) {
      play->from_queue = false;
      if (!play->active) {
        forget(play);
      }
    }
    queue_changed(ui);
    save_place(ui); // the saved position was the old queue's
  }
  find_again(ui, lib, now_us);
  library_task_put();
  return waiting;
}
