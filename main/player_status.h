#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// What the player can be asked about itself. player.h is the control
// interface (commands in, events out); this is the read-only side, used by
// the screen, by the code that logs the player's events and by the debug
// console's `stat`. One test aid for the console is at the end.
//
// Everything here may be called from any task at any time. Each number is
// read on its own, so a snapshot taken while a track is starting or ending
// can mix values from just before and just after.

typedef enum {
  PLAYER_STATE_IDLE,
  PLAYER_STATE_PLAYING,
  PLAYER_STATE_PAUSED,
} player_state_t;

typedef struct {
  player_state_t state;
  uint32_t position_ms; // audio handed to the DAC link so far in this track
  int volume;           // 0..20

  // The buffer between the decoder and the output, in bytes.
  size_t ring_size;
  size_t ring_fill;
  // The least the buffer held between first being full and the decoder
  // reaching the end of the file: the reserve left at the worst moment. -1
  // until the buffer has been full once in this track. (At the start and the
  // end of a track the buffer is nearly empty by design; those do not count.)
  int32_t ring_low;
  uint32_t underruns; // times the buffer ran dry while playing, since start-up

  // The last track that played to its end: frames handed to the DAC link and
  // the number its header promised (0 if the header does not say).
  uint32_t ended_frames;
  uint32_t ended_expected;

  // Load of the track in progress, or of the last one if there is none.
  // `wall_ms` runs from the `play` command to now or to the track's end.
  // `decoder_busy_ms` is how long the decoder task spent reading and decoding
  // (waiting for the card included). `output_busy_ms` is how long the output
  // task spent fetching, converting and scaling blocks; its wait for the DAC
  // link to take a block is not counted.
  uint32_t wall_ms;
  uint32_t decoder_busy_ms;
  uint32_t output_busy_ms;

  // Bytes of each task's stack that have never been used.
  uint32_t decoder_stack_free;
  uint32_t output_stack_free;
} player_status_t;

void player_get_status(player_status_t *out);

// Copy the path most recently accepted by a `play` command into `out` (always
// terminated; empty if nothing has been played yet).
void player_get_path(char *out, size_t out_size);

// Test aid, for the debug console: make the decoder task do nothing at all
// for `ms` milliseconds, as if the card had stopped answering. Nothing is
// decoded and no command is served until the time is up, so once the buffer
// has played out there is an underrun. This is how the underrun path is
// exercised without a faulty card.
#define PLAYER_DEBUG_STALL_MAX_MS 30000
esp_err_t player_debug_stall(uint32_t ms);
