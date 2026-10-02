#include "esp_log.h"
#include "library_task.h"
#include "player.h"
#include "player_status.h"
#include "storage.h"
#include "translit.h"
#include "ui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// The track that is playing and the list it came from. Part of the UI task:
// every function here is called from its loop (ui.c) and changes ui->play.
//
// THE TRACK
//
// `path` is the track that is playing or, when nothing is, the last one that
// was: the "remembered" track that D starts again. `title`, `duration_ms`
// and `elapsed_ms` are what Now Playing shows of it. They are copies, made
// when the track is chosen; nothing here points into the index.
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
//                        UI takes it up as the current track, with no list.
//
// Starting a track sets both to its number. A stop (for a scan, or from the
// console) is reported by no event, so ui_play_stop sets min_id past
// play_id: whatever the stopped track had already posted is old news too.
//
// THE LIST
//
// A track chosen in the menus comes with the list it was in (a tracklist_t:
// an album, an artist or Songs), its position there and the generation of
// the index those numbers belong to. When the track ends, position + 1 is
// looked up and played: the index is taken once, for the track's path,
// title and artist, and given back before the player is called. The list
// is over when there is no such position, when the index has been replaced
// since the track was chosen, or when there is no index: then nothing is
// started and Now Playing shows the last track, stopped, at 0:00.
//
// A TRACK THAT WILL NOT PLAY
//
// An ERROR for the current track puts a notice on Now Playing's bottom row
// for 1.5 s ("File missing" if the file was not found, "Can't play"
// otherwise), and when that time is up the list goes on as if the track had
// ended. The third failure in a row ends it there instead. "In a row" means
// with no track starting in between: a STARTED resets the count, and so
// does playback coming to rest (the list ended, three failures, a stop).
// Choosing another track while a notice is up does not: that is the same
// run of failures going on.
// The wait is not a sleep: notice_until_us is one of the times the UI loop
// sleeps until (ui_next_change_ms), and ui_play_on_time acts on it.

// The player's events are logged here, by the task that reads them, under
// the player's name: the audio tasks themselves log nothing while they run.
static const char *PLAYER_TAG = "player";

static const char ROOT[] = STORAGE_MOUNT_POINT "/";
#define ROOT_LEN (sizeof(ROOT) - 1)

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

void ui_play_start(ui_t *ui) {
  ui_play_t *play = &ui->play;
  uint32_t id;
  play->notice_until_us = 0;
  play->elapsed_ms = 0;
  if (player_play(play->path, 0, &id) != ESP_OK) {
    // The player's command queue is full, or there is no memory for the
    // path. Nothing will come of it: the track is stopped, not playing.
    ESP_LOGW(PLAYER_TAG, "the player did not take %s", play->path);
    play->active = false;
    play->min_id = play->play_id + 1;
    return;
  }
  play->play_id = id;
  play->min_id = id;
  play->active = true;
}

void ui_play_choose(ui_t *ui, const library_t *lib, tracklist_t list,
                    uint32_t index, int64_t now_us) {
  ui_play_t *play = &ui->play;
  load_track(play, lib, tracklist_track(lib, list, index), now_us);
  play->list = list;
  play->list_pos = index;
  play->list_generation = ui->generation;
}

// Playback is over, not just a track: the last track stays on show, stopped,
// and D plays it from its beginning. Whatever is chosen next starts a new
// run, with no failures against it.
static void come_to_rest(ui_play_t *play) {
  play->active = false;
  play->notice_until_us = 0;
  play->elapsed_ms = 0;
  play->failures = 0;
}

// The current track is over. Start the next of its list, if there is one.
static void advance(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  bool loaded = false;
  if (play->list.kind != TRACKLIST_NONE &&
      play->list_generation == ui->generation) {
    const library_t *lib = ui_index_get(ui);
    if (lib != NULL) {
      uint32_t track = tracklist_track(lib, play->list, play->list_pos + 1);
      if (track != UINT32_MAX) {
        load_track(play, lib, track, now_us);
        play->list_pos++;
        loaded = true;
      }
      library_task_put();
    }
  }
  if (loaded) {
    ui_play_start(ui);
  } else {
    come_to_rest(play);
  }
}

void ui_play_toggle(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  player_status_t status;
  player_get_status(&status);
  if (status.state == PLAYER_STATE_PLAYING) {
    player_pause();
  } else if (status.state == PLAYER_STATE_PAUSED) {
    player_resume();
  } else if (!play->active && play->notice_until_us == 0 &&
             play->path[0] != '\0' && !ui->scanning && !ui->no_card) {
    // Nothing is loaded, none is on its way and a track is remembered: play
    // it again. It keeps its place in its list, so the list follows on if
    // the index is still the one it was chosen from. (Not during a scan,
    // which wants the card to itself, nor without a card.)
    play->title_since_us = now_us;
    ui_play_start(ui);
  }
}

// An event of a play the UI did not start: the console's `play`. Take it up
// as the current track. It is in no list.
static void adopt(ui_t *ui, uint32_t play_id, int64_t now_us) {
  ui_play_t *play = &ui->play;
  char text[UI_TITLE_MAX];
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
  play->list.kind = TRACKLIST_NONE;
  play->notice_until_us = 0;
  ui_play_library_changed(ui, now_us); // its title and artist, if indexed
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
      play->elapsed_ms = 0;
      play->duration_ms = event->duration_ms;
      play->failures = 0;
    }
    break;
  }
  case PLAYER_EVENT_POSITION:
    ESP_LOGI(PLAYER_TAG, "POSITION ms=%" PRIu32, event->position_ms);
    if (!old) {
      play->elapsed_ms = event->position_ms;
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
      advance(ui, now_us);
    }
    break;
  }
  case PLAYER_EVENT_ERROR:
    ESP_LOGE(PLAYER_TAG, "ERROR reason=%s", error_name(event->error));
    if (!old) {
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
    come_to_rest(play);
    return;
  }
  advance(ui, now_us);
}

void ui_play_stop(ui_t *ui) {
  ui_play_t *play = &ui->play;
  // After any `play` of ours in the player's queue, so that a track started
  // a moment ago (the one before it having just ended) is stopped too.
  player_stop();
  come_to_rest(play); // and no next track after a notice either
  play->min_id = play->play_id + 1;
}

void ui_play_library_changed(ui_t *ui, int64_t now_us) {
  ui_play_t *play = &ui->play;
  if (strncmp(play->path, ROOT, ROOT_LEN) != 0) {
    return; // no track, or one that is not on the card
  }
  const library_t *lib = ui_index_get(ui);
  if (lib == NULL) {
    return;
  }
  // The index has paths relative to the card: "music/a.flac".
  char text[UI_TITLE_MAX];
  uint32_t track;
  uint32_t duration_ms;
  bool found = library_find_path(lib, play->path + ROOT_LEN, &track);
  bool titled = found &&
                title_from_index(lib, track, text, sizeof(text), &duration_ms);
  library_task_put(); // only `text` and the numbers from here

  if (titled) {
    set_title(play, text, now_us);
    if (play->duration_ms == 0) {
      play->duration_ms = duration_ms;
    }
  } else if (!found && !play->active) {
    // The remembered track is not in the index any more: there is nothing
    // for D to start. (One that is playing stays, under its file name.)
    play->path[0] = '\0';
    play->title[0] = '\0';
    play->list.kind = TRACKLIST_NONE;
  }
}
