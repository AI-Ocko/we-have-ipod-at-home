#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TAGS_TEXT_MAX 128 // bytes per field, including the terminator

typedef enum {
  TAGS_FORMAT_FLAC,
  TAGS_FORMAT_MP3,
} tags_format_t;

// What a file says about itself. Text is UTF-8, exactly as stored in the
// file apart from being cut to TAGS_TEXT_MAX - 1 bytes at a character
// boundary; a field the file does not have is an empty string. Converting
// for the LCD and filling in "Unknown Artist" is the caller's business.
typedef struct {
  char title[TAGS_TEXT_MAX];
  char artist[TAGS_TEXT_MAX];
  char album[TAGS_TEXT_MAX];
  char album_artist[TAGS_TEXT_MAX];
  uint16_t track_no;    // 0 if not given
  uint16_t disc_no;     // 0 if not given
  uint32_t duration_ms; // 0 if not known
} tags_t;

// Read tags from an open file of the given format. Returns false only if the
// file could not be read at all or is plainly not that format; a file with no
// tags returns true with empty fields. Leaves the file position unspecified.
bool tags_read(FILE *f, tags_format_t format, tags_t *out);

// The format a file name's extension claims (.flac, .mp3; any case).
// Returns false for anything else.
bool tags_format_from_name(const char *name, tags_format_t *format);
