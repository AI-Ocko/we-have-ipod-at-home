#include "id3.h"

#define ID3V2_FLAG_FOOTER 0x10

uint32_t id3v2_tag_bytes(const uint8_t h[ID3V2_HEADER_BYTES]) {
  // "ID3", version, revision, flags, then the body's length as four 7-bit
  // bytes. The standard gives the pattern a real header must match: neither
  // version byte is 0xFF and no length byte has its top bit set. Anything
  // else is audio (or rubbish) that happens to contain the letters.
  if (h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] == 0xFF ||
      h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80) != 0) {
    return 0;
  }
  uint32_t body = ((uint32_t)h[6] << 21) | ((uint32_t)h[7] << 14) |
                  ((uint32_t)h[8] << 7) | h[9];
  // An ID3v2.4 footer is a second copy of the header after the body; the
  // length does not count it.
  uint32_t footer = (h[5] & ID3V2_FLAG_FOOTER) ? ID3V2_HEADER_BYTES : 0;
  return ID3V2_HEADER_BYTES + body + footer;
}

bool id3v1_tag_begins(const uint8_t m[ID3V1_MARKER_BYTES]) {
  return m[0] == 'T' && m[1] == 'A' && m[2] == 'G';
}
