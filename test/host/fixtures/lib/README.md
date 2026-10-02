# Test library

A small music library for the scanner and the index: used as it is by the host
tests, and copied to the SD card as `/testlib/`. Generated and checked by
`tools/make_test_audio.sh`; do not edit by hand. This file lists every file in
the tree and its tags, exactly as written. If the generator changes, change
this file with it.

21 files: 14 audio files to index, 2 audio files in folders that must not be
entered, and 5 others. 45,763 bytes without this file.

Each audio file is 0.1 s of a stereo tone at 44.1 kHz: 4410 samples (100 ms)
in the FLACs; 5 MPEG frames (5760 samples, 130.612 ms) after an `Info` frame
in the MP3s. See `../tags/README.md` for the MP3 details.

`–` means the tag is not in the file.

## The 14 files to index

| Path | Bytes | Title | Artist | Album | Album artist | Disc | Track |
|---|---|---|---|---|---|---|---|
| `Radiohead/Kid A/01 Everything.flac` | 2924 | `Everything In Its Right Place` | `Radiohead` | `Kid A` | – | `1` | `1` |
| `Radiohead/Kid A/02 Kid A.flac` | 2900 | `Kid A` | `Radiohead` | `Kid A` | – | `1` | `2` |
| `Radiohead/Kid A/10 Motion.flac` | 2921 | `Motion Picture Soundtrack` | `Radiohead` | `Kid A` | – | `1` | `10` |
| `Radiohead/OK Computer/01 Airbag.mp3` | 2587 | `Airbag` | `Radiohead` | `OK Computer` | – | – | `1` |
| `Radiohead/OK Computer/02 Paranoid.mp3` | 2597 | `Paranoid Android` | `radiohead` (lower case) | `OK Computer` | – | – | `2` |
| `Bjork/Homogenic/01 Hunter.flac` | 2886 | `Hunter` | `Björk` | `Homogenic` | – | – | `1` |
| `Bjork/Homogenic/02 Joga.flac` | 2885 | `Jóga` | `Björk` | `Homogenic` | – | – | `2` |
| `Various/Hits/01 a.flac` | 2940 | `Alpha` | `The Alphas` | `Greatest Hits` | `Various Artists` | `1` | `1` |
| `Various/Hits/02 b.flac` | 2938 | `Beta` | `The Betas` | `Greatest Hits` | `Various Artists` | `1` | `2` |
| `Various/Hits/01 c.flac` | 2940 | `Gamma` | `The Gammas` | `Greatest Hits` | `Various Artists` | `2` | `1` |
| `Queen/Hits/01 Bohemian.mp3` | 2634 | `Bohemian Rhapsody` | `Queen` | `Greatest Hits` | – | – | `1` |
| `loose/untagged song.flac` | 2817 | – | – | – | – | – | – |
| `loose/also untagged.mp3` | 2506 | – | – | – | – | – | – |
| `deep/a/b/c/d/Deep Track.flac` | 2896 | `Deep Track` | `Deep Artist` | `Deep Album` | – | – | `1` |

How the tags are stored:

- **FLAC files:** Vorbis comments in UTF-8 with upper-case keys, in this
  order, leaving out the ones marked `–`: `TITLE`, `ARTIST`, `ALBUM`,
  `ALBUMARTIST`, `DISCNUMBER`, `TRACKNUMBER`. Track and disc are plain numbers
  with no leading zero and no total (`TRACKNUMBER=10`).
- `Björk` is `42 6a c3 b6 72 6b` (ö is U+00F6) and `Jóga` is `4a c3 b3 67 61`
  (ó is U+00F3). The folder and file names are plain ASCII (`Bjork`, `Joga`).
- `Radiohead/OK Computer/01 Airbag.mp3`: ID3v2.3, 81 bytes, frames TIT2, TPE1,
  TALB, TRCK in ISO-8859-1 (encoding byte 0), no terminators, no padding. No
  ID3v1 tag.
- `Radiohead/OK Computer/02 Paranoid.mp3`: ID3v2.4, 91 bytes, frames TIT2,
  TPE1, TALB, TRCK in UTF-8 (encoding byte 3), no terminators, no padding. No
  ID3v1 tag. The artist is `radiohead`, all lower case, so that it differs
  from the other four Radiohead tracks only by case.
- `Queen/Hits/01 Bohemian.mp3`: no ID3v2 tag; an ID3v1.1 tag in the last 128
  bytes: title `Bohemian Rhapsody`, artist `Queen`, album `Greatest Hits`,
  year `1981`, empty comment, track 1, genre 255. Fields are filled with zero
  bytes.
- `loose/untagged song.flac`: a VORBIS_COMMENT block with the encoder's vendor
  string and no comments.
- `loose/also untagged.mp3`: no ID3v2 and no ID3v1 tag; starts with the Info
  frame.

Things the tree is built to show:

- Two different albums are called `Greatest Hits`: one by `Various Artists`
  (an album-artist tag, three track artists, two discs) and one by `Queen`.
- In `Various/Hits`, `01 a.flac` and `01 c.flac` are both track 1: disc 1 and
  disc 2. The album order is Alpha (1-1), Beta (1-2), Gamma (2-1), which is
  not the order of the file names.
- `Radiohead` and `radiohead` are one artist if case is ignored.
- `Kid A` has track 10 after track 2: numeric order, not text order.
- The two untagged files fall back to their file names.

## Files that must be skipped

| Path | Bytes | What it is | Why it is skipped |
|---|---|---|---|
| `.hidden/secret.flac` | 2896 | a good FLAC: `TITLE=Secret`, `ARTIST=Hidden Artist`, `ALBUM=Hidden Album`, `TRACKNUMBER=1` | the folder name starts with `.` |
| `System Volume Information/x.flac` | 2901 | a good FLAC: `TITLE=System File`, `ARTIST=System Artist`, `ALBUM=System Album`, `TRACKNUMBER=1` | the folder is `System Volume Information` |
| `notes.txt` | 79 | one line of text | not `.flac` or `.mp3` |
| `cover.jpg` | 4 | `ff d8 ff d9` (the JPEG start and end markers, no picture) | not `.flac` or `.mp3` |
| `README.md` | | this file | not `.flac` or `.mp3` |

If `Hidden Artist` or `System Artist` shows up in the index, the scanner went
into a folder it should not have.

## Files with an audio extension that are not audio

These have the right extension, so the scanner will try them. They have no
tags to read and cannot be played. The scan must carry on past them; whether
they are left out of the index or listed under their file names is for the
scanner's tests to decide.

| Path | Bytes | What it is |
|---|---|---|
| `loose/empty.flac` | 0 | an empty file |
| `loose/text.mp3` | 512 | plain text: `This is not audio. It is plain text for the player's error handling.` and a newline, repeated and cut at 512 bytes |
