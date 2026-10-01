#include "flac_header.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define ID3_HEADER_LEN 10
#define ID3_FLAG_FOOTER 0x10
#define BLOCK_HEADER_LEN 4
#define BLOCK_LAST 0x80
#define BLOCK_TYPE_MASK 0x7F
#define BLOCK_TYPE_STREAMINFO 0
#define STREAMINFO_LEN 34

static const char MARKER[4] = {'f', 'L', 'a', 'C'};

static bool read_exact(FILE *f, uint8_t *buf, size_t n) {
  return fread(buf, 1, n, f) == n;
}

// Offsets are added up in 64 bits and checked here, because `long` is 32 bits
// on the ESP32: an ID3 size and a few block lengths from a damaged file could
// otherwise wrap round to a small, plausible offset.
static bool seek_to(FILE *f, uint64_t pos) {
  return pos <= (uint64_t)LONG_MAX && fseek(f, (long)pos, SEEK_SET) == 0;
}

static uint32_t be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

// What to make of a file that ended `got` bytes into a 4-byte marker. It is
// "truncated" only if the bytes it does have are the start of the marker;
// anything else is simply some other kind of file.
static flac_header_result_t short_marker(const uint8_t *got_bytes, size_t got,
                                         bool id3_allowed) {
  if (memcmp(got_bytes, MARKER, got) == 0) {
    return FLAC_HEADER_TRUNCATED;
  }
  if (id3_allowed && got < 3 && memcmp(got_bytes, "ID3", got) == 0) {
    return FLAC_HEADER_TRUNCATED;
  }
  return FLAC_HEADER_NOT_FLAC;
}

flac_header_result_t flac_header_read(FILE *f, flac_header_t *out) {
  uint8_t b[STREAMINFO_LEN];
  uint64_t pos = 0; // where the next unread header byte is

  if (!seek_to(f, 0)) {
    return FLAC_HEADER_TRUNCATED;
  }
  size_t got = fread(b, 1, sizeof(MARKER), f);
  if (got < sizeof(MARKER)) {
    return short_marker(b, got, true);
  }

  if (memcmp(b, "ID3", 3) == 0) {
    if (!read_exact(f, &b[4], ID3_HEADER_LEN - 4)) {
      return FLAC_HEADER_TRUNCATED;
    }
    // Four 7-bit bytes. The top bits are masked off so that a damaged size
    // still cannot exceed 28 bits.
    uint32_t size = ((uint32_t)(b[6] & 0x7F) << 21) |
                    ((uint32_t)(b[7] & 0x7F) << 14) |
                    ((uint32_t)(b[8] & 0x7F) << 7) | (b[9] & 0x7F);
    pos = ID3_HEADER_LEN + (uint64_t)size;
    if (b[5] & ID3_FLAG_FOOTER) {
      // An ID3v2.4 footer is a second copy of the header after the tag, not
      // counted in the size.
      pos += ID3_HEADER_LEN;
    }
    if (!seek_to(f, pos)) {
      return FLAC_HEADER_TRUNCATED;
    }
    got = fread(b, 1, sizeof(MARKER), f);
    if (got < sizeof(MARKER)) {
      return short_marker(b, got, false);
    }
  }

  if (memcmp(b, MARKER, sizeof(MARKER)) != 0) {
    return FLAC_HEADER_NOT_FLAC;
  }
  pos += sizeof(MARKER);

  // STREAMINFO must be the first block and is always 34 bytes. A file that
  // breaks either rule is not one a FLAC decoder would accept, so it is
  // reported as not FLAC rather than searched for a later STREAMINFO.
  if (!read_exact(f, b, BLOCK_HEADER_LEN)) {
    return FLAC_HEADER_TRUNCATED;
  }
  bool last = (b[0] & BLOCK_LAST) != 0;
  if ((b[0] & BLOCK_TYPE_MASK) != BLOCK_TYPE_STREAMINFO ||
      be24(&b[1]) != STREAMINFO_LEN) {
    return FLAC_HEADER_NOT_FLAC;
  }
  if (!read_exact(f, b, STREAMINFO_LEN)) {
    return FLAC_HEADER_TRUNCATED;
  }
  pos += BLOCK_HEADER_LEN + STREAMINFO_LEN;

  // Bytes 0..9: block sizes (16 + 16 bits), frame sizes (24 + 24 bits).
  // Bytes 10..17, bit-packed: rate (20), channels - 1 (3), bits - 1 (5),
  // total samples (36). The rest is the MD5 of the audio.
  flac_header_t h;
  h.max_frame_bytes = be24(&b[7]);
  h.sample_rate =
      ((uint32_t)b[10] << 12) | ((uint32_t)b[11] << 4) | (b[12] >> 4);
  h.channels = (uint8_t)(((b[12] >> 1) & 0x07) + 1);
  h.bits_per_sample = (uint8_t)((((b[12] & 0x01) << 4) | (b[13] >> 4)) + 1);
  h.total_frames = ((uint64_t)(b[13] & 0x0F) << 32) |
                   ((uint64_t)be24(&b[14]) << 8) | b[17];

  // Step over the remaining blocks. Every pass consumes four bytes of the
  // file for a block header, so this ends at the end of the file at the
  // latest, whatever the lengths say.
  while (!last) {
    if (!read_exact(f, b, BLOCK_HEADER_LEN)) {
      return FLAC_HEADER_TRUNCATED;
    }
    last = (b[0] & BLOCK_LAST) != 0;
    uint32_t len = be24(&b[1]);
    pos += BLOCK_HEADER_LEN + (uint64_t)len;
    if (len == 0) {
      continue;
    }
    if (!last) {
      // A length that runs past the end of the file shows up as a failed
      // read of the next block header.
      if (!seek_to(f, pos)) {
        return FLAC_HEADER_TRUNCATED;
      }
    } else {
      // Nothing more is read after the last block, so a length that runs
      // past the end of the file would go unnoticed. Land on the block's
      // final byte instead of just after it and read that one byte: still a
      // single seek, and it proves the whole header is in the file.
      if (!seek_to(f, pos - 1) || !read_exact(f, b, 1)) {
        return FLAC_HEADER_TRUNCATED;
      }
    }
  }

  if (pos > (uint64_t)LONG_MAX) {
    return FLAC_HEADER_TRUNCATED;
  }
  h.audio_start = (long)pos;
  *out = h;
  return FLAC_HEADER_OK;
}

uint32_t flac_header_duration_ms(const flac_header_t *h) {
  if (h->total_frames == 0 || h->sample_rate == 0) {
    return 0;
  }
  // Whole seconds and the remainder are converted separately so that no
  // total_frames value can overflow the multiplication by 1000.
  uint64_t seconds = h->total_frames / h->sample_rate;
  uint64_t rest = h->total_frames % h->sample_rate; // below 2^32
  if (seconds > UINT32_MAX / 1000) {
    return UINT32_MAX;
  }
  uint64_t ms = seconds * 1000 + rest * 1000 / h->sample_rate;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
