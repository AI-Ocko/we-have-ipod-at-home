# Tag fixtures

One file per case the tag reader has to cope with. Generated and checked by
`tools/make_test_audio.sh` (ID3 tags are built byte by byte by
`tools/id3_tag.py`); do not edit by hand. This file is the list of what each
fixture holds, exactly. Tests are written from it: if the generator changes,
change this file with it.

`(absent)` means the tag or frame is not in the file. `(empty)` means it is in
the file with a value of zero length.

## Audio and durations

Every fixture carries the same signal: 0.1 s (4410 samples) of a 440 Hz / 660 Hz
stereo tone at 44.1 kHz, 16 bits.

**FLAC:** STREAMINFO says 44100 Hz, 2 channels, 16 bits, 4410 samples.
Duration **100 ms** exactly.

**MP3:** MPEG-1 layer III, 44.1 kHz, stereo, 1152 samples per frame. Every MP3
fixture has **5 audio frames = 5760 samples = 130.612 ms** (131 ms rounded,
130 ms truncated). That is what a decoder puts out, and what "frames x 1152 /
sample rate" gives. It is longer than the 100 ms signal because the encoder
adds its delay (576 samples) in front and padding (774 samples) behind:
576 + 4410 + 774 = 5760.

Three kinds of MP3 audio are used:

| Audio | Used by | First frame | Audio frames | Audio bytes |
|---|---|---|---|---|
| CBR 128 kbps with an `Info` frame | every tagged MP3, `mp3_id3v1_only`, `mp3_bad_size` | a 417-byte frame holding `Info` at offset 36; frame count 5, byte count 2506 (which counts the Info frame itself) | 5 (417, 418, 418, 418, 418 bytes) | 2506 with the Info frame |
| VBR with a `Xing` frame | `mp3_no_tags` | a 208-byte frame (64 kbps header) holding `Xing` at offset 36; frame count 5, byte count 2006 | 5 (835, 208, 182, 156, 417 bytes: 256, 64, 56, 48, 128 kbps) | 2006 with the Xing frame |
| CBR 128 kbps, no Info/Xing frame | `mp3_no_xing_cbr` | audio | 5 (417, 418, 418, 418, 418 bytes) | 2089 |

In the Info/Xing frame: flags `00 00 00 0f` (frames, bytes, seek table and
quality all present), the frame count as a 32-bit big-endian number at offset
44 of the frame, the byte count at offset 48. The frame also records the
encoder delay and padding (576 and 774), from which a reader that trims them
gets exactly 100 ms. The Info/Xing frame is not an audio frame and is not
counted in the 5.

Duration to expect from each MP3:

| File | From the frame count (5 x 1152 / 44100) | From size and bit rate |
|---|---|---|
| any with an Info or Xing frame | 130.612 ms | not needed |
| `mp3_no_xing_cbr.mp3` | 130.612 ms | 2089 bytes x 8 / 128000 = 130.563 ms (0.04 % short) |
| `mp3_no_tags.mp3` (VBR) | 130.612 ms | meaningless: the first audio frame is 256 kbps, the Xing frame's own header says 64 kbps |

## FLAC fixtures

All have STREAMINFO first. The VORBIS_COMMENT block's vendor string is
`reference libFLAC 1.5.0 20250211`. No padding or seek table blocks.
Comments are listed in file order, exactly as stored (keys included).

| File | Bytes | Blocks, in order | Vorbis comments |
|---|---|---|---|
| `flac_full.flac` | 2929 | STREAMINFO, VORBIS_COMMENT | `TITLE=Idioteque`, `ARTIST=Radiohead`, `ALBUM=Kid A`, `ALBUMARTIST=Radiohead`, `TRACKNUMBER=8`, `DISCNUMBER=1` |
| `flac_lowercase_keys.flac` | 2929 | STREAMINFO, VORBIS_COMMENT | `title=Idioteque`, `Artist=Radiohead`, `album=Kid A`, `AlbumArtist=Radiohead`, `tracknumber=8`, `DiscNumber=1` |
| `flac_track_of_total.flac` | 2855 | STREAMINFO, VORBIS_COMMENT | `TRACKNUMBER=3/12`, `DISCNUMBER=2/2` (no title, artist or album) |
| `flac_utf8.flac` | 2886 | STREAMINFO, VORBIS_COMMENT | `TITLE=Sigur Rós – Hoppípolla`, `ARTIST=Björk`, `ALBUM=你好` |
| `flac_no_tags.flac` | 2773 | STREAMINFO (marked as the last block) | no VORBIS_COMMENT block at all |
| `flac_empty_values.flac` | 2838 | STREAMINFO, VORBIS_COMMENT | `TITLE=`, `ARTIST=` (both present, both empty; nothing else) |
| `flac_picture_first.flac` | 209841 | STREAMINFO, PICTURE, VORBIS_COMMENT | `TITLE=Picture First`, `ARTIST=The Block Order`, `ALBUM=Cover Before Comments` |
| `flac_long_title.flac` | 3427 | STREAMINFO, VORBIS_COMMENT | `TITLE=` followed by 600 characters (see below); nothing else |

Expected field values:

| File | Title | Artist | Album | Album artist | Track | Disc |
|---|---|---|---|---|---|---|
| `flac_full.flac` | `Idioteque` | `Radiohead` | `Kid A` | `Radiohead` | 8 | 1 |
| `flac_lowercase_keys.flac` | `Idioteque` | `Radiohead` | `Kid A` | `Radiohead` | 8 | 1 |
| `flac_track_of_total.flac` | (absent) | (absent) | (absent) | (absent) | 3 (of 12) | 2 (of 2) |
| `flac_utf8.flac` | `Sigur Rós – Hoppípolla` | `Björk` | `你好` | (absent) | (absent) | (absent) |
| `flac_no_tags.flac` | (absent) | (absent) | (absent) | (absent) | (absent) | (absent) |
| `flac_empty_values.flac` | (empty) | (empty) | (absent) | (absent) | (absent) | (absent) |
| `flac_picture_first.flac` | `Picture First` | `The Block Order` | `Cover Before Comments` | (absent) | (absent) | (absent) |
| `flac_long_title.flac` | the 600 characters | (absent) | (absent) | (absent) | (absent) | (absent) |

Details:

- `flac_utf8.flac`, the values in UTF-8:
  - title, 26 bytes: `53 69 67 75 72 20 52 c3 b3 73 20 e2 80 93 20 48 6f 70 70 c3 ad 70 6f 6c 6c 61`
    (ó is U+00F3 `c3 b3`, the dash is U+2013 `e2 80 93`, í is U+00ED `c3 ad`)
  - artist, 6 bytes: `42 6a c3 b6 72 6b` (ö is U+00F6)
  - album, 6 bytes: `e4 bd a0 e5 a5 bd` (U+4F60 U+597D)
- `flac_picture_first.flac`: the PICTURE block starts at offset 42 and is
  206940 bytes long (plus its 4-byte block header); the picture itself is a
  206899-byte PNG, 264x260, type 3 (front cover), MIME type `image/png`. The
  VORBIS_COMMENT block header is at offset 206986.
- `flac_long_title.flac`: the title is 600 ASCII characters: sixty groups of
  ten, `000-abcdef`, `001-abcdef`, ... `059-abcdef`, with nothing between
  them. It starts `000-abcdef001-abcdef` and ends `058-abcdef059-abcdef`.
  Character *n* (from 0) is inside group *n* / 10.

## MP3 fixtures

In every ID3v2 tag here: the header flags are 0 (no unsynchronisation, no
extended header, no footer) and every frame's two flag bytes are 0. ID3v2.3
frame sizes are plain 32-bit big-endian numbers; ID3v2.4 frame sizes are
sync-safe (7 bits per byte). The MPEG audio starts where the tag ends.

Text encodings, as named in the tables:

| Name | Encoding byte | The bytes after it |
|---|---|---|
| Latin-1 | `00` | ISO-8859-1 |
| UTF-16 LE+BOM | `01` | `ff fe`, then UTF-16 little-endian |
| UTF-16 BE+BOM | `01` | `fe ff`, then UTF-16 big-endian |
| UTF-16BE | `02` | UTF-16 big-endian, no byte order mark |
| UTF-8 | `03` | UTF-8 |

Text has **no terminator** unless the table says "+ terminator" (`00`, or
`00 00` in the UTF-16 encodings; the terminator is not part of the value).

### Summary

| File | Bytes | ID3v2 | Tag bytes (header included) = offset of the audio | ID3v1 | Info/Xing | Duration |
|---|---|---|---|---|---|---|
| `mp3_id3v23_latin1.mp3` | 2904 | 2.3 | 398 | no | Info | 130.612 ms |
| `mp3_id3v23_utf16.mp3` | 2754 | 2.3 | 248 | no | Info | 130.612 ms |
| `mp3_id3v24_utf8.mp3` | 2845 | 2.4 | 339 | no | Info | 130.612 ms |
| `mp3_id3v24_utf16be.mp3` | 2732 | 2.4 | 226 | no | Info | 130.612 ms |
| `mp3_id3v1_only.mp3` | 2634 | none | 0 | yes | Info | 130.612 ms |
| `mp3_both.mp3` | 2705 | 2.4 | 71 | yes | Info | 130.612 ms |
| `mp3_no_tags.mp3` | 2006 | none | 0 | no | Xing (VBR) | 130.612 ms |
| `mp3_no_xing_cbr.mp3` | 2089 | none | 0 | no | none | 130.612 ms |
| `mp3_apic.mp3` | 209691 | 2.3 | 207185 | no | Info | 130.612 ms |
| `mp3_bad_size.mp3` | 2535 | 2.3, bad size | claims 65546; the audio is at 29 | no | Info | 130.612 ms |

Expected field values (for `mp3_both.mp3`, after combining the two tags):

| File | Title | Artist | Album | Album artist | Track | Disc |
|---|---|---|---|---|---|---|
| `mp3_id3v23_latin1.mp3` | `Café del Mar` | `Motörhead` | `Déjà Vu` | `Señor Coconut` | 5 (of 10) | 1 (of 2) |
| `mp3_id3v23_utf16.mp3` | `Владивосток 2000` | `Мумий Тролль` | `你好世界` | `Naïve 𝄞 Trio` | 2 | 1 |
| `mp3_id3v24_utf8.mp3` | `Leyndarmál – 秘密` | `Ásgeir` | `Dýrð í dauðaþögn` | `Ýmsir flytjendur` | 4 (of 11) | 1 |
| `mp3_id3v24_utf16be.mp3` | `Тёмная ночь` | `Марк Бернес` | `Два бойца – 二人` | `Разные исполнители` | 9 | 2 (of 3) |
| `mp3_id3v1_only.mp3` | `Für Elise` | `Ludwig van Beethoven` | `Bagatelles and Piano Favorites` | (absent) | 7 | (absent) |
| `mp3_both.mp3` | `Title From V2` | `Artist From V2` | `Album Only In V1` | (absent) | 3 | (absent) |
| `mp3_no_tags.mp3` | (absent) | (absent) | (absent) | (absent) | (absent) | (absent) |
| `mp3_no_xing_cbr.mp3` | (absent) | (absent) | (absent) | (absent) | (absent) | (absent) |
| `mp3_apic.mp3` | `After The Picture` | `The Cover Arts` | `Front Cover` | `Picture People` | 12 | 1 |
| `mp3_bad_size.mp3` | see below | (absent) | (absent) | (absent) | (absent) | (absent) |

### `mp3_id3v23_latin1.mp3`

Header `49 44 33 03 00 00 00 00 03 04` (size 388). 256 zero bytes of padding
from offset 142 to the end of the tag at 398.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | TSSE | 8 | Latin-1 | `fixture` (the encoder's name: a frame to step over) |
| 28 | TIT2 | 13 | Latin-1 | `Café del Mar` = `43 61 66 e9 20 64 65 6c 20 4d 61 72` |
| 51 | TPE1 | 10 | Latin-1 | `Motörhead` = `4d 6f 74 f6 72 68 65 61 64` |
| 71 | TALB | 8 | Latin-1 | `Déjà Vu` = `44 e9 6a e0 20 56 75` |
| 89 | TPE2 | 14 | Latin-1 | `Señor Coconut` = `53 65 f1 6f 72 20 43 6f 63 6f 6e 75 74` |
| 113 | TRCK | 5 | Latin-1 | `5/10` |
| 128 | TPOS | 4 | Latin-1 | `1/2` |

As UTF-8 the values are `Caf\xc3\xa9 del Mar`, `Mot\xc3\xb6rhead`,
`D\xc3\xa9j\xc3\xa0 Vu`, `Se\xc3\xb1or Coconut`.

### `mp3_id3v23_utf16.mp3`

Header `49 44 33 03 00 00 00 00 01 6e` (size 238). 64 zero bytes of padding
from offset 184 to the end of the tag at 248.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | TIT2 | 35 | UTF-16 LE+BOM | `Владивосток 2000` (16 characters) |
| 55 | TPE1 | 29 | UTF-16 LE+BOM + terminator | `Мумий Тролль` (12 characters) |
| 94 | TALB | 11 | UTF-16 BE+BOM | `你好世界` = `fe ff 4f 60 59 7d 4e 16 75 4c` |
| 115 | TPE2 | 29 | UTF-16 LE+BOM | `Naïve 𝄞 Trio`: U+1D11E is the surrogate pair D834 DD1E, bytes `34 d8 1e dd` at offset 140 |
| 154 | TRCK | 5 | UTF-16 LE+BOM | `2` = `ff fe 32 00` |
| 169 | TPOS | 5 | UTF-16 LE+BOM | `1` = `ff fe 31 00` |

The first frame, whole: `54 49 54 32 00 00 00 23 00 00 01 ff fe 12 04 3b 04 30 04
34 04 38 04 32 04 3e 04 41 04 42 04 3e 04 3a 04 20 00 32 00 30 00 30 00 30 00`.

As UTF-8: the title is 27 bytes
(`d0 92 d0 bb d0 b0 d0 b4 d0 b8 d0 b2 d0 be d1 81 d1 82 d0 be d0 ba 20 32 30 30 30`),
the artist 23 bytes, the album 12 bytes (`e4 bd a0 e5 a5 bd e4 b8 96 e7 95 8c`),
the album artist 16 bytes (`4e 61 c3 af 76 65 20 f0 9d 84 9e 20 54 72 69 6f`).

### `mp3_id3v24_utf8.mp3`

Header `49 44 33 04 00 00 00 00 02 49` (size 329). No padding: the last frame
ends at 339, where the audio starts.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | TIT2 | 23 | UTF-8 | `Leyndarmál – 秘密` = `4c 65 79 6e 64 61 72 6d c3 a1 6c 20 e2 80 93 20 e7 a7 98 e5 af 86` |
| 43 | TXXX | 179 | UTF-8 | description `NOTE` + terminator, then a 173-character sentence (a frame to step over) |
| 232 | TPE1 | 9 | UTF-8 + terminator | `Ásgeir` = `c3 81 73 67 65 69 72`, then `00` |
| 251 | TALB | 23 | UTF-8 | `Dýrð í dauðaþögn` (22 bytes) |
| 284 | TPE2 | 18 | UTF-8 | `Ýmsir flytjendur` (17 bytes) |
| 312 | TRCK | 5 | UTF-8 | `4/11` |
| 327 | TPOS | 2 | UTF-8 | `1` |

The TXXX frame is the point of this file: its size bytes are `00 00 01 33`,
which is 179 as a sync-safe number and 307 as a plain one. A reader that
takes it as 307 looks for the next frame at offset 360, which is inside the
audio, and loses TPE1, TALB, TPE2, TRCK and TPOS.

### `mp3_id3v24_utf16be.mp3`

Header `49 44 33 04 00 00 00 00 01 58` (size 216). 32 zero bytes of padding
from offset 194 to the end of the tag at 226.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | TIT2 | 23 | UTF-16BE | `Тёмная ночь` (11 characters) = `04 22 04 51 04 3c 04 3d 04 30 04 4f 00 20 04 3d 04 3e 04 47 04 4c` |
| 43 | TPE1 | 25 | UTF-16BE + terminator | `Марк Бернес` (11 characters), then `00 00` |
| 78 | TALB | 29 | UTF-16BE | `Два бойца – 二人` (14 characters; the dash is `20 13`, then `00 20 4e 8c 4e ba`) |
| 117 | TPE2 | 37 | UTF-16BE | `Разные исполнители` (18 characters) |
| 164 | TRCK | 3 | UTF-16BE | `9` = `00 39` |
| 177 | TPOS | 7 | UTF-16BE | `2/3` = `00 32 00 2f 00 33` |

### `mp3_id3v1_only.mp3`

No ID3v2 tag: the file starts with the Info frame (`ff fb 90 00`). The last
128 bytes are an ID3v1.1 tag, at offset 2506:

| Offset in the tag | Field | Bytes | Value |
|---|---|---|---|
| 0 | marker | 3 | `TAG` |
| 3 | title | 30 | `Für Elise` = `46 fc 72 20 45 6c 69 73 65`, then 21 zero bytes |
| 33 | artist | 30 | `Ludwig van Beethoven`, then 10 zero bytes |
| 63 | album | 30 | `Bagatelles and Piano Favorites`: exactly 30 characters, so **no terminator**; the year follows at once |
| 93 | year | 4 | `1810` |
| 97 | comment | 28 | all zero |
| 125 | | 1 | `00` (marks ID3v1.1) |
| 126 | track | 1 | `07` |
| 127 | genre | 1 | `ff` |

The text is ISO-8859-1: the title as UTF-8 is `F\xc3\xbcr Elise`.

### `mp3_both.mp3`

ID3v2.4 at the start, ID3v1.1 at the end, and they disagree. The ID3v2 values
win; the ID3v1 tag supplies what the ID3v2 tag lacks (the album).

Header `49 44 33 04 00 00 00 00 00 3d` (size 61). No padding.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | TIT2 | 14 | UTF-8 | `Title From V2` |
| 34 | TPE1 | 15 | Latin-1 | `Artist From V2` |
| 59 | TRCK | 2 | Latin-1 | `3` |

There is no TALB, TPE2 or TPOS frame. The ID3v1.1 tag, at offset 2577:

| Field | Value |
|---|---|
| title | `Title From V1` |
| artist | `Artist From V1` |
| album | `Album Only In V1` |
| year | `1999` |
| track | 9 |
| genre | 255 |

Expected: title `Title From V2`, artist `Artist From V2`, album
`Album Only In V1`, track 3, no album artist, no disc.

### `mp3_no_tags.mp3`

No ID3v2, no ID3v1. Variable bit rate; the first frame holds a `Xing` header
(frames 5, bytes 2006). See "Audio and durations".

### `mp3_no_xing_cbr.mp3`

No ID3v2, no ID3v1, no Info or Xing frame (neither word occurs anywhere in the
file). Five frames of 128 kbps: 417, 418, 418, 418, 418 bytes = 2089 bytes.
The first frame header is `ff fb 90 44`. True duration 130.612 ms; size and
bit rate give 130.563 ms.

### `mp3_apic.mp3`

Header `49 44 33 03 00 00 00 0c 52 47` (size 207175). 128 zero bytes of padding
from offset 207057 to the end of the tag at 207185.

| Offset | Frame | Body bytes | Encoding | Value |
|---|---|---|---|---|
| 10 | APIC | 206912 | Latin-1 | MIME type `image/png` + terminator, picture type `03` (front cover), empty description (`00`), then a 206899-byte PNG |
| 206932 | TIT2 | 18 | Latin-1 | `After The Picture` |
| 206960 | TPE1 | 15 | Latin-1 | `The Cover Arts` |
| 206985 | TALB | 12 | Latin-1 | `Front Cover` |
| 207007 | TPE2 | 15 | Latin-1 | `Picture People` |
| 207032 | TRCK | 3 | Latin-1 | `12` |
| 207045 | TPOS | 2 | Latin-1 | `1` |

The APIC size bytes are `00 03 28 40`: a plain number in ID3v2.3. (Read as
sync-safe they would give 54336, which lands inside the picture.)

### `mp3_bad_size.mp3`

Header `49 44 33 03 00 00 00 04 00 00`: ID3v2.3 with a size of 65536, so the
tag claims to end at offset 65546 in a file of 2535 bytes.

After the header comes one well-formed frame, then the audio:

| Offset | What | Bytes |
|---|---|---|
| 10 | TIT2, body 9 bytes, Latin-1, `Bad Size` | `54 49 54 32 00 00 00 09 00 00 00 42 61 64 20 53 69 7a 65` |
| 29 | the Info frame and 5 audio frames (2506 bytes) | `ff fb 90 00 ...` |

What the reader makes of it is for the reader's tests to decide (reject the
tag, or keep the `Bad Size` title and stop at the first thing that is not a
frame). Either way it must not read past the end of the file or try to skip
65546 bytes, and it must not fail in a way that stops a scan.
