#pragma once

#include <stddef.h>

// The LCD1602 shows basic ASCII only. These functions turn UTF-8 tag text into
// something it can display.

typedef struct {
  size_t chars;   // code points read from the input
  size_t unknown; // how many of those had no LCD equivalent and became '?'
} translit_stats_t;

// Convert `utf8` into `out`. At most `out_size` bytes are written, always
// including the terminator when `out_size` is non-zero. The returned counts
// cover the whole input even if the output was truncated. A NULL input gives
// an empty string.
translit_stats_t translit(const char *utf8, char *out, size_t out_size);

// Convert a track title for display. If the title is missing, empty, or more
// than half of it has no LCD equivalent, the file name without its extension
// is converted instead. Either argument may be NULL.
void translit_title(const char *title, const char *file_name, char *out,
                    size_t out_size);
