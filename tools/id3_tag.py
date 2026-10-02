#!/usr/bin/env python3
"""Write one ID3 tag to standard output, byte for byte as asked.

Used by tools/make_test_audio.sh for the tag fixtures. ffmpeg picks text
encodings and frame order itself; a tag reader's tests need to know exactly
which bytes are in the file, so the tags are built here instead.

  id3_tag.py v2 <3|4> [--padding N] [--claim N] FRAME...
  id3_tag.py v1 <title> <artist> <album> <year> <track>

FRAME is one of:

  ID=ENC:text               a text frame, for example TIT2=latin1:Airbag
  TXXX=ENC:description:text a user-defined text frame
  APIC=path                 a front-cover picture frame holding that PNG file

ENC names the text encoding byte and the bytes that follow it:

  latin1       0  ISO-8859-1
  utf16        1  UTF-16, little-endian, after the byte order mark FF FE
  utf16be-bom  1  UTF-16, big-endian, after the byte order mark FE FF
  utf16be      2  UTF-16, big-endian, no byte order mark   (ID3v2.4 only)
  utf8         3  UTF-8                                    (ID3v2.4 only)

Text is written without a terminator unless ENC ends in "+nul" (for example
utf16+nul), which adds one: 00, or 00 00 for the UTF-16 encodings.

--padding N adds N zero bytes after the last frame, inside the tag.
--claim N writes N as the tag size in the header instead of the true size
(for a fixture whose tag claims to be longer than the file).

ID3v2.3 frame sizes are plain 32-bit big-endian numbers. ID3v2.4 frame sizes
and the tag size of both versions are "sync-safe": 7 bits per byte.

The ID3v1 tag is version 1.1: 128 bytes, ISO-8859-1, fields filled with zero
bytes, the track number in the last byte of the comment field, genre 255.
A 30-character field therefore has no terminator.
"""
import os
import sys

# name: (encoding byte, byte order mark, Python codec)
ENCODINGS = {
    "latin1":      (0, b"",         "iso-8859-1"),
    "utf16":       (1, b"\xff\xfe", "utf-16-le"),
    "utf16be-bom": (1, b"\xfe\xff", "utf-16-be"),
    "utf16be":     (2, b"",         "utf-16-be"),
    "utf8":        (3, b"",         "utf-8"),
}


def fail(message):
    sys.exit("id3_tag: " + message)


def syncsafe(n):
    if not 0 <= n < 1 << 28:
        fail("%d does not fit in a sync-safe size" % n)
    return bytes([n >> 21 & 0x7F, n >> 14 & 0x7F, n >> 7 & 0x7F, n & 0x7F])


class Encoding:
    def __init__(self, name, version):
        self.terminate = name.endswith("+nul")
        base = name[:-4] if self.terminate else name
        if base not in ENCODINGS:
            fail("unknown encoding '%s'" % name)
        self.byte, self.bom, self.codec = ENCODINGS[base]
        if self.byte >= 2 and version != 4:
            fail("encoding '%s' exists only in ID3v2.4" % base)
        self.nul = b"\x00\x00" if self.codec.startswith("utf-16") else b"\x00"

    def text(self, s, terminate):
        return self.bom + s.encode(self.codec) + (self.nul if terminate else b"")


def frame_body(frame_id, value, version):
    if frame_id == "APIC":
        with open(value, "rb") as f:
            picture = f.read()
        if not picture.startswith(b"\x89PNG"):
            fail("%s is not a PNG file" % value)
        # Latin-1 description (empty), MIME type, picture type 3 = front cover.
        return b"\x00" + b"image/png\x00" + b"\x03" + b"\x00" + picture
    name, _, rest = value.partition(":")
    enc = Encoding(name, version)
    if frame_id == "TXXX":
        description, _, text = rest.partition(":")
        return (bytes([enc.byte]) + enc.text(description, True)
                + enc.text(text, enc.terminate))
    if not frame_id.startswith("T"):
        fail("do not know how to write a %s frame" % frame_id)
    return bytes([enc.byte]) + enc.text(rest, enc.terminate)


def id3v2(args):
    version = int(args.pop(0))
    if version not in (3, 4):
        fail("version must be 3 or 4")
    padding, claim = 0, None
    while args and args[0].startswith("--"):
        option = args.pop(0)
        if option == "--padding":
            padding = int(args.pop(0))
        elif option == "--claim":
            claim = int(args.pop(0))
        else:
            fail("unknown option " + option)
    frames = b""
    for arg in args:
        frame_id, sep, value = arg.partition("=")
        if not sep or len(frame_id) != 4:
            fail("bad frame '%s'" % arg)
        body = frame_body(frame_id, value, version)
        size = syncsafe(len(body)) if version == 4 else len(body).to_bytes(4, "big")
        frames += frame_id.encode("ascii") + size + b"\x00\x00" + body
    frames += bytes(padding)
    # "ID3", version, revision 0, flags 0, size of everything after this header.
    header = b"ID3" + bytes([version, 0, 0])
    return header + syncsafe(len(frames) if claim is None else claim) + frames


def id3v1(args):
    if len(args) != 5:
        fail("v1 takes: title artist album year track")
    title, artist, album, year, track = args

    def field(s, width):
        raw = s.encode("iso-8859-1")
        if len(raw) > width:
            fail("'%s' is longer than %d bytes" % (s, width))
        return raw.ljust(width, b"\x00")

    track = int(track)
    if not 1 <= track <= 255:
        fail("track must be 1..255")
    # The comment is 28 bytes, then a zero byte, then the track: ID3v1.1.
    tag = (b"TAG" + field(title, 30) + field(artist, 30) + field(album, 30)
           + field(year, 4) + bytes(28) + b"\x00" + bytes([track]) + b"\xff")
    assert len(tag) == 128
    return tag


def main():
    # The arguments are UTF-8 whatever the locale says.
    args = [os.fsencode(a).decode("utf-8") for a in sys.argv[1:]]
    if len(args) < 2 or args[0] not in ("v1", "v2"):
        sys.exit(__doc__.strip())
    tag = id3v2(args[1:]) if args[0] == "v2" else id3v1(args[1:])
    sys.stdout.buffer.write(tag)


if __name__ == "__main__":
    main()
