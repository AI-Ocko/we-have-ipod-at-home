#!/usr/bin/env bash
# Generate the deterministic test audio set and the host test fixtures.
#
# Usage: tools/make_test_audio.sh
#
#   test-audio/               t01..t10, for the SD card's /test/ folder (not committed)
#   test-audio/lib1000/       1,000 tagged tracks for timing a library scan (not committed)
#   test/host/fixtures/       tiny files for the host unit tests (committed)
#   test/host/fixtures/tags/  one file per kind of tag the tag reader must cope with
#   test/host/fixtures/lib/   a small library tree for the scanner; goes on the card too
#
# Every tone comes from ffmpeg's `sine` source (integer maths, -18 dBFS, no
# dither), is trimmed to an exact sample count and encoded by the reference
# `flac` encoder, so a re-run with the same tool versions gives the same bytes.
# The one noise burst (t10) comes from ffmpeg's `anoisesrc` with fixed seeds.
# The peak level of every FLAC is checked too: none is above -12 dBFS.
# The script checks its own output and exits non-zero if anything is off.
set -euo pipefail

if [ $# -ne 0 ]; then
  sed -n '2,10p' "$0" | cut -c3-
  case $1 in -h|--help) exit 0 ;; *) exit 2 ;; esac
fi

die() { echo "make_test_audio: ERROR: $*" >&2; exit 1; }

for tool in ffmpeg ffprobe flac metaflac python3 iconv od; do
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
# 5 s of white noise, then 15 s of a very faint tone.
#
# These files are played through headphones, so nothing here is near full
# scale: the noise peaks at 0.095 of full scale (-20.4 dBFS). The tone is
# 60 dB below the other files' tones (about -78 dBFS), the same in both
# channels.
#
# Noise cannot be compressed, and the two channels are independent (two
# seeds), so its frames are about 13 KB; the faint tone's are about a
# twentieth of that. A decoder that sizes its parsing by the largest frame
# then finds several whole frames in what it has gathered, and returns some of
# them without taking any new input: the case the player once mistook for a
# stall and skipped audio on. The seeds are fixed, so the noise is the same on
# every run.
loud_quiet() {
  local noise="anoisesrc=r=$1:color=white:amplitude=0.095"
  echo "$noise:seed=20260930[nl];$noise:seed=20261001[nr];[nl][nr]join=inputs=2:channel_layout=stereo,aformat=sample_fmts=s16,atrim=end_sample=$(( $1 * 5 ))[loud];sine=f=440:r=$1,volume=-60dB,pan=stereo|c0=c0|c1=c0,atrim=end_sample=$(( $1 * 15 ))[quiet];[loud][quiet]concat=n=2:v=0:a=1"
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

# check_peak <file> <highest allowed peak, dBFS>
# The files are listened to on headphones. This catches a change to a filter
# graph that would make one of them loud by accident.
check_peak() {
  local name peak
  name=$(basename "$1")
  peak=$(ffmpeg -hide_banner -nostdin -i "$1" \
           -af astats=measure_perchannel=none:measure_overall=Peak_level \
           -f null - 2>&1 | sed -n 's/.*Peak level dB: *//p')
  [ -n "$peak" ] || die "$name: could not measure the peak level"
  awk -v p="$peak" -v max="$2" 'BEGIN { exit !(p + 0 <= max + 0) }' ||
    die "$name: peak is $peak dBFS, must not be above $2 dBFS"
  echo "$name: peak $peak dBFS (limit $2)"
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

# ---- helpers for the tag fixtures and the test libraries --------------------

# hex <file> <offset> <count>: those bytes as lower-case hex digits.
hex() { od -An -v -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

# check_bytes <file> <offset> <hex digits> <what they are>
check_bytes() {
  local got
  got=$(hex "$1" "$2" $(( ${#3} / 2 )))
  [ "$got" = "$3" ] ||
    die "$(basename "$1"): $4: the bytes at offset $2 are '$got', expected '$3'"
}

# Total size of the files under a directory, in bytes.
dir_bytes() { find "$1" -type f -printf '%s\n' | awk '{ n += $1 } END { print n + 0 }'; }

# check_listing <dir> <file>...: the directory tree holds exactly these files.
check_listing() {
  local dir=$1 got want
  shift
  got=$(cd "$dir" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
  want=$(printf '%s\n' "$@" | LC_ALL=C sort)
  [ "$got" = "$want" ] ||
    die "$dir does not hold exactly the expected files (< expected, > found):
$(diff <(echo "$want") <(echo "$got") || true)"
}

# flac_blocks <file>: the metadata block types in file order, such as "0 4"
# (0 STREAMINFO, 4 VORBIS_COMMENT, 6 PICTURE).
flac_blocks() {
  metaflac --list "$1" |
    awk '/^METADATA block #/ { getline; printf "%s%s", sep, $2; sep = " " }'
}

# check_blocks <file> <block types>
check_blocks() {
  local got
  got=$(flac_blocks "$1")
  [ "$got" = "$2" ] || die "$(basename "$1"): metadata blocks are '$got', expected '$2'"
}

# check_tags <file> [KEY=value...]
# The Vorbis comments must be exactly these, in this order, byte for byte
# (--no-utf8-convert: the values are UTF-8 whatever the locale is).
check_tags() {
  local file=$1 got want
  shift
  want=$(printf '%s\n' "$@")
  got=$(metaflac --no-utf8-convert --export-tags-to=- "$file")
  [ "$got" = "$want" ] || die "$(basename "$file"): the Vorbis comments are:
$got
expected:
$want"
  flac --totally-silent --test "$file" || die "$(basename "$file"): fails flac --test"
}

# tagged_flac <out> <source flac> [KEY=value...]
# A copy of the source with these Vorbis comments, written as given (metaflac
# keeps the case of the keys) and then read back and compared.
tagged_flac() {
  local out=$1 src=$2
  shift 2
  cp "$src" "$out"
  [ $# -eq 0 ] ||
    metaflac --dont-use-padding --no-utf8-convert "${@/#/--set-tag=}" "$out"
  check_tags "$out" "$@"
}

# ID3 tags are built byte by byte (see tools/id3_tag.py): ffmpeg would choose
# the text encodings and the frame order itself.
id3v2() { python3 "$root/tools/id3_tag.py" v2 "$@"; }
id3v1() { python3 "$root/tools/id3_tag.py" v1 "$@"; }

# id3v2_bytes <file>: the length of the ID3v2 tag as its header states it,
# header included (the four size bytes carry 7 bits each).
id3v2_bytes() {
  local b
  b=$(hex "$1" 6 4)
  echo $(( 10 + (0x${b:0:2} << 21) + (0x${b:2:2} << 14) + (0x${b:4:2} << 7) + 0x${b:6:2} ))
}

# check_id3v2 <file> <version: 3 or 4> <tag bytes, header included>
# "ID3", the version, no flags, the stated size, and MPEG audio (an MPEG-1
# layer III frame header, ff fb) straight after the tag.
check_id3v2() {
  local got
  check_bytes "$1" 0 "4944330${2}0000" "ID3v2.$2 header"
  got=$(id3v2_bytes "$1")
  [ "$got" = "$3" ] || die "$(basename "$1"): the ID3v2 tag is $got bytes, expected $3"
  check_bytes "$1" "$3" fffb "MPEG audio after the ID3v2 tag"
}

# check_xing <file> <offset of the first MPEG frame> <Info|Xing|none>
# The Xing/Info header sits 36 bytes into the first frame of an MPEG-1 stereo
# file. "none": it is not there, and neither word is anywhere in the file.
check_xing() {
  local got
  got=$(hex "$1" $(( $2 + 36 )) 4)
  case $3 in
    Info) [ "$got" = 496e666f ] || die "$(basename "$1"): no Info header in the first frame" ;;
    Xing) [ "$got" = 58696e67 ] || die "$(basename "$1"): no Xing header in the first frame" ;;
    none) ! LC_ALL=C grep -q -a -e Xing -e Info "$1" ||
            die "$(basename "$1"): has a Xing/Info header" ;;
    *)    die "check_xing: '$3'?" ;;
  esac
}

# latin1_field <text> <width>: the text in ISO-8859-1, filled with zero bytes
# to the width, as hex digits.
latin1_field() {
  local h zeros
  h=$(printf '%s' "$1" | iconv -f UTF-8 -t ISO-8859-1 | od -An -v -tx1 | tr -d ' \n')
  zeros=$(printf '%0*d' $(( $2 * 2 )) 0)
  echo "$h${zeros:${#h}}"
}

# check_id3v1 <file> <title> <artist> <album> <year> <track>
# The last 128 bytes, field by field. This does not go through id3_tag.py.
check_id3v1() {
  local at=$(( $(size "$1") - 128 ))
  check_bytes "$1" "$at"           544147                    "ID3v1 marker"
  check_bytes "$1" $(( at + 3 ))   "$(latin1_field "$2" 30)" "ID3v1 title"
  check_bytes "$1" $(( at + 33 ))  "$(latin1_field "$3" 30)" "ID3v1 artist"
  check_bytes "$1" $(( at + 63 ))  "$(latin1_field "$4" 30)" "ID3v1 album"
  check_bytes "$1" $(( at + 93 ))  "$(latin1_field "$5" 4)"  "ID3v1 year"
  check_bytes "$1" $(( at + 97 ))  "$(latin1_field "" 29)"   "ID3v1 comment"
  check_bytes "$1" $(( at + 126 )) "$(printf '%02xff' "$6")" "ID3v1 track and genre"
}

# check_no_id3v1 <file>
check_no_id3v1() {
  [ "$(hex "$1" $(( $(size "$1") - 128 )) 3)" != 544147 ] ||
    die "$(basename "$1"): has an ID3v1 tag"
}

# check_mp3_tags <file> <title> <artist> <album> <album artist> <track> <disc>
# What ffprobe reads from the ID3v2 tag: a second opinion from a reader that
# shares no code with id3_tag.py. "" means the field must be absent.
check_mp3_tags() {
  local file=$1 all key got
  shift
  all=$(ffprobe -v error -show_entries format_tags -of default=nw=1 "$file")
  for key in title artist album album_artist track disc; do
    got=$(sed -n "s/^TAG:$key=//p" <<<"$all")
    [ "$got" = "$1" ] ||
      die "$(basename "$file"): ffprobe reads $key as '$got', expected '$1'"
    shift
  done
}

# check_mp3_frames <file> <frames>
# The number of audio frames (ffprobe does not count a Xing/Info frame).
check_mp3_frames() {
  local got
  got=$(ffprobe -v error -select_streams a:0 -count_packets \
          -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$1")
  [ "$got" = "$2" ] || die "$(basename "$1"): $got audio frames, expected $2"
}

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
# The tones are -18.06 dBFS as made by `sine` (-18.15 after the 24-bit
# scaling), t06 is -12.00.
check_peak "$audio/t01_44k_16_st.flac"            -18
check_peak "$audio/t02_96k_24_st.flac"            -18
check_peak "$audio/t03_48k_16_mono.flac"          -18
check_peak "$audio/t06_tone_30s.flac"             -12
check_peak "$audio/t10_loud_quiet_44k_16_st.flac" -20

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
for f in f_44k_16_st f_96k_24_st f_48k_16_mono f_with_picture f_with_id3; do
  check_peak "$fix/$f.flac" -18
done
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

# ---- test/host/fixtures/tags/ -----------------------------------------------
# One file per case the tag reader has to cope with. Every value, encoding and
# duration is written down in tags/README.md, which the reader's tests are
# written from: change the two together.

tags=$fix/tags
mkdir -p "$tags"

# The audio of every FLAC here: 0.1 s of the stereo tone, with a STREAMINFO
# block and the encoder's own VORBIS_COMMENT block (a vendor string, no tags).
base=$fix/f_44k_16_st.flac
check_blocks "$base" "0 4"

# A picture of more than 200 KB: an uncompressed PNG again, 264x260.
ff -f lavfi -i "testsrc=size=264x260" -frames:v 1 -pix_fmt rgb24 \
   -compression_level 0 -fflags +bitexact "$tmp/big.png"
big=$(size "$tmp/big.png")
[ "$big" -ge 204800 ] || die "big.png is $big bytes, need 204800"

# 600 characters: "000-abcdef001-abcdef" and so on to "059-abcdef", so that
# the number shows how far into the title any ten characters are.
long_title=$(for i in $(seq 0 59); do printf '%03d-abcdef' "$i"; done)
[ ${#long_title} = 600 ] || die "the long title is ${#long_title} characters"

tagged_flac "$tags/flac_full.flac" "$base" \
  TITLE=Idioteque ARTIST=Radiohead "ALBUM=Kid A" ALBUMARTIST=Radiohead \
  TRACKNUMBER=8 DISCNUMBER=1
tagged_flac "$tags/flac_lowercase_keys.flac" "$base" \
  title=Idioteque Artist=Radiohead "album=Kid A" AlbumArtist=Radiohead \
  tracknumber=8 DiscNumber=1
tagged_flac "$tags/flac_track_of_total.flac" "$base" TRACKNUMBER=3/12 DISCNUMBER=2/2
tagged_flac "$tags/flac_utf8.flac" "$base" \
  "TITLE=Sigur Rós – Hoppípolla" ARTIST=Björk ALBUM=你好
tagged_flac "$tags/flac_empty_values.flac" "$base" TITLE= ARTIST=
tagged_flac "$tags/flac_long_title.flac" "$base" "TITLE=$long_title"
for f in full lowercase_keys track_of_total utf8 empty_values long_title; do
  check_blocks "$tags/flac_$f.flac" "0 4"
done

# No VORBIS_COMMENT block at all: STREAMINFO is the only (and last) block.
cp "$base" "$tags/flac_no_tags.flac"
metaflac --dont-use-padding --remove --block-type=VORBIS_COMMENT "$tags/flac_no_tags.flac"
check_blocks "$tags/flac_no_tags.flac" "0"
check_tags   "$tags/flac_no_tags.flac"

# The picture first, the comments after it. metaflac adds a new block at the
# end, so: take the comment block out, add the picture, then add the tags.
cp "$tags/flac_no_tags.flac" "$tmp/picture_first.flac"
metaflac --dont-use-padding --import-picture-from="$tmp/big.png" "$tmp/picture_first.flac"
tagged_flac "$tags/flac_picture_first.flac" "$tmp/picture_first.flac" \
  "TITLE=Picture First" "ARTIST=The Block Order" "ALBUM=Cover Before Comments"
check_blocks "$tags/flac_picture_first.flac" "0 6 4"
pic=$(metaflac --list --block-type=PICTURE "$tags/flac_picture_first.flac" |
        sed -n 's/^ *data length: //p')
[ "$pic" = "$big" ] || die "flac_picture_first.flac: picture data is ${pic:-missing} bytes, expected $big"

for f in "$tags"/*.flac; do
  check_flac "$f" 44100 2 16 4410
done

# The MP3 audio: the same 0.1 s stereo tone, three ways. No tags from ffmpeg,
# and +bitexact keeps its version number out of the Info/Xing frame.
#   info.mp3  128 kbps constant, with an Info frame (what most CBR files have)
#   bare.mp3  128 kbps constant, nothing but audio frames
#   xing.mp3  variable bit rate, with a Xing frame
# Each has 5 audio frames of 1152 samples (130.6 ms): the encoder adds its
# delay and padding to the 4410 samples. The Info/Xing frame is not audio.
plain="-id3v2_version 0 -write_id3v1 0 -fflags +bitexact"
make_mp3 "$tmp/info.mp3" "$(stereo 44100)" 4410 -b:a 128k $plain -write_xing 1
make_mp3 "$tmp/bare.mp3" "$(stereo 44100)" 4410 -b:a 128k $plain -write_xing 0
make_mp3 "$tmp/xing.mp3" "$(stereo 44100)" 4410 -q:a 4    $plain -write_xing 1
mp3_frames=5

# ID3v2.3, every frame in ISO-8859-1. TSSE (the encoder's name) is a frame
# the reader has no use for and must step over. 256 bytes of padding.
{ id3v2 3 --padding 256 TSSE=latin1:fixture \
    "TIT2=latin1:Café del Mar" TPE1=latin1:Motörhead "TALB=latin1:Déjà Vu" \
    "TPE2=latin1:Señor Coconut" TRCK=latin1:5/10 TPOS=latin1:1/2
  cat "$tmp/info.mp3"
} > "$tags/mp3_id3v23_latin1.mp3"

# ID3v2.3, every frame in UTF-16 with a byte order mark. TPE1 ends in a
# terminator (00 00); TALB is big-endian (FE FF) where the rest are
# little-endian (FF FE); TPE2 has a character outside the 16-bit range
# (U+1D11E, a surrogate pair). 64 bytes of padding.
{ id3v2 3 --padding 64 \
    "TIT2=utf16:Владивосток 2000" "TPE1=utf16+nul:Мумий Тролль" \
    TALB=utf16be-bom:你好世界 "TPE2=utf16:Naïve 𝄞 Trio" TRCK=utf16:2 TPOS=utf16:1
  cat "$tmp/info.mp3"
} > "$tags/mp3_id3v23_utf16.mp3"

# ID3v2.4, every frame in UTF-8. The body of the TXXX frame is 179 bytes, so
# its sync-safe size (00 00 01 33) is not the same four bytes as the plain
# number (00 00 00 b3), and it comes before most of the frames the reader wants.
# TPE1 ends in a terminator. No padding: the audio follows the last frame.
note="This note is longer than 127 bytes, so the sync-safe size of its frame is not the same as the plain 32-bit number. A reader that mixes the two up loses every frame after it."
{ id3v2 4 "TIT2=utf8:Leyndarmál – 秘密" "TXXX=utf8:NOTE:$note" \
    TPE1=utf8+nul:Ásgeir "TALB=utf8:Dýrð í dauðaþögn" \
    "TPE2=utf8:Ýmsir flytjendur" TRCK=utf8:4/11 TPOS=utf8:1
  cat "$tmp/info.mp3"
} > "$tags/mp3_id3v24_utf8.mp3"

# ID3v2.4, every frame in UTF-16 big-endian with no byte order mark.
# TPE1 ends in a terminator (00 00). 32 bytes of padding.
{ id3v2 4 --padding 32 \
    "TIT2=utf16be:Тёмная ночь" "TPE1=utf16be+nul:Марк Бернес" \
    "TALB=utf16be:Два бойца – 二人" "TPE2=utf16be:Разные исполнители" \
    TRCK=utf16be:9 TPOS=utf16be:2/3
  cat "$tmp/info.mp3"
} > "$tags/mp3_id3v24_utf16be.mp3"

# ID3v1.1 only. The album fills all 30 bytes, so it has no terminator.
v1_only=("Für Elise" "Ludwig van Beethoven" "Bagatelles and Piano Favorites" 1810 7)
{ cat "$tmp/info.mp3"
  id3v1 "${v1_only[@]}"
} > "$tags/mp3_id3v1_only.mp3"

# ID3v2.4 and ID3v1.1 that disagree: the title, artist and track of the
# ID3v2 tag must win; the album is only in the ID3v1 tag.
v1_both=("Title From V1" "Artist From V1" "Album Only In V1" 1999 9)
{ id3v2 4 "TIT2=utf8:Title From V2" "TPE1=latin1:Artist From V2" TRCK=latin1:3
  cat "$tmp/info.mp3"
  id3v1 "${v1_both[@]}"
} > "$tags/mp3_both.mp3"

cp "$tmp/xing.mp3" "$tags/mp3_no_tags.mp3"
cp "$tmp/bare.mp3" "$tags/mp3_no_xing_cbr.mp3"

# ID3v2.3 with the picture in front of the text frames. 128 bytes of padding.
{ id3v2 3 --padding 128 "APIC=$tmp/big.png" \
    "TIT2=latin1:After The Picture" "TPE1=latin1:The Cover Arts" \
    "TALB=latin1:Front Cover" "TPE2=latin1:Picture People" \
    TRCK=latin1:12 TPOS=latin1:1
  cat "$tmp/info.mp3"
} > "$tags/mp3_apic.mp3"

# An ID3v2.3 header that says the tag is 65536 bytes long, in a file of under
# 3 KB. One good frame follows the header, then the audio.
{ id3v2 3 --claim 65536 "TIT2=latin1:Bad Size"
  cat "$tmp/info.mp3"
} > "$tags/mp3_bad_size.mp3"

# Header, tag length and what follows the tag; then one frame of each text
# encoding, byte for byte; then every value as ffprobe reads it.
t=$tags/mp3_id3v23_latin1.mp3
check_id3v2 "$t" 3 398
check_xing  "$t" 398 Info
check_bytes "$t" 10 "545353450000000800000066697874757265" "TSSE frame"
check_bytes "$t" 28 "544954320000000d000000436166e92064656c204d6172" "TIT2 in ISO-8859-1"
check_bytes "$t" 142 "0000000000000000" "padding after the last frame"
check_mp3_tags "$t" "Café del Mar" Motörhead "Déjà Vu" "Señor Coconut" 5/10 1/2
check_no_id3v1 "$t"

t=$tags/mp3_id3v23_utf16.mp3
check_id3v2 "$t" 3 248
check_xing  "$t" 248 Info
check_bytes "$t" 10 "5449543200000023000001fffe12043b04" "TIT2 in UTF-16, little-endian mark"
check_bytes "$t" 55 "545045310000001d000001fffe1c04" "TPE1 in UTF-16"
check_bytes "$t" 90 "4c040000" "TPE1 terminator"
check_bytes "$t" 94 "54414c420000000b000001feff4f60597d4e16754c" "TALB in UTF-16, big-endian mark"
check_bytes "$t" 140 "34d81edd" "U+1D11E as a surrogate pair in TPE2"
check_mp3_tags "$t" "Владивосток 2000" "Мумий Тролль" 你好世界 "Naïve 𝄞 Trio" 2 1
check_no_id3v1 "$t"

t=$tags/mp3_id3v24_utf8.mp3
check_id3v2 "$t" 4 339
check_xing  "$t" 339 Info
check_bytes "$t" 10 "54495432000000170000034c65796e6461726dc3a16c20e2809320e7a798e5af86" "TIT2 in UTF-8"
check_bytes "$t" 43 "54585858000001330000034e4f544500" "TXXX with a sync-safe size of 179"
check_bytes "$t" 232 "5450453100000009000003c38173676569720054414c42" "TPE1 with its terminator, then TALB"
check_mp3_tags "$t" "Leyndarmál – 秘密" Ásgeir "Dýrð í dauðaþögn" "Ýmsir flytjendur" 4/11 1
check_no_id3v1 "$t"

t=$tags/mp3_id3v24_utf16be.mp3
check_id3v2 "$t" 4 226
check_xing  "$t" 226 Info
check_bytes "$t" 10 "544954320000001700000204220451043c" "TIT2 in UTF-16 big-endian, no mark"
check_bytes "$t" 74 "0441000054414c42" "TPE1 terminator, then TALB"
check_mp3_tags "$t" "Тёмная ночь" "Марк Бернес" "Два бойца – 二人" "Разные исполнители" 9 2/3
check_no_id3v1 "$t"

t=$tags/mp3_id3v1_only.mp3
check_bytes "$t" 0 fffb "MPEG audio at the start (no ID3v2 tag)"
check_xing  "$t" 0 Info
check_id3v1 "$t" "${v1_only[@]}"
check_bytes "$t" $(( $(size "$t") - 128 + 63 )) \
  "4261676174656c6c657320616e64205069616e6f204661766f726974657331383130" \
  "the 30-byte album running straight into the year"

t=$tags/mp3_both.mp3
check_id3v2 "$t" 4 71
check_xing  "$t" 71 Info
check_mp3_tags "$t" "Title From V2" "Artist From V2" "" "" 3 ""
check_id3v1 "$t" "${v1_both[@]}"

t=$tags/mp3_no_tags.mp3
check_bytes "$t" 0 fffb "MPEG audio at the start (no ID3v2 tag)"
check_xing  "$t" 0 Xing
check_no_id3v1 "$t"

# Constant bit rate: 128 kbps at 44.1 kHz is 417 bytes a frame, or 418 with
# the padding bit, and the file is nothing but such frames.
t=$tags/mp3_no_xing_cbr.mp3
check_bytes "$t" 0 fffb90 "an MPEG-1 layer III frame at 128 kbps, 44.1 kHz"
check_xing  "$t" 0 none
check_no_id3v1 "$t"
got=$(ffprobe -v error -show_entries packet=size -of default=nw=1:nk=1 "$t" |
        sort -u | tr '\n' ' ')
[ "$got" = "417 418 " ] || die "mp3_no_xing_cbr.mp3: frame sizes are '$got', expected 417 and 418"
[ "$(size "$t")" = $(( 417 + (mp3_frames - 1) * 418 )) ] ||
  die "mp3_no_xing_cbr.mp3 is $(size "$t") bytes: not one frame of 417 and $(( mp3_frames - 1 )) of 418"

# The APIC frame is first; its size is a plain 32-bit number (ID3v2.3), and
# TIT2 is where that size says the next frame is. 13 = the encoding byte,
# "image/png" and its terminator, the picture type, an empty description.
t=$tags/mp3_apic.mp3
apic=$(( big + 13 ))
check_id3v2 "$t" 3 $(( big + 286 ))
check_xing  "$t" $(( big + 286 )) Info
check_bytes "$t" 10 "$(printf '41504943%08x000000696d6167652f706e67000300' "$apic")89504e47" "APIC frame"
check_bytes "$t" $(( 20 + apic )) "544954320000001200000041667465722054686520506963747572655450453100" \
  "TIT2 in ISO-8859-1 after the picture"
check_mp3_tags "$t" "After The Picture" "The Cover Arts" "Front Cover" "Picture People" 12 1
check_no_id3v1 "$t"

t=$tags/mp3_bad_size.mp3
check_bytes "$t" 0 "49443303000000040000" "ID3v2.3 header claiming 65536 bytes"
check_bytes "$t" 10 "54495432000000090000004261642053697a65fffb" "TIT2, then MPEG audio"
[ "$(id3v2_bytes "$t")" -gt "$(size "$t")" ] || die "mp3_bad_size.mp3: the tag does not reach past the end of the file"

for f in id3v23_latin1 id3v23_utf16 id3v24_utf8 id3v24_utf16be id3v1_only both \
         no_tags no_xing_cbr apic; do
  check_mp3        "$tags/mp3_$f.mp3" 44100 2
  check_mp3_frames "$tags/mp3_$f.mp3" "$mp3_frames"
done

check_listing "$tags" README.md \
  flac_full.flac flac_lowercase_keys.flac flac_track_of_total.flac flac_utf8.flac \
  flac_no_tags.flac flac_empty_values.flac flac_picture_first.flac flac_long_title.flac \
  mp3_id3v23_latin1.mp3 mp3_id3v23_utf16.mp3 mp3_id3v24_utf8.mp3 mp3_id3v24_utf16be.mp3 \
  mp3_id3v1_only.mp3 mp3_both.mp3 mp3_no_tags.mp3 mp3_no_xing_cbr.mp3 mp3_apic.mp3 \
  mp3_bad_size.mp3
# Under 64 KB each, but for the two with a picture: over 200 KB, under 260 KB.
for f in "$tags"/*; do
  case $(basename "$f") in
    flac_picture_first.flac|mp3_apic.mp3)
      [ "$(size "$f")" -gt 204800 ] && [ "$(size "$f")" -lt 260000 ] ||
        die "$(basename "$f") is $(size "$f") bytes; it must be between 204800 and 260000" ;;
    *)
      [ "$(size "$f")" -lt 65536 ] ||
        die "$(basename "$f") is $(size "$f") bytes; fixtures must stay under 64 KB" ;;
  esac
done

# ---- test/host/fixtures/lib/ ------------------------------------------------
# A small library for the scanner: on the host as it is, and copied to the
# card as /testlib/. Every file and its tags are listed in lib/README.md.

lib=$fix/lib
mkdir -p "$lib/Radiohead/Kid A" "$lib/Radiohead/OK Computer" "$lib/Bjork/Homogenic" \
         "$lib/Various/Hits" "$lib/Queen/Hits" "$lib/loose" "$lib/deep/a/b/c/d" \
         "$lib/.hidden" "$lib/System Volume Information"

# lib_flac <path under lib/> [KEY=value...]
lib_flac() {
  local out=$lib/$1
  shift
  tagged_flac "$out" "$base" "$@"
}

lib_flac "Radiohead/Kid A/01 Everything.flac" "TITLE=Everything In Its Right Place" \
  ARTIST=Radiohead "ALBUM=Kid A" DISCNUMBER=1 TRACKNUMBER=1
lib_flac "Radiohead/Kid A/02 Kid A.flac" "TITLE=Kid A" \
  ARTIST=Radiohead "ALBUM=Kid A" DISCNUMBER=1 TRACKNUMBER=2
lib_flac "Radiohead/Kid A/10 Motion.flac" "TITLE=Motion Picture Soundtrack" \
  ARTIST=Radiohead "ALBUM=Kid A" DISCNUMBER=1 TRACKNUMBER=10
lib_flac "Bjork/Homogenic/01 Hunter.flac" \
  TITLE=Hunter ARTIST=Björk ALBUM=Homogenic TRACKNUMBER=1
lib_flac "Bjork/Homogenic/02 Joga.flac" \
  TITLE=Jóga ARTIST=Björk ALBUM=Homogenic TRACKNUMBER=2
lib_flac "Various/Hits/01 a.flac" TITLE=Alpha "ARTIST=The Alphas" \
  "ALBUM=Greatest Hits" "ALBUMARTIST=Various Artists" DISCNUMBER=1 TRACKNUMBER=1
lib_flac "Various/Hits/02 b.flac" TITLE=Beta "ARTIST=The Betas" \
  "ALBUM=Greatest Hits" "ALBUMARTIST=Various Artists" DISCNUMBER=1 TRACKNUMBER=2
lib_flac "Various/Hits/01 c.flac" TITLE=Gamma "ARTIST=The Gammas" \
  "ALBUM=Greatest Hits" "ALBUMARTIST=Various Artists" DISCNUMBER=2 TRACKNUMBER=1
lib_flac "deep/a/b/c/d/Deep Track.flac" \
  "TITLE=Deep Track" "ARTIST=Deep Artist" "ALBUM=Deep Album" TRACKNUMBER=1
lib_flac "loose/untagged song.flac"
# Tagged, in folders the scanner must not enter.
lib_flac ".hidden/secret.flac" \
  TITLE=Secret "ARTIST=Hidden Artist" "ALBUM=Hidden Album" TRACKNUMBER=1
lib_flac "System Volume Information/x.flac" \
  "TITLE=System File" "ARTIST=System Artist" "ALBUM=System Album" TRACKNUMBER=1

# The MP3s, one of each kind of tag: ID3v2.3 in ISO-8859-1, ID3v2.4 in UTF-8
# (with the artist in lower case), and ID3v1.1 alone.
t="$lib/Radiohead/OK Computer/01 Airbag.mp3"
{ id3v2 3 TIT2=latin1:Airbag TPE1=latin1:Radiohead "TALB=latin1:OK Computer" TRCK=latin1:1
  cat "$tmp/info.mp3"
} > "$t"
check_id3v2    "$t" 3 81
check_mp3_tags "$t" Airbag Radiohead "OK Computer" "" 1 ""
check_no_id3v1 "$t"

t="$lib/Radiohead/OK Computer/02 Paranoid.mp3"
{ id3v2 4 "TIT2=utf8:Paranoid Android" TPE1=utf8:radiohead "TALB=utf8:OK Computer" TRCK=utf8:2
  cat "$tmp/info.mp3"
} > "$t"
check_id3v2    "$t" 4 91
check_mp3_tags "$t" "Paranoid Android" radiohead "OK Computer" "" 2 ""
check_no_id3v1 "$t"

t="$lib/Queen/Hits/01 Bohemian.mp3"
v1_queen=("Bohemian Rhapsody" Queen "Greatest Hits" 1981 1)
{ cat "$tmp/info.mp3"
  id3v1 "${v1_queen[@]}"
} > "$t"
check_bytes "$t" 0 fffb "MPEG audio at the start (no ID3v2 tag)"
check_id3v1 "$t" "${v1_queen[@]}"

t="$lib/loose/also untagged.mp3"
cp "$tmp/info.mp3" "$t"
check_bytes    "$t" 0 fffb "MPEG audio at the start (no ID3v2 tag)"
check_no_id3v1 "$t"

for t in "Radiohead/OK Computer/01 Airbag.mp3" "Radiohead/OK Computer/02 Paranoid.mp3" \
         "Queen/Hits/01 Bohemian.mp3" "loose/also untagged.mp3"; do
  check_mp3        "$lib/$t" 44100 2
  check_mp3_frames "$lib/$t" "$mp3_frames"
done

# What the scanner must pass over, or take without tags, and carry on.
printf 'Notes that sit beside the music. Not audio; the scanner must ignore this file.\n' \
  > "$lib/notes.txt"
printf '\xff\xd8\xff\xd9' > "$lib/cover.jpg"   # JPEG start and end markers only
: > "$lib/loose/empty.flac"
text 512 > "$lib/loose/text.mp3"
[ "$(size "$lib/cover.jpg")" = 4 ]        || die "lib/cover.jpg is not 4 bytes"
[ "$(size "$lib/loose/empty.flac")" = 0 ] || die "lib/loose/empty.flac is not empty"
[ "$(size "$lib/loose/text.mp3")" = 512 ] || die "lib/loose/text.mp3 is not 512 bytes"

check_listing "$lib" README.md notes.txt cover.jpg \
  "Radiohead/Kid A/01 Everything.flac" "Radiohead/Kid A/02 Kid A.flac" \
  "Radiohead/Kid A/10 Motion.flac" \
  "Radiohead/OK Computer/01 Airbag.mp3" "Radiohead/OK Computer/02 Paranoid.mp3" \
  "Bjork/Homogenic/01 Hunter.flac" "Bjork/Homogenic/02 Joga.flac" \
  "Various/Hits/01 a.flac" "Various/Hits/02 b.flac" "Various/Hits/01 c.flac" \
  "Queen/Hits/01 Bohemian.mp3" \
  "loose/untagged song.flac" "loose/also untagged.mp3" "loose/empty.flac" "loose/text.mp3" \
  "deep/a/b/c/d/Deep Track.flac" \
  ".hidden/secret.flac" "System Volume Information/x.flac"
lib_size=$(dir_bytes "$lib")
[ "$lib_size" -lt 200000 ] || die "lib/ is $lib_size bytes; it must stay under 200 KB"

# ---- test-audio/lib1000/ ----------------------------------------------------
# 1,000 tagged tracks for timing a scan on the board: 10 artists, each with
# 10 albums of 10 tracks. Not committed. The audio is 0.1 s of the mono tone,
# which is half the size of the stereo one.

big_lib=$audio/lib1000
make_flac "$tmp/mono.flac" "$(mono 44100)" 4410 16 $small
for a in 0 1 2 3 4 5 6 7 8 9; do
  for b in 0 1 2 3 4 5 6 7 8 9; do
    dir="$big_lib/Artist 0$a/Album 0$b"
    mkdir -p "$dir"
    for n in 1 2 3 4 5 6 7 8 9 10; do
      nn=$(printf '%02d' "$n")
      cp "$tmp/mono.flac" "$dir/$nn Track.flac"
      metaflac --dont-use-padding --no-utf8-convert --set-tag="TITLE=Track $nn" \
        --set-tag="ARTIST=Artist 0$a" --set-tag="ALBUM=Album 0$b" \
        --set-tag="TRACKNUMBER=$n" "$dir/$nn Track.flac"
    done
  done
done

n=$(find "$big_lib" -type f | wc -l)
[ "$n" = 1000 ] || die "lib1000/ holds $n files, expected 1000 (delete it and run again)"
n=$(find "$big_lib" -type f -name '[01][0-9] Track.flac' | wc -l)
[ "$n" = 1000 ] || die "lib1000/ holds $n files named 'NN Track.flac', expected 1000"
n=$(find "$big_lib" -mindepth 1 -maxdepth 1 -type d -name 'Artist 0[0-9]' | wc -l)
[ "$n" = 10 ] || die "lib1000/ holds $n artist folders, expected 10"
n=$(find "$big_lib" -mindepth 2 -type d -path '*/Artist 0[0-9]/Album 0[0-9]' | wc -l)
[ "$n" = 100 ] || die "lib1000/ holds $n album folders, expected 100"
[ "$(find "$big_lib" -type d | wc -l)" = 111 ] || die "lib1000/ holds folders it should not"
big_lib_size=$(dir_bytes "$big_lib")
[ "$big_lib_size" -lt 4000000 ] || die "lib1000/ is $big_lib_size bytes; it must stay under 4 MB"
check_tags "$big_lib/Artist 00/Album 00/01 Track.flac" \
  "TITLE=Track 01" "ARTIST=Artist 00" "ALBUM=Album 00" TRACKNUMBER=1
check_tags "$big_lib/Artist 09/Album 09/10 Track.flac" \
  "TITLE=Track 10" "ARTIST=Artist 09" "ALBUM=Album 09" TRACKNUMBER=10
check_flac "$big_lib/Artist 00/Album 00/01 Track.flac" 44100 1 16 4410
check_flac "$big_lib/Artist 09/Album 09/10 Track.flac" 44100 1 16 4410

echo "tags/: $(find "$tags" -type f | wc -l) files, $(dir_bytes "$tags") bytes"
echo "lib/: $(find "$lib" -type f | wc -l) files, $lib_size bytes"
echo "lib1000/: 1000 files in 100 folders, $big_lib_size bytes"

echo "OK: $(find "$audio" -maxdepth 1 -type f | wc -l) files in test-audio/, $(find "$fix" -maxdepth 1 -type f | wc -l) in test/host/fixtures/"
echo "Copy the files in test-audio/ to a folder named 'test' on the SD card,"
echo "test/host/fixtures/lib to the card as 'testlib', and test-audio/lib1000 as 'lib1000'."
