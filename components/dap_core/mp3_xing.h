#pragma once

#include <stddef.h>
#include <stdint.h>

// How long an MP3 is, from the Xing/Info header.
//
// An MP3 stream has no header of its own; it is a row of frames, each with
// its own bit rate. Encoders therefore put a frame in front that holds no
// sound but says how many frames follow: tagged "Xing" in a variable bit rate
// file, "Info" in a constant bit rate one. With it the length is known
// before anything is decoded, whatever the bit rates of the frames. Without
// it the length can only be estimated from the file's size and one frame's
// bit rate, which is right for constant bit rate files only.

// The most bytes this needs: a frame header (4), the largest side
// information block (32), then the tag, its flags and the frame count (4
// each).
#define MP3_XING_PROBE_BYTES 48

// `frame` is the first `len` bytes of the audio, where the first frame is
// expected to start. Returns the length in milliseconds that the Xing/Info
// header states (frames x samples per frame / sample rate), or 0 if those
// bytes are not a Layer III frame holding such a header with a frame count.
// A length that does not fit is reported as UINT32_MAX.
//
// The count includes the encoder's padding at both ends (about 30 ms in a
// 44.1 kHz file), so the result is that much longer than what is heard.
uint32_t mp3_xing_duration_ms(const uint8_t *frame, size_t len);
