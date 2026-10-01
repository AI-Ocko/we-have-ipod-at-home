// fopencookie (used to count the reads and seeks the parser makes) is a GNU
// extension.
#define _GNU_SOURCE

#include "flac_header.h"
#include "unity.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

// The file a test has open. tearDown closes it, so a failed assertion (which
// jumps out of the test) cannot leak it and trip the leak checker.
static FILE *open_file;

void setUp(void) { open_file = NULL; }

void tearDown(void) {
  if (open_file != NULL) {
    fclose(open_file);
    open_file = NULL;
  }
}

static FILE *open_fixture(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(open_file, path);
  return open_file;
}

// A temporary file holding `n` bytes, positioned at its start.
static FILE *file_of(const uint8_t *bytes, size_t n) {
  if (open_file != NULL) {
    fclose(open_file);
  }
  open_file = tmpfile();
  TEST_ASSERT_NOT_NULL(open_file);
  if (n > 0) {
    TEST_ASSERT_EQUAL_size_t(n, fwrite(bytes, 1, n, open_file));
  }
  rewind(open_file);
  return open_file;
}

// --- Building small FLAC files by hand -------------------------------------

static uint8_t img[512];
static size_t img_len;

static void img_reset(void) {
  memset(img, 0, sizeof(img));
  img_len = 0;
}

static void img_bytes(const void *p, size_t n) {
  TEST_ASSERT_TRUE(img_len + n <= sizeof(img));
  memcpy(&img[img_len], p, n);
  img_len += n;
}

static void img_zeros(size_t n) {
  TEST_ASSERT_TRUE(img_len + n <= sizeof(img));
  img_len += n; // img_reset already zeroed it
}

static void img_marker(void) { img_bytes("fLaC", 4); }

static void img_block_header(int last, int type, uint32_t length) {
  uint8_t h[4] = {(uint8_t)((last ? 0x80 : 0) | type), (uint8_t)(length >> 16),
                  (uint8_t)(length >> 8), (uint8_t)length};
  img_bytes(h, 4);
}

static void img_id3_header(uint8_t flags, uint32_t size) {
  uint8_t h[10] = {'I',
                   'D',
                   '3',
                   4,
                   0,
                   flags,
                   (uint8_t)((size >> 21) & 0x7F),
                   (uint8_t)((size >> 14) & 0x7F),
                   (uint8_t)((size >> 7) & 0x7F),
                   (uint8_t)(size & 0x7F)};
  img_bytes(h, 10);
}

// The 34-byte STREAMINFO body. The frame-size fields get distinct values so a
// parser reading the wrong one is caught.
static void img_streaminfo_body(uint32_t rate, unsigned channels, unsigned bits,
                                uint64_t total, uint32_t max_frame) {
  uint8_t b[34] = {0};
  b[0] = 0x10; // min block size 4096
  b[2] = 0x10; // max block size 4096
  b[4] = 0x00; // min frame size 0x000E0F
  b[5] = 0x0E;
  b[6] = 0x0F;
  b[7] = (uint8_t)(max_frame >> 16);
  b[8] = (uint8_t)(max_frame >> 8);
  b[9] = (uint8_t)max_frame;
  b[10] = (uint8_t)(rate >> 12);
  b[11] = (uint8_t)(rate >> 4);
  b[12] = (uint8_t)(((rate & 0xF) << 4) | ((channels - 1) << 1) |
                    ((bits - 1) >> 4));
  b[13] = (uint8_t)((((bits - 1) & 0xF) << 4) | ((total >> 32) & 0xF));
  b[14] = (uint8_t)(total >> 24);
  b[15] = (uint8_t)(total >> 16);
  b[16] = (uint8_t)(total >> 8);
  b[17] = (uint8_t)total;
  memset(&b[18], 0xFF, 16); // MD5: all ones, so it cannot pass for zeros
  img_bytes(b, 34);
}

// Marker plus a normal 44.1 kHz STREAMINFO block.
static void img_start(int streaminfo_is_last) {
  img_reset();
  img_marker();
  img_block_header(streaminfo_is_last, 0, 34);
  img_streaminfo_body(44100, 2, 16, 4410, 2491);
}

static FILE *img_file(void) { return file_of(img, img_len); }

// --- Fixtures ---------------------------------------------------------------

static void assert_44k_16_stereo(const flac_header_t *h) {
  TEST_ASSERT_EQUAL_UINT32(44100, h->sample_rate);
  TEST_ASSERT_EQUAL_UINT8(2, h->channels);
  TEST_ASSERT_EQUAL_UINT8(16, h->bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64(4410, h->total_frames);
  TEST_ASSERT_EQUAL_UINT32(2491, h->max_frame_bytes);
}

static void test_reads_44k_16_stereo(void) {
  flac_header_t h;
  memset(&h, 0xAA, sizeof(h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(open_fixture("f_44k_16_st.flac"), &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_TRUE(h.max_frame_bytes > 0);
  TEST_ASSERT_TRUE(h.audio_start > 42);
  // fLaC, STREAMINFO (4 + 34), then a 40-byte comment block (4 + 40).
  TEST_ASSERT_EQUAL_INT64(86, h.audio_start);
}

static void test_reads_96k_24_stereo(void) {
  flac_header_t h;
  memset(&h, 0xAA, sizeof(h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(open_fixture("f_96k_24_st.flac"), &h));
  TEST_ASSERT_EQUAL_UINT32(96000, h.sample_rate);
  TEST_ASSERT_EQUAL_UINT8(2, h.channels);
  TEST_ASSERT_EQUAL_UINT8(24, h.bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64(9600, h.total_frames);
  TEST_ASSERT_EQUAL_UINT32(9112, h.max_frame_bytes);
  TEST_ASSERT_EQUAL_INT64(130, h.audio_start);
}

static void test_reads_48k_16_mono(void) {
  flac_header_t h;
  memset(&h, 0xAA, sizeof(h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(open_fixture("f_48k_16_mono.flac"), &h));
  TEST_ASSERT_EQUAL_UINT32(48000, h.sample_rate);
  TEST_ASSERT_EQUAL_UINT8(1, h.channels);
  TEST_ASSERT_EQUAL_UINT8(16, h.bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64(4800, h.total_frames);
  TEST_ASSERT_EQUAL_UINT32(1493, h.max_frame_bytes);
  TEST_ASSERT_EQUAL_INT64(86, h.audio_start);
}

static void test_audio_start_is_past_a_picture_block(void) {
  flac_header_t h;
  memset(&h, 0xAA, sizeof(h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(open_fixture("f_with_picture.flac"), &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_TRUE(h.audio_start >= 20000);
  // As the plain file, plus the picture block (4 + 27946).
  TEST_ASSERT_EQUAL_INT64(86 + 4 + 27946, h.audio_start);
}

static void test_audio_start_points_at_a_frame_sync(void) {
  static const char *const names[] = {
      "f_44k_16_st.flac",    "f_96k_24_st.flac", "f_48k_16_mono.flac",
      "f_with_picture.flac", "f_with_id3.flac",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    FILE *f = open_fixture(names[i]);
    flac_header_t h;
    TEST_ASSERT_EQUAL_MESSAGE(FLAC_HEADER_OK, flac_header_read(f, &h),
                              names[i]);
    uint8_t sync[2] = {0, 0};
    TEST_ASSERT_EQUAL_MESSAGE(0, fseek(f, h.audio_start, SEEK_SET), names[i]);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, fread(sync, 1, 2, f), names[i]);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, sync[0], names[i]);
    TEST_ASSERT_TRUE_MESSAGE(sync[1] == 0xF8 || sync[1] == 0xF9, names[i]);
  }
}

static void test_skips_a_leading_id3_tag(void) {
  flac_header_t h;
  memset(&h, 0xAA, sizeof(h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(open_fixture("f_with_id3.flac"), &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_TRUE(h.audio_start >= 1034);
  TEST_ASSERT_EQUAL_INT64(1024 + 86, h.audio_start);
}

static void test_rejects_a_file_that_is_not_flac(void) {
  flac_header_t h;
  TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC,
                    flac_header_read(open_fixture("f_not_flac.bin"), &h));
}

static void test_reports_a_truncated_header(void) {
  flac_header_t h;
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED,
                    flac_header_read(open_fixture("f_truncated.flac"), &h));
}

static void test_reports_an_empty_file(void) {
  flac_header_t h;
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED,
                    flac_header_read(file_of(NULL, 0), &h));
}

static void test_duration(void) {
  flac_header_t h;
  memset(&h, 0, sizeof(h));

  h.sample_rate = 44100;
  h.total_frames = 441000;
  TEST_ASSERT_EQUAL_UINT32(10000, flac_header_duration_ms(&h));

  h.total_frames = 0;
  TEST_ASSERT_EQUAL_UINT32(0, flac_header_duration_ms(&h));

  h.sample_rate = 0;
  h.total_frames = 441000;
  TEST_ASSERT_EQUAL_UINT32(0, flac_header_duration_ms(&h));

  // The longest length the format can state, at a low rate, is about 99 days:
  // more milliseconds than 32 bits hold. It must saturate, not wrap around.
  h.sample_rate = 8000;
  h.total_frames = (UINT64_C(1) << 36) - 1;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, flac_header_duration_ms(&h));

  // The largest length that still fits is reported exactly: 2^32 - 1 ms at
  // 8 kHz is (2^32 - 1) * 8 frames.
  h.total_frames = UINT64_C(0xFFFFFFFF) * 8;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, flac_header_duration_ms(&h));
  h.total_frames -= 1;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, flac_header_duration_ms(&h));

  // The struct can hold more than the format's 36 bits; that must not wrap
  // either.
  h.total_frames = UINT64_MAX;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, flac_header_duration_ms(&h));
  h.sample_rate = UINT32_MAX;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, flac_header_duration_ms(&h));

  // Part of a millisecond is dropped, not rounded up.
  h.sample_rate = 44100;
  h.total_frames = 4410;
  TEST_ASSERT_EQUAL_UINT32(100, flac_header_duration_ms(&h));
  h.total_frames = 4409;
  TEST_ASSERT_EQUAL_UINT32(99, flac_header_duration_ms(&h));
}

// --- Further cases (not in the brief's list) --------------------------------

// Each bit-packed field at a value with every bit set differently from its
// neighbours, so bits leaking across a field boundary show up.
static void test_unpacks_fields_that_straddle_bytes(void) {
  flac_header_t h;

  img_reset();
  img_marker();
  img_block_header(1, 0, 34);
  img_streaminfo_body(0xFFFFF, 1, 32, 0, 0xFFFFFF);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFF, h.sample_rate);
  TEST_ASSERT_EQUAL_UINT8(1, h.channels);
  TEST_ASSERT_EQUAL_UINT8(32, h.bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64(0, h.total_frames);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFF, h.max_frame_bytes);

  img_reset();
  img_marker();
  img_block_header(1, 0, 34);
  img_streaminfo_body(0, 8, 1, (UINT64_C(1) << 36) - 1, 0);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  TEST_ASSERT_EQUAL_UINT32(0, h.sample_rate);
  TEST_ASSERT_EQUAL_UINT8(8, h.channels);
  TEST_ASSERT_EQUAL_UINT8(1, h.bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64((UINT64_C(1) << 36) - 1, h.total_frames);
  TEST_ASSERT_EQUAL_UINT32(0, h.max_frame_bytes);

  img_reset();
  img_marker();
  img_block_header(1, 0, 34);
  img_streaminfo_body(0x12345, 6, 20, UINT64_C(0x9ABCDEF01), 0x654321);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  TEST_ASSERT_EQUAL_UINT32(0x12345, h.sample_rate);
  TEST_ASSERT_EQUAL_UINT8(6, h.channels);
  TEST_ASSERT_EQUAL_UINT8(20, h.bits_per_sample);
  TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x9ABCDEF01), h.total_frames);
  TEST_ASSERT_EQUAL_UINT32(0x654321, h.max_frame_bytes);
}

// A stream whose only metadata block is STREAMINFO and which has no audio
// frames at all is complete: the header ends where the file does.
static void test_accepts_a_header_with_no_audio_after_it(void) {
  flac_header_t h;
  img_start(1);
  TEST_ASSERT_EQUAL_size_t(42, img_len);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_EQUAL_INT64(42, h.audio_start);
}

static void test_reads_from_the_start_whatever_the_position(void) {
  flac_header_t h;
  FILE *f = open_fixture("f_44k_16_st.flac");
  TEST_ASSERT_EQUAL(0, fseek(f, 1000, SEEK_SET));
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(f, &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_EQUAL_INT64(86, h.audio_start);
}

static void test_reports_a_file_that_ends_right_after_the_marker(void) {
  flac_header_t h;
  img_reset();
  img_marker();
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));
}

// A file too short to hold the marker is "truncated" only while what there is
// of it could still be the start of a FLAC file or of an ID3 tag.
static void test_a_short_file_is_judged_by_the_bytes_it_has(void) {
  flac_header_t h;
  const uint8_t part_marker[] = {'f', 'L', 'a'};
  const uint8_t part_id3[] = {'I', 'D'};
  const uint8_t id3_cut[] = {'I', 'D', '3', 4, 0, 0, 0};
  const uint8_t other[] = {'R', 'I'};
  const uint8_t near_miss[] = {'f', 'L', 'A'};
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED,
                    flac_header_read(file_of(part_marker, 3), &h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED,
                    flac_header_read(file_of(part_id3, 2), &h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED,
                    flac_header_read(file_of(id3_cut, 7), &h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC,
                    flac_header_read(file_of(other, 2), &h));
  TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC,
                    flac_header_read(file_of(near_miss, 3), &h));
}

static void test_reports_a_header_cut_at_every_length(void) {
  // marker, STREAMINFO, a 6-byte block, a last block of 5 bytes, two bytes
  // of audio: 4 + 38 + 10 + 9 + 2.
  img_start(0);
  img_block_header(0, 4, 6);
  img_zeros(6);
  img_block_header(1, 1, 5);
  img_zeros(5);
  const size_t header_len = img_len;
  TEST_ASSERT_EQUAL_size_t(61, header_len);
  img_bytes("\xFF\xF8", 2);

  static uint8_t whole[sizeof(img)];
  memcpy(whole, img, sizeof(img));
  for (size_t n = 0; n < header_len; n++) {
    flac_header_t h;
    char msg[32];
    snprintf(msg, sizeof(msg), "cut to %u bytes", (unsigned)n);
    TEST_ASSERT_EQUAL_MESSAGE(FLAC_HEADER_TRUNCATED,
                              flac_header_read(file_of(whole, n), &h), msg);
  }
  flac_header_t h;
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(file_of(whole, header_len), &h));
  TEST_ASSERT_EQUAL_INT64(61, h.audio_start);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK,
                    flac_header_read(file_of(whole, header_len + 2), &h));
  TEST_ASSERT_EQUAL_INT64(61, h.audio_start);
}

static void test_reports_an_id3_size_past_the_end_of_the_file(void) {
  flac_header_t h;
  img_reset();
  img_id3_header(0, 0x0FFFFFFF); // the largest size the field can hold
  img_marker();
  img_block_header(1, 0, 34);
  img_streaminfo_body(44100, 2, 16, 4410, 2491);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));

  // One byte short of reaching the marker's last byte.
  img_reset();
  img_id3_header(0, 30);
  img_zeros(26);
  img_bytes("fLa", 3);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));
}

static void test_reports_a_block_length_past_the_end_of_the_file(void) {
  flac_header_t h;

  // A block in the middle that claims the largest length the field can hold.
  img_start(0);
  img_block_header(0, 6, 0xFFFFFF);
  img_zeros(100);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));

  // The same for the last block, which has no block header after it to miss.
  img_start(0);
  img_block_header(1, 6, 0xFFFFFF);
  img_zeros(100);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));

  // One byte short.
  img_start(0);
  img_block_header(1, 6, 101);
  img_zeros(100);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));
}

// Blocks with no body make no progress through the file except by their own
// headers; the walk must still end when the file does.
static void test_a_run_of_empty_blocks_ends_at_the_end_of_the_file(void) {
  flac_header_t h;
  img_start(0);
  for (int i = 0; i < 50; i++) {
    img_block_header(0, 1, 0);
  }
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));

  img_block_header(1, 1, 0);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  TEST_ASSERT_EQUAL_INT64(42 + 51 * 4, h.audio_start);
}

static void test_rejects_a_first_block_that_is_not_streaminfo(void) {
  flac_header_t h;
  img_reset();
  img_marker();
  img_block_header(0, 4, 34);
  img_streaminfo_body(44100, 2, 16, 4410, 2491);
  img_block_header(1, 0, 34);
  img_streaminfo_body(44100, 2, 16, 4410, 2491);
  TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC, flac_header_read(img_file(), &h));
}

static void test_rejects_a_streaminfo_of_the_wrong_length(void) {
  flac_header_t h;
  static const uint32_t lengths[] = {0, 33, 35, 0x220000};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    img_reset();
    img_marker();
    img_block_header(1, 0, lengths[i]);
    img_streaminfo_body(44100, 2, 16, 4410, 2491);
    img_zeros(8);
    TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC, flac_header_read(img_file(), &h));
  }
}

static void test_skips_an_id3_tag_that_has_a_footer(void) {
  flac_header_t h;
  img_reset();
  img_id3_header(0x10, 20); // flag bit 4: a 10-byte footer follows the tag
  img_zeros(20);
  img_bytes("3DI\x04\x00\x10\x00\x00\x00\x14", 10);
  img_marker();
  img_block_header(1, 0, 34);
  img_streaminfo_body(44100, 2, 16, 4410, 2491);
  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, flac_header_read(img_file(), &h));
  assert_44k_16_stereo(&h);
  TEST_ASSERT_EQUAL_INT64(10 + 20 + 10 + 42, h.audio_start);
}

static void test_rejects_an_id3_tag_followed_by_something_else(void) {
  flac_header_t h;
  img_reset();
  img_id3_header(0, 16);
  img_zeros(16);
  img_bytes("\xFF\xFB\x90\x00 an MP3, say", 16);
  TEST_ASSERT_EQUAL(FLAC_HEADER_NOT_FLAC, flac_header_read(img_file(), &h));
}

static void test_leaves_the_result_alone_on_failure(void) {
  flac_header_t h;
  flac_header_t before;
  memset(&h, 0xAA, sizeof(h));
  memset(&before, 0xAA, sizeof(before));
  // Fails after STREAMINFO has been read, when there is something to leak.
  img_start(0);
  TEST_ASSERT_EQUAL(FLAC_HEADER_TRUNCATED, flac_header_read(img_file(), &h));
  TEST_ASSERT_EQUAL_MEMORY(&before, &h, sizeof(h));
}

// --- What the parser costs on a slow card ----------------------------------

typedef struct {
  FILE *f;
  unsigned long bytes;
  unsigned seeks;
} counted_t;

static ssize_t counted_read(void *cookie, char *buf, size_t size) {
  counted_t *c = cookie;
  size_t n = fread(buf, 1, size, c->f);
  c->bytes += n;
  return (ssize_t)n;
}

static int counted_seek(void *cookie, off64_t *offset, int whence) {
  counted_t *c = cookie;
  c->seeks++;
  if (fseeko(c->f, (off_t)*offset, whence) != 0) {
    return -1;
  }
  *offset = ftello(c->f);
  return 0;
}

// The picture is 27946 bytes of a 30767-byte file. Reading it instead of
// seeking over it would show up here as thousands of bytes.
static void test_steps_over_a_picture_without_reading_it(void) {
  counted_t c = {open_fixture("f_with_picture.flac"), 0, 0};
  cookie_io_functions_t io = {counted_read, NULL, counted_seek, NULL};
  FILE *f = fopencookie(&c, "rb", io);
  TEST_ASSERT_NOT_NULL(f);
  // Unbuffered, so every request the parser makes reaches the counters as is.
  setvbuf(f, NULL, _IONBF, 0);

  flac_header_t h;
  flac_header_result_t r = flac_header_read(f, &h);
  fclose(f);

  TEST_ASSERT_EQUAL(FLAC_HEADER_OK, r);
  TEST_ASSERT_EQUAL_INT64(28036, h.audio_start);
  printf("f_with_picture.flac: %lu bytes read, %u seeks\n", c.bytes, c.seeks);
  // 4 marker + 4 + 34 STREAMINFO + two more 4-byte block headers, and at most
  // a byte to prove the last block is all there.
  TEST_ASSERT_TRUE(c.bytes <= 51);
  // Back to the start, over the comment block, over the picture.
  TEST_ASSERT_TRUE(c.seeks <= 3);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_44k_16_stereo);
  RUN_TEST(test_reads_96k_24_stereo);
  RUN_TEST(test_reads_48k_16_mono);
  RUN_TEST(test_audio_start_is_past_a_picture_block);
  RUN_TEST(test_audio_start_points_at_a_frame_sync);
  RUN_TEST(test_skips_a_leading_id3_tag);
  RUN_TEST(test_rejects_a_file_that_is_not_flac);
  RUN_TEST(test_reports_a_truncated_header);
  RUN_TEST(test_reports_an_empty_file);
  RUN_TEST(test_duration);
  RUN_TEST(test_unpacks_fields_that_straddle_bytes);
  RUN_TEST(test_accepts_a_header_with_no_audio_after_it);
  RUN_TEST(test_reads_from_the_start_whatever_the_position);
  RUN_TEST(test_reports_a_file_that_ends_right_after_the_marker);
  RUN_TEST(test_a_short_file_is_judged_by_the_bytes_it_has);
  RUN_TEST(test_reports_a_header_cut_at_every_length);
  RUN_TEST(test_reports_an_id3_size_past_the_end_of_the_file);
  RUN_TEST(test_reports_a_block_length_past_the_end_of_the_file);
  RUN_TEST(test_a_run_of_empty_blocks_ends_at_the_end_of_the_file);
  RUN_TEST(test_rejects_a_first_block_that_is_not_streaminfo);
  RUN_TEST(test_rejects_a_streaminfo_of_the_wrong_length);
  RUN_TEST(test_skips_an_id3_tag_that_has_a_footer);
  RUN_TEST(test_rejects_an_id3_tag_followed_by_something_else);
  RUN_TEST(test_leaves_the_result_alone_on_failure);
  RUN_TEST(test_steps_over_a_picture_without_reading_it);
  return UNITY_END();
}
