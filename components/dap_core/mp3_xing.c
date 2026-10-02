#include "mp3_xing.h"

#include <stdbool.h>
#include <string.h>

#define FRAME_HEADER_BYTES 4
#define VERSION_MPEG1 3
#define VERSION_RESERVED 1
#define LAYER_III 1
#define RATE_RESERVED 3
#define MODE_MONO 3
#define XING_FLAG_FRAMES 0x01
#define BITRATE_INVALID 15

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

// True if `p` begins with a frame header this module understands.
//
// Frame header: 11 bits of sync, version (2), layer (2), protection (1);
// bit rate (4), sample rate (2), padding (1), private (1); channel mode (2)
// and five bits that do not matter here.
static bool layer3_header(const uint8_t *p, size_t len) {
  if (len < FRAME_HEADER_BYTES) {
    return false;
  }
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
    return false;
  }
  unsigned version = (p[1] >> 3) & 0x03;
  unsigned layer = (p[1] >> 1) & 0x03;
  unsigned rate_index = (p[2] >> 2) & 0x03;
  return version != VERSION_RESERVED && layer == LAYER_III &&
         rate_index != RATE_RESERVED;
}

uint32_t mp3_xing_duration_ms(const uint8_t *p, size_t len) {
  if (!layer3_header(p, len)) {
    return 0;
  }
  unsigned version = (p[1] >> 3) & 0x03;
  unsigned rate_index = (p[2] >> 2) & 0x03;
  unsigned mode = p[3] >> 6;

  // MPEG-1 rates; MPEG-2 halves them and MPEG-2.5 halves them again, and
  // both of those put half as many samples in a frame.
  static const uint16_t RATE[3] = {44100, 48000, 32000};
  uint32_t rate = RATE[rate_index];
  uint32_t samples_per_frame = 1152;
  if (version != VERSION_MPEG1) {
    rate >>= (version == 0) ? 2 : 1; // 0 is MPEG-2.5, 2 is MPEG-2
    samples_per_frame = 576;
  }

  // The tag comes after the side information, whose size depends on the
  // version and on mono or not. (An empty frame's side information is all
  // zeros, which is why older players play this frame as silence.)
  size_t side;
  if (version == VERSION_MPEG1) {
    side = (mode == MODE_MONO) ? 17 : 32;
  } else {
    side = (mode == MODE_MONO) ? 9 : 17;
  }
  const uint8_t *tag = p + FRAME_HEADER_BYTES + side;
  if (len < FRAME_HEADER_BYTES + side + 12) {
    return 0;
  }
  if (memcmp(tag, "Xing", 4) != 0 && memcmp(tag, "Info", 4) != 0) {
    return 0;
  }
  // The frame count is the first optional field, there if its flag is set.
  if ((be32(tag + 4) & XING_FLAG_FRAMES) == 0) {
    return 0;
  }
  uint64_t samples = (uint64_t)be32(tag + 8) * samples_per_frame; // < 2^43
  uint64_t ms = samples * 1000 / rate;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

bool mp3_frame_header(const uint8_t *p, size_t len, uint32_t *bitrate) {
  *bitrate = 0;
  if (!layer3_header(p, len)) {
    return false;
  }
  // Kilobits per second by index. Index 0 is "free format": any rate the
  // encoder liked, not stated in the header. Index 15 is not allowed, so a
  // header with it is not a header.
  static const uint16_t MPEG1_KBPS[15] = {0,   32,  40,  48,  56,  64,  80, 96,
                                          112, 128, 160, 192, 224, 256, 320};
  static const uint16_t MPEG2_KBPS[15] = {0,  8,  16, 24,  32,  40,  48, 56,
                                          64, 80, 96, 112, 128, 144, 160};
  unsigned version = (p[1] >> 3) & 0x03;
  unsigned index = p[2] >> 4;
  if (index == BITRATE_INVALID) {
    return false;
  }
  // MPEG-2 and MPEG-2.5 share a table.
  *bitrate = 1000u *
             (version == VERSION_MPEG1 ? MPEG1_KBPS[index] : MPEG2_KBPS[index]);
  return true;
}
