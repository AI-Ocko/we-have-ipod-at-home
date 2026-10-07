#!/usr/bin/env python3
"""Write the playlist fixtures for test/host/test_playlist.c.

Usage: tools/make_test_playlists.py

Every byte that matters (the UTF-8 byte-order mark, CR LF endings, a missing
final newline, UTF-8 names, binary junk) is written here with an escape, so
the committed files can be checked against this script: a re-run gives the
same bytes. test/host/fixtures/playlists/.gitattributes keeps git from
touching their line endings.

The paths name the 16 tracks of test/host/lib16.h, taken as paths from the
card root, plus the one UTF-8 track the test adds to them:
"Sigur R\u00f3s/\u00c1g\u00e6tis byrjun/01 Intro.flac".

  device.m3u          as the device writes it: "../" + path, LF, nothing else
  computer.m3u8       as a computer writes it: BOM, CR LF, #EXTM3U, #EXTINF,
                      drive letters, backslashes, file:// URLs with %XX,
                      paths relative to /playlists/ and to the card root,
                      letters in the wrong case, UTF-8, a missing entry, a
                      blank line, absolute paths under a computer's mount
                      point (/run/media/<user>/<card>/, file:///run/media/,
                      /Volumes/<card>/, /media/<user>/<card>/), a line with
                      spaces and tabs at both ends, and a comment after the
                      last entry
  missing.m3u         good entries between ones that match nothing
  blocks.m3u          #EXTM3U, entries with #EXTINF and blank lines before
                      them, and two lines after the last entry
  no_final_newline.m3u  two entries, the last with no newline after it
  over_limit.m3u      1,003 entries (track i % 16 of the list below)
  too_long.m3u        a good entry, then a line of 1,025 bytes
  junk.m3u            6,000 bytes of deterministic binary junk, cut into
                      lines of at most 1,000 bytes
  empty.m3u           no bytes at all
"""

import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "test", "host", "fixtures", "playlists")

BOM = b"\xef\xbb\xbf"
UTF8_TRACK = "Sigur R\u00f3s/\u00c1g\u00e6tis byrjun/01 Intro.flac".encode()

# The 16 paths of lib16.h, in its row order.
LIB16 = [
    b"Radiohead/Kid A/01 Everything.flac",
    b"Radiohead/Kid A/02 Kid A.flac",
    b"Radiohead/Kid A/10 Motion.flac",
    b"Radiohead/OK Computer/01 Airbag.mp3",
    b"Radiohead/OK Computer/02 Paranoid.mp3",
    b"Bjork/Homogenic/01 Hunter.flac",
    b"Bjork/Homogenic/02 Joga.flac",
    b"Various/Hits/01 a.flac",
    b"Various/Hits/02 b.flac",
    b"Various/Hits/01 c.flac",
    b"Queen/Hits/01 Bohemian.mp3",
    b"loose/untagged song.flac",
    b"loose/also untagged.mp3",
    b"loose/empty.flac",
    b"loose/text.mp3",
    b"deep/a/b/c/d/Deep Track.flac",
]


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def lf(lines):
    return b"".join(l + b"\n" for l in lines)


def main():
    os.makedirs(OUT, exist_ok=True)
    write(".gitattributes", b"* -text\n")

    write("device.m3u", lf([b"../" + LIB16[i] for i in (0, 5, 10, 15, 11)]
                           + [b"../" + UTF8_TRACK]))

    computer = [
        b"#EXTM3U",
        b"#EXTINF:100,Radiohead - Kid A",
        b"E:\\Radiohead\\Kid A\\02 Kid A.flac",                  # drive, \
        b"#EXTINF:100,Bjork - Hunter",
        b"file:///E:/Bjork/Homogenic/01%20Hunter.flac",          # file://
        b"",                                                     # blank
        b"..\\Queen\\Hits\\01 Bohemian.mp3",                     # from /playlists/
        b"loose/untagged song.flac",                             # from the root
        b"RADIOHEAD/ok computer/01 AIRBAG.MP3",                  # wrong case
        b"#EXTINF:100,Sigur Ros - Intro",
        b"/" + UTF8_TRACK,                                       # UTF-8
        b"file:///e:/Sigur%20R%C3%B3s/%C3%81g%C3%A6tis%20byrjun/01%20Intro.flac",
        b"e:/Gone/Not Here.flac",                                # missing
        b"/deep/a/b/c/d/Deep Track.flac",
        # Absolute paths with the computer's mount point in front: found
        # again with their leading folders removed.
        b"#EXTINF:100,Radiohead - Motion Picture Soundtrack",
        b"/run/media/christopher/SDCARD/Radiohead/Kid A/10 Motion.flac",
        b"file:///run/media/christopher/SD%20CARD/Various/Hits/01%20a.flac",
        b"/Volumes/SDCARD/Various/Hits/02 b.flac",               # macOS
        b"/media/christopher/SDCARD/Radiohead/OK Computer/02 Paranoid.mp3",
        b" \tloose/also untagged.mp3 \t ",                       # spaces, tabs
        b"# made by hand",                                       # after the last
    ]
    write("computer.m3u8", BOM + b"".join(l + b"\r\n" for l in computer))

    write("missing.m3u", lf([
        b"../Gone/one.flac",
        b"../" + LIB16[1],
        b"Radiohead/Kid A/03 Not On The Card.flac",
        b"../" + LIB16[6],
        b"http://example.com/stream.mp3",
        b"../../above/the/root.flac",
        b"../" + LIB16[12],
    ]))

    write("blocks.m3u", lf([
        b"#EXTM3U",
        b"#EXTINF:100,Radiohead - Everything",
        b"../" + LIB16[0],
        b"#EXTINF:100,Bjork - Hunter",
        b"",
        b"../" + LIB16[5],
        b"../" + LIB16[10],
        b"#EXTINF:131,Nobody - Gone",
        b"../Gone/missing.mp3",
        b"# a comment after the last entry",
        b"",
    ]))

    write("no_final_newline.m3u",
          b"../" + LIB16[5] + b"\n../" + LIB16[10])

    write("over_limit.m3u",
          lf([b"../" + LIB16[i % 16] for i in range(1003)]))

    write("too_long.m3u", lf([b"../" + LIB16[0], b"x" * 1025, b"../" + LIB16[1]]))

    # A linear congruential generator: the same bytes on every run.
    x = 12345
    junk = bytearray()
    run = 0
    while len(junk) < 6000:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        b = (x >> 16) & 0xFF
        if len(junk) == 0 and b == 0xEF:
            b = 0
        junk.append(b)
        run = 0 if b == 0x0A else run + 1
        if run == 1000:
            junk.append(0x0A)
            run = 0
    write("junk.m3u", bytes(junk))

    write("empty.m3u", b"")


if __name__ == "__main__":
    main()
