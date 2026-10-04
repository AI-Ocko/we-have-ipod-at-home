#pragma once

#include <stddef.h>
#include <stdint.h>

// CRC-32 as in zlib, PNG and Ethernet: polynomial 0x04C11DB7 reflected
// (0xEDB88320), start value and final XOR 0xFFFFFFFF, bytes taken low bit
// first. The check value for "123456789" is 0xCBF43926.
//
// Start with 0 and feed the bytes in pieces, in order: the result for `a`
// then `b` is crc32_update(crc32_update(0, a, n), b, m). The start value and
// final XOR are applied inside, so every result is a finished checksum.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
