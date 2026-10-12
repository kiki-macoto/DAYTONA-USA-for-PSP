#!/usr/bin/env python3
"""A picture around the game on the PSP's screen (a cabinet's bezel, say), as
the game reads it: overlay.bin next to EBOOT.PBP, 480 x 272 pixels in the
frame buffer's RGB565 (red in the low bits), rows of 480, little-endian.

  make_overlay.py PICTURE.png overlay.bin [--window X0,Y0,X1,Y1]

The picture's window for the game is its white area (every channel 200 or
more; or --window, in the picture's pixels, X1 and Y1 past the last). The
picture is scaled so the window is the screen's height (the game's 272
lines) and centred on the game (the screen's middle); what falls outside the
screen is cut off, and the window itself is black. The game draws only its
own 351 columns, so the picture shows at the sides.

PNG only (8 bits a channel, not interlaced: the standard library decodes it).
Python's standard library only, so build_psp.py can run it anywhere.
"""
import argparse
import struct
import sys
import zlib

W, H = 480, 272


def read_png(path):
    """(width, height, rows of (r, g, b) tuples)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"make_overlay: {path} is not a PNG")
    pos, idat, palette = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or channels is None:
        sys.exit(f"make_overlay: {path}: only 8-bit, non-interlaced PNGs (this one: {depth} bits, "
                 f"colour type {ctype}, interlace {interlace})")
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 3:
            rows.append([palette[v] + (255,) for v in line])
        elif ctype == 0:
            rows.append([(v, v, v, 255) for v in line])
        elif ctype == 4:
            rows.append([(line[i],) * 3 + (line[i + 1],) for i in range(0, stride, 2)])
        elif ctype == 2:
            rows.append([tuple(line[i:i + 3]) + (255,) for i in range(0, stride, 3)])
        else:
            rows.append([tuple(line[i:i + 4]) for i in range(0, stride, 4)])
    return w, h, rows


def in_window(p, alpha):
    """A pixel of the window: see-through (a picture with an alpha channel),
    else white."""
    return p[3] < 128 if alpha else min(p[:3]) >= 200


def find_window(w, h, rows, alpha):
    xs, ys = [], []
    for y in range(h):
        row = rows[y]
        for x in range(w):
            if in_window(row[x], alpha):
                xs.append(x)
                ys.append(y)
    if not xs:
        sys.exit("make_overlay: no see-through or white window in the picture (give --window X0,Y0,X1,Y1)")
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("picture")
    ap.add_argument("out")
    ap.add_argument("--window", help="X0,Y0,X1,Y1: the game's window in the picture (default: its white area)")
    args = ap.parse_args()
    w, h, rows = read_png(args.picture)
    alpha = any(p[3] < 128 for row in rows for p in row)
    x0, y0, x1, y1 = (map(int, args.window.split(","))) if args.window else find_window(w, h, rows, alpha)
    # The window black (what the game's own frame shows beside its picture),
    # and the picture over black where it is see-through.
    for y in range(h):
        row = rows[y]
        for x in range(w):
            p = row[x]
            if x0 <= x < x1 and y0 <= y < y1 and (args.window or not alpha and in_window(p, alpha)):
                row[x] = (0, 0, 0)
            else:
                row[x] = (p[0] * p[3] // 255, p[1] * p[3] // 255, p[2] * p[3] // 255)
    s = (y1 - y0) / H  # picture pixels per screen pixel
    cx = (x0 + x1) / 2
    out = bytearray()
    for sy in range(H):
        top, bottom = y0 + sy * s, y0 + (sy + 1) * s
        for sx in range(W):
            left, right = cx + (sx - W / 2) * s, cx + (sx + 1 - W / 2) * s
            # The average of the picture's pixels under this one (a box);
            # black off the picture.
            r = g = b = n = 0
            for py in range(int(top), max(int(top) + 1, int(-(-bottom // 1)))):
                for px in range(int(left // 1), max(int(left // 1) + 1, int(-(-right // 1)))):
                    if 0 <= px < w and 0 <= py < h:
                        p = rows[py][px]
                        r, g, b = r + p[0], g + p[1], b + p[2]
                    n += 1
            r, g, b = r // n, g // n, b // n
            out += struct.pack("<H", (r >> 3) | (g >> 2) << 5 | (b >> 3) << 11)
    open(args.out, "wb").write(out)
    print(f"make_overlay: {args.out} (the window {x0},{y0}-{x1},{y1} of {w} x {h}, "
          f"{1 / s:.3f} screen pixels a picture pixel)")


if __name__ == "__main__":
    main()
