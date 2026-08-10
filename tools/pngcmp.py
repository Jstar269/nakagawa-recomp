# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Compare two framebuffer PNGs written by tools/ppm2png.py.

Route evidence for a visual issue is only as good as the comparison behind it, and
"they look the same to me" is not a comparison. This decodes both images and reports
per-pixel agreement over an optional crop, so a claim that a scene renders identically
across two routes can be stated as a number.

Only the exact subset ppm2png.py emits is supported: 8-bit RGB, non-interlaced, one
IDAT stream, filter type 0 on every scanline. Anything else is rejected loudly rather
than silently mis-decoded.

Usage:
  python tools/pngcmp.py a.png b.png [--crop x0,y0,x1,y1] [--tolerance N]
"""

import argparse
import struct
import sys
import zlib


def decode(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, hdr = 8, [], None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", payload)
        elif tag == b"IDAT":
            idat.append(payload)
        elif tag == b"IEND":
            break
        pos += 12 + length
    if hdr is None:
        raise SystemExit(f"{path}: no IHDR")
    w, h, depth, color, comp, filt, interlace = hdr
    if (depth, color, interlace) != (8, 2, 0):
        raise SystemExit(f"{path}: unsupported PNG (depth={depth} color={color} interlace={interlace})")
    raw = zlib.decompress(b"".join(idat))
    stride = w * 3
    rows = []
    for y in range(h):
        off = y * (stride + 1)
        if raw[off] != 0:
            raise SystemExit(f"{path}: row {y} uses filter {raw[off]}; only filter 0 is supported")
        rows.append(raw[off + 1:off + 1 + stride])
    return w, h, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--crop", help="x0,y0,x1,y1 (exclusive right/bottom)")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="max per-channel absolute difference still counted as equal")
    args = ap.parse_args()

    wa, ha, ra = decode(args.a)
    wb, hb, rb = decode(args.b)
    if (wa, ha) != (wb, hb):
        raise SystemExit(f"size mismatch: {wa}x{ha} vs {wb}x{hb}")

    x0, y0, x1, y1 = 0, 0, wa, ha
    if args.crop:
        x0, y0, x1, y1 = (int(v) for v in args.crop.split(","))
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(wa, x1), min(ha, y1)
    if x1 <= x0 or y1 <= y0:
        raise SystemExit("empty crop")

    total = same = 0
    worst = 0
    for y in range(y0, y1):
        row_a, row_b = ra[y], rb[y]
        for x in range(x0, x1):
            i = x * 3
            d = max(abs(row_a[i + c] - row_b[i + c]) for c in range(3))
            total += 1
            if d <= args.tolerance:
                same += 1
            if d > worst:
                worst = d
    pct = 100.0 * same / total if total else 0.0
    print(f"region=({x0},{y0})-({x1},{y1}) pixels={total} "
          f"identical={same} ({pct:.2f}%) worst_channel_delta={worst} tolerance={args.tolerance}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
