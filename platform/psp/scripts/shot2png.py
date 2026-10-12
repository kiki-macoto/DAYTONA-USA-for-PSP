#!/usr/bin/env python3
"""The PSP's frame dumps (build_psp.py game --inputs ... --dump-every N:
shots/frame_NNNNN.raw next to EBOOT.PBP, the GE's frame buffer as it is:
RGB565 with red in the low bits, 512 pixels a row, 272 rows) to PNG, the
game's area only (351 x 272 from x = 64), standard library only.

  shot2png.py IN.raw OUT.png
  shot2png.py DIR          (every .raw in DIR, each next to itself)

Frame dumps are game output: keep them out of the repository.
"""
import struct
import sys
import zlib
from pathlib import Path

STRIDE, ROWS = 512, 272
X0, WIDTH = 64, 351  # renderer.h: kOffsetX, kW * kScale


def convert(src, dst):
    data = Path(src).read_bytes()
    rows = []
    for y in range(ROWS):
        row = bytearray(b"\0")
        line = struct.unpack_from(f"<{STRIDE}H", data, y * STRIDE * 2)
        for x in range(X0, X0 + WIDTH):
            p = line[x]
            r, g, b = p & 0x1F, (p >> 5) & 0x3F, p >> 11
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, ROWS, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    Path(dst).write_bytes(png)


def main():
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        for raw in sorted(Path(sys.argv[1]).glob("*.raw")):
            convert(raw, raw.with_suffix(".png"))
    elif len(sys.argv) == 3:
        convert(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
