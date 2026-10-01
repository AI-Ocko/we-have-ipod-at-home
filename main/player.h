#pragma once

#include "esp_err.h"

// Register the audio decoders. Call once at start-up.
void player_init(void);

// Decode a FLAC file and play it through audio_out. Blocks until the track
// ends or fails. Must run on a task with at least 16KB of stack: the FLAC
// decoder overflows smaller ones.
esp_err_t player_play_file(const char *path);
