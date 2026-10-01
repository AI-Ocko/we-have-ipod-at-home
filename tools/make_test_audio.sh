#!/usr/bin/env bash
# Generate the deterministic test audio set and the host test fixtures.
#
# Usage: tools/make_test_audio.sh
#
#   test-audio/          t01..t10, for the SD card's /test/ folder (not committed)
#   test/host/fixtures/  tiny files for the host unit tests (committed)
#
# Every tone comes from ffmpeg's `sine` source (integer maths, -18 dBFS, no
# dither), is trimmed to an exact sample count and encoded by the reference
# `flac` encoder, so a re-run with the same tool versions gives the same bytes.
# The one noise burst (t10) comes from ffmpeg's `anoisesrc` with a fixed seed.
# The script checks its own output and exits non-zero if anything is off.
set -euo pipefail

if [ $# -ne 0 ]; then
  sed -n '2,7p' "$0" | cut -c3-
  case $1 in -h|--help) exit 0 ;; *) exit 2 ;; esac
fi

die() { echo "make_test_audio: ERROR: $*" >&2; exit 1; }

for tool in ffmpeg ffprobe flac metaflac; do
  command -v "$tool" >/dev/null || die "'$tool' is not installed"
done

root=$(cd "$(dirname "$0")/.." && pwd)
audio=$root/test-audio
fix=$root/test/host/fixtures
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$audio" "$fix"

ff() { ffmpeg -hide_banner -loglevel error -nostdin -y "$@"; }

# Filter graphs. $1 = sample rate.
stereo() { echo "sine=f=440:r=$1[l];sine=f=660:r=$1[r];[l][r]join=inputs=2:channel_layout=stereo"; }
mono()   { echo "sine=f=440:r=$1"; }
# `sine` only makes 16-bit samples. Scaling them in floating point (no dither)
# fills the low 8 bits, so the 24-bit files are not just padded 16-bit ones.
stereo24() { echo "$(stereo "$1"),aformat=sample_fmts=dbl,volume=0.99:precision=double"; }
# Both channels the same, raised from the source's -18.06 dBFS to -12 dBFS.
steady() { echo "sine=f=440:r=$1,volume=6.0618dB,pan=stereo|c0=c0|c1=c0"; }
# 5 s of full-scale white noise, then a tone 50 dB below the others (about
# -68 dBFS), both the same in the two channels. Noise cannot be compressed, so
# its frames are as big as 16-bit stereo-as-mono frames get (about 8 KB); the
# faint tone's frames are about a ninth of that. A decoder that sizes its
# parsing by the largest frame then finds several whole frames in what it has
# gathered, and returns some of them without taking any new input: the case
# the player once mistook for a stall and skipped audio on. The noise has a
# fixed seed, so it is the same on every run.
loud_quiet() {
  local same="pan=stereo|c0=c0|c1=c0"
  echo "anoisesrc=r=$1:color=white:amplitude=1:seed=20260930,aformat=sample_fmts=s16,$same,atrim=end_sample=$(( $1 * 5 ))[loud];sine=f=440:r=$1,volume=-50dB,$same,atrim=end_sample=$(( $1 * 15 ))[quiet];[loud][quiet]concat=n=2:v=0:a=1"
}

# make_flac <out> <graph> <frames> <bits> [extra flac options]
make_flac() {
  local out=$1 graph=$2 frames=$3 bits=$4; shift 4
  ff -f lavfi -i "$graph,atrim=end_sample=$frames" \
     -c:a "pcm_s${bits}le" -fflags +bitexact "$tmp/pcm.wav"
  flac --totally-silent --force --best "$@" -o "$out" "$tmp/pcm.wav"
}

# make_mp3 <out> <graph> <frames> <lame options...>
make_mp3() {
  local out=$1 graph=$2 frames=$3; shift 3
  ff -f lavfi -i "$graph,atrim=end_sample=$frames" -c:a libmp3lame "$@" "$out"
}

# check_flac <file> <rate> <channels> <bits> <frames>
check_flac() {
  local got want="$2 $3 $4 $5"
  got=$(metaflac --show-sample-rate --show-channels --show-bps \
                 --show-total-samples "$1" | tr '\n' ' ')
  [ "${got% }" = "$want" ] ||
    die "$(basename "$1"): rate/channels/bits/frames are '${got% }', expected '$want'"
  flac --totally-silent --test "$1" || die "$(basename "$1"): fails flac --test"
}

# check_frame_spread <file> <least max frame bytes> <least largest/median>
# The property t10 exists for: one stream with both big and small frames.
check_frame_spread() {
  local name sizes n largest median declared
  name=$(basename "$1")
  declared=$(metaflac --show-max-framesize "$1")
  [ "${declared:-0}" -ge "$2" ] ||
    die "$name: STREAMINFO says the largest frame is ${declared:-unknown} bytes, need $2"
  flac --totally-silent --force --analyze -o "$tmp/frames.ana" "$1" ||
    die "$name: flac --analyze failed"
  # One line per frame: "frame=N<tab>offset=N<tab>bits=N<tab>...".
  sizes=$(sed -n 's/^frame=[0-9]*\toffset=[0-9]*\tbits=\([0-9]*\)\t.*/\1/p' \
            "$tmp/frames.ana" | sort -n)
  [ -n "$sizes" ] || die "$name: no frames in the flac --analyze output"
  n=$(wc -l <<<"$sizes")
  largest=$(( $(tail -n 1 <<<"$sizes") / 8 ))
  median=$(( $(sed -n "$(( (n + 1) / 2 ))p" <<<"$sizes") / 8 ))
  [ "$largest" -ge $(( median * $3 )) ] ||
    die "$name: largest frame $largest bytes is under $3 times the median $median"
  echo "$name: $n frames, largest $largest bytes, median $median bytes, STREAMINFO max $declared"
}

# check_mp3 <file> <rate> <channels>
check_mp3() {
  local got
  got=$(ffprobe -v error -select_streams a:0 \
          -show_entries stream=codec_name,sample_rate,channels -of csv=p=0 "$1")
  [ "$got" = "mp3,$2,$3" ] || die "$(basename "$1"): ffprobe says '$got', expected 'mp3,$2,$3'"
}

size() { stat -c %s "$1"; }

# Exactly $1 bytes of repeated text.
# (Not `yes | head`: under pipefail the SIGPIPE to `yes` would fail the script.)
text() { head -c "$1" < <(yes "This is not audio. It is plain text for the player's error handling."); }

# ---- test-audio/ ------------------------------------------------------------

make_flac "$audio/t01_44k_16_st.flac"   "$(stereo 44100)"   441000  16
make_flac "$audio/t02_96k_24_st.flac"   "$(stereo24 96000)" 960000  24
make_flac "$audio/t03_48k_16_mono.flac" "$(mono 48000)"     480000  16
make_mp3  "$audio/t04_44k_cbr192_st.mp3" "$(stereo 44100)"  441000  -b:a 192k
make_mp3  "$audio/t05_48k_vbr_mono.mp3"  "$(mono 48000)"    480000  -q:a 4
make_flac "$audio/t06_tone_30s.flac"    "$(steady 44100)"   1323000 16
: > "$audio/t07_empty.flac"
text 4096 > "$audio/t08_not_audio.flac"
head -c $(( $(size "$audio/t01_44k_16_st.flac") * 60 / 100 )) \
  "$audio/t01_44k_16_st.flac" > "$audio/t09_truncated.flac"
make_flac "$audio/t10_loud_quiet_44k_16_st.flac" "$(loud_quiet 44100)" 882000 16

check_flac "$audio/t01_44k_16_st.flac"   44100 2 16 441000
check_flac "$audio/t02_96k_24_st.flac"   96000 2 24 960000
check_flac "$audio/t03_48k_16_mono.flac" 48000 1 16 480000
check_flac "$audio/t06_tone_30s.flac"    44100 2 16 1323000
check_mp3  "$audio/t04_44k_cbr192_st.mp3" 44100 2
check_mp3  "$audio/t05_48k_vbr_mono.mp3"  48000 1
[ "$(size "$audio/t07_empty.flac")" = 0 ]        || die "t07 is not empty"
[ "$(size "$audio/t08_not_audio.flac")" = 4096 ] || die "t08 is not 4096 bytes"
check_flac "$audio/t10_loud_quiet_44k_16_st.flac" 44100 2 16 882000
check_frame_spread "$audio/t10_loud_quiet_44k_16_st.flac" 8000 8

# ---- test/host/fixtures/ ----------------------------------------------------
# 0.1 s each. No padding block and no seek table: they only cost bytes here.

small="--no-padding --no-seektable"
make_flac "$fix/f_44k_16_st.flac"   "$(stereo 44100)"   4410 16 $small
make_flac "$fix/f_96k_24_st.flac"   "$(stereo24 96000)" 9600 24 $small
make_flac "$fix/f_48k_16_mono.flac" "$(mono 48000)"     4800 16 $small

# A PICTURE metadata block of more than 20 KB. An uncompressed PNG is big
# enough at 96x96 and does not depend on the zlib version.
ff -f lavfi -i "testsrc=size=96x96" -frames:v 1 -pix_fmt rgb24 \
   -compression_level 0 -fflags +bitexact "$tmp/cover.png"
cp "$fix/f_44k_16_st.flac" "$fix/f_with_picture.flac"
metaflac --dont-use-padding --import-picture-from="$tmp/cover.png" "$fix/f_with_picture.flac"

# A 1024-byte ID3v2.4 tag in front of the FLAC data: a 10-byte header whose
# sync-safe size says 1014 (0x00 0x00 0x07 0x76), one TIT2 frame, zero padding.
{
  printf 'ID3\x04\x00\x00\x00\x00\x07\x76'
  printf 'TIT2\x00\x00\x00\x0d\x00\x00\x03Fixture tone'
  head -c $(( 1014 - 10 - 13 )) /dev/zero
  cat "$fix/f_44k_16_st.flac"
} > "$fix/f_with_id3.flac"

text 64 > "$fix/f_not_flac.bin"
head -c 20 "$fix/f_44k_16_st.flac" > "$fix/f_truncated.flac"

check_flac "$fix/f_44k_16_st.flac"    44100 2 16 4410
check_flac "$fix/f_96k_24_st.flac"    96000 2 24 9600
check_flac "$fix/f_48k_16_mono.flac"  48000 1 16 4800
check_flac "$fix/f_with_picture.flac" 44100 2 16 4410
pic=$(metaflac --list --block-type=PICTURE "$fix/f_with_picture.flac" |
        sed -n 's/^ *length: //p')
[ "${pic:-0}" -ge 20480 ] || die "f_with_picture.flac: picture block is ${pic:-missing} bytes, need 20480"
[ "$(head -c 10 "$fix/f_with_id3.flac" | od -An -tx1 | tr -d ' ')" = 49443304000000000776 ] ||
  die "f_with_id3.flac: wrong ID3v2 header"
tail -c +1025 "$fix/f_with_id3.flac" | cmp -s - "$fix/f_44k_16_st.flac" ||
  die "f_with_id3.flac: the FLAC data does not start at byte 1024"
[ "$(size "$fix/f_not_flac.bin")" = 64 ]   || die "f_not_flac.bin is not 64 bytes"
[ "$(size "$fix/f_truncated.flac")" = 20 ] || die "f_truncated.flac is not 20 bytes"
for f in "$fix"/*; do
  [ "$(size "$f")" -lt 65536 ] || die "$(basename "$f") is $(size "$f") bytes; fixtures must stay under 64 KB"
done

echo "OK: $(ls "$audio" | wc -l) files in test-audio/, $(ls "$fix" | wc -l) in test/host/fixtures/"
echo "Copy the contents of test-audio/ to a folder named 'test' on the SD card."
