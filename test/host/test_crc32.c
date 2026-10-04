#include "crc32.h"
#include "unity.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// CRC-32 one bit at a time, written from the definition and sharing nothing
// with the code under test.
static uint32_t crc32_bitwise(const uint8_t *p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

// Every byte value at least twice, in an order that is not a simple count.
static uint8_t bytes[600];

static void fill_bytes(void) {
  uint32_t x = 12345;
  for (size_t i = 0; i < sizeof(bytes); i++) {
    bytes[i] = (i < 256) ? (uint8_t)i : (uint8_t)(x >> 24);
    x = x * 1103515245u + 12345u;
  }
}

void test_check_value(void) {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_update(0, "123456789", 9));
  // Every entry of a table, if there is one, against the definition.
  fill_bytes();
  for (size_t n = 1; n <= sizeof(bytes); n += 37) {
    TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(bytes, n),
                            crc32_update(0, bytes, n));
  }
  TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(bytes, sizeof(bytes)),
                          crc32_update(0, bytes, sizeof(bytes)));
  // One byte of each value.
  for (unsigned v = 0; v < 256; v++) {
    uint8_t b = (uint8_t)v;
    TEST_ASSERT_EQUAL_HEX32(crc32_bitwise(&b, 1), crc32_update(0, &b, 1));
  }
}

void test_pieces_equal_the_whole(void) {
  fill_bytes();
  uint32_t whole = crc32_update(0, bytes, sizeof(bytes));
  // Two pieces, split at every place.
  for (size_t cut = 0; cut <= sizeof(bytes); cut++) {
    uint32_t crc = crc32_update(0, bytes, cut);
    crc = crc32_update(crc, bytes + cut, sizeof(bytes) - cut);
    TEST_ASSERT_EQUAL_HEX32(whole, crc);
  }
  // Many pieces of one byte.
  uint32_t crc = 0;
  for (size_t i = 0; i < sizeof(bytes); i++) {
    crc = crc32_update(crc, bytes + i, 1);
  }
  TEST_ASSERT_EQUAL_HEX32(whole, crc);
  // "1234" then "56789".
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                          crc32_update(crc32_update(0, "1234", 4), "56789", 5));
}

void test_empty_is_zero(void) {
  TEST_ASSERT_EQUAL_HEX32(0, crc32_update(0, "", 0));
  TEST_ASSERT_EQUAL_HEX32(0, crc32_update(0, NULL, 0));
  // No bytes leave any checksum as it was.
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_update(0xCBF43926u, "x", 0));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, crc32_update(0xFFFFFFFFu, NULL, 0));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_check_value);
  RUN_TEST(test_pieces_equal_the_whole);
  RUN_TEST(test_empty_is_zero);
  return UNITY_END();
}
