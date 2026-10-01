#pragma once

#include <stdbool.h>
#include <stdint.h>

// ID3 tags wrap a file's audio: version 2 goes in front of it (and can hold a
// cover picture of any size), version 1 is the last 128 bytes of the file.
// The player needs to know where they are so that it can start reading at the
// audio, stop before the tag at the end, and work out the length and the
// seek positions of an MP3 from the audio bytes alone. These two functions
// only look at bytes the caller has read; they touch no file.

#define ID3V2_HEADER_BYTES 10
#define ID3V1_TAG_BYTES 128
#define ID3V1_MARKER_BYTES 3

// The whole length of the ID3v2 tag that begins with these ten bytes: its
// header, its body and its footer if it has one. 0 if they do not begin an
// ID3v2 tag. At most 268435475 (a 28-bit body plus header and footer).
uint32_t id3v2_tag_bytes(const uint8_t header[ID3V2_HEADER_BYTES]);

// True if these three bytes, read 128 bytes before the end of a file, begin
// an ID3v1 tag.
bool id3v1_tag_begins(const uint8_t marker[ID3V1_MARKER_BYTES]);
