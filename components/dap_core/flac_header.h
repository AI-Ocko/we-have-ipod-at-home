#pragma once

#include <stdint.h>
#include <stdio.h>

// Reads the start of a FLAC file: the stream format, the exact length and
// where the audio begins. The player uses it to check that every frame of a
// track was played, to show the duration and to seek. It runs against files on
// a slow SD card, so it reads only the header bytes it needs and steps over
// everything else (tags, embedded pictures) with a seek.

typedef struct {
  uint32_t sample_rate;     // Hz
  uint8_t channels;         // 1..8
  uint8_t bits_per_sample;  // 4..32
  uint64_t total_frames;    // samples per channel; 0 if the file does not say
  uint32_t max_frame_bytes; // largest encoded frame; 0 if the file does not say
  long audio_start;         // byte offset of the first audio frame
} flac_header_t;

typedef enum {
  FLAC_HEADER_OK,
  FLAC_HEADER_NOT_FLAC,  // no "fLaC" marker where one should be
  FLAC_HEADER_TRUNCATED, // the file ends inside the header
} flac_header_result_t;

// Read a FLAC file's header from the start of `f`. Skips a leading ID3v2 tag
// if there is one. Leaves the file position unspecified.
//
// `*out` is written only when the result is FLAC_HEADER_OK. The values are the
// ones the file states; whether the player supports them is the caller's call.
//
// A file that has the marker but does not start with a 34-byte STREAMINFO
// block is FLAC_HEADER_NOT_FLAC. A read or seek that fails for any reason
// (including an offset too large for `long`) is FLAC_HEADER_TRUNCATED.
flac_header_result_t flac_header_read(FILE *f, flac_header_t *out);

// The size of a STREAMINFO block's body.
#define FLAC_STREAMINFO_BYTES 34

// Unpack the body of a STREAMINFO block that the caller has read: everything
// in `*out` except `audio_start`, which only walking the file's other blocks
// can give and which is left as it was. For a reader that wants the format
// and the length but not the walk (flac_header_read does both).
void flac_header_unpack_streaminfo(const uint8_t body[FLAC_STREAMINFO_BYTES],
                                   flac_header_t *out);

// Track length in milliseconds, or 0 if total_frames or sample_rate is 0.
// Rounds down; a length that does not fit is reported as UINT32_MAX.
uint32_t flac_header_duration_ms(const flac_header_t *h);
