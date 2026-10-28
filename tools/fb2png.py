#!/usr/bin/env python3
"""Framebuffer dump (main.c's fb: amiga_row_t rows, src/amiga/amiga.h) to PNG.

    fb2png.py fb.bin <sizeof(amiga_row_t)> <rows> out.png

Each row expands to two 720-pixel output lines exactly as amiga_fb_line()
(src/amiga/fb.c) does for the scanout: DIRECT (360 x 0x0RGB, each pixel
twice), IDX8 (720 indices into pal[64]), LACE4 (two lines of 720 nibbles,
pal[0..15] and pal[16..31]).  The rows are little-endian, as on the RP2350.
No third-party modules: the PNG is written with zlib.
"""
import struct
import sys
import zlib

FB_W, OUT_W = 360, 720
DIRECT, IDX8, LACE4 = 0, 1, 2


def rgb(c):
    return bytes((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17))


def row_lines(row):
    mode = row[0]
    pal = struct.unpack_from("<64H", row, 4)
    px = row[4 + 128:]
    if mode == IDX8:
        line = b"".join(rgb(pal[px[i] & 63]) for i in range(OUT_W))
        return [line, line]
    if mode == LACE4:
        out = []
        for sub in (0, 1):
            p, n = pal[16 * sub:16 * sub + 16], px[FB_W * sub:FB_W * sub + FB_W]
            out.append(b"".join(rgb(p[b >> 4]) + rgb(p[b & 15]) for b in n))
        return out
    words = struct.unpack_from("<%dH" % FB_W, px, 0)
    line = b"".join(rgb(c) * 2 for c in words)
    return [line, line]


def png(path, w, h, lines):
    raw = b"".join(b"\x00" + l for l in lines)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    fb, rowsize, rows, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    data = open(fb, "rb").read()
    if len(data) < rowsize * rows:
        sys.exit("fb2png: %s holds %d bytes, want %d x %d" % (fb, len(data), rows, rowsize))
    lines = []
    for r in range(rows):
        lines += row_lines(data[r * rowsize:(r + 1) * rowsize])
    png(out, OUT_W, 2 * rows, lines)


if __name__ == "__main__":
    main()
