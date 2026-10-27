#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Draw the Windows app icon, ``assets/brand/nakagawa.ico``, from the N1 mark.

The icon is the project's N1 Block & Branch mark in its light-ink colours
(``docs/images/brand/nakagawa-mark-dark.svg``) on a rounded ink tile, so it reads
on light and dark taskbars alike. Every size is drawn from the SVG's rectangles
and polygon at that size, with exact pixel coverage, not scaled down from one
large bitmap. Sizes 16 to 48 are stored as 32-bit BMP images with alpha, 64 to
256 as PNG, which is what Windows expects of a modern icon.

``assets/brand/nakagawa.rc`` puts the icon in the Windows player and game
executables, and ``tools/stage_install.py`` copies it for the desktop shortcut.
On other systems the player sets the same picture as its SDL window icon:
``nk_ui_app_icon_rgba()`` in ``src/ui/nk_ui_widgets.c`` draws it with the
constants below, and ``tools/test_app_icon.py`` checks that the two agree.

    python tools/make_app_icon.py                  # rewrite assets/brand/nakagawa.ico
    python tools/make_app_icon.py --check          # exit 1 when the committed icon's pixels differ
    python tools/make_app_icon.py --png 256 <file> # also write one size as a PNG to look at

``--check`` compares decoded pixels, not file bytes, because zlib builds may
compress the PNG images differently.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[1]
MARK_SVG = Path("docs/images/brand/nakagawa-mark-dark.svg")
OUTPUT = Path("assets/brand/nakagawa.ico")

SIZES = (16, 24, 32, 48, 64, 128, 256)
#: Sizes up to this one are stored as BMP, larger ones as PNG.
LARGEST_BMP = 48
#: The tile: N1 ink (the theme's ``ink``), corner radius and the mark's inset, both
#: as a fraction of the icon size. Keep in step with NK_UI_APP_ICON_* in nk_ui_widgets.c.
TILE_RGB = (0x10, 0x15, 0x1C)
CORNER_FRACTION = 0.18
INSET_FRACTION = 0.16
#: Samples per pixel side for the tile's rounded corners.
CORNER_SAMPLES = 16

SVG_NS = "{http://www.w3.org/2000/svg}"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

Point = tuple[float, float]
Shape = tuple[tuple[int, int, int], list[Point]]


def _hex_rgb(value: str) -> tuple[int, int, int]:
    text = value.strip().lstrip("#")
    if len(text) != 6:
        raise ValueError(f"make_app_icon: fill {value!r} is not #RRGGBB")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


def load_mark(svg_path: Path = ROOT / MARK_SVG) -> tuple[tuple[float, float, float, float], list[Shape]]:
    """Return the mark's view box and its filled shapes as convex polygons, in paint order."""
    root = ET.fromstring(svg_path.read_text(encoding="utf-8"))
    view_box = tuple(float(v) for v in root.attrib["viewBox"].split())
    if len(view_box) != 4:
        raise ValueError("make_app_icon: the mark's viewBox needs four numbers")
    shapes: list[Shape] = []
    for element in root:
        tag = element.tag.removeprefix(SVG_NS)
        if tag == "title":
            continue
        fill = _hex_rgb(element.attrib["fill"])
        if tag == "rect":
            x, y = float(element.attrib["x"]), float(element.attrib["y"])
            w, h = float(element.attrib["width"]), float(element.attrib["height"])
            shapes.append((fill, [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]))
        elif tag == "polygon":
            numbers = [float(v) for v in element.attrib["points"].replace(",", " ").split()]
            shapes.append((fill, list(zip(numbers[0::2], numbers[1::2]))))
        else:
            raise ValueError(f"make_app_icon: unexpected <{tag}> in the mark")
    return view_box, shapes  # type: ignore[return-value]


def _clip(poly: list[Point], axis: int, bound: float, keep_above: bool) -> list[Point]:
    out: list[Point] = []
    count = len(poly)
    for i in range(count):
        a, b = poly[i], poly[(i + 1) % count]
        a_in = a[axis] >= bound if keep_above else a[axis] <= bound
        b_in = b[axis] >= bound if keep_above else b[axis] <= bound
        if a_in:
            out.append(a)
        if a_in != b_in:
            t = (bound - a[axis]) / (b[axis] - a[axis])
            out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
    return out


def _area(poly: list[Point]) -> float:
    total = 0.0
    count = len(poly)
    for i in range(count):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % count]
        total += x0 * y1 - x1 * y0
    return abs(total) * 0.5


def _coverage(poly: list[Point], px: int, py: int) -> float:
    """Exact area of the convex polygon inside pixel (px, py)."""
    clipped = _clip(poly, 0, float(px), True)
    if clipped:
        clipped = _clip(clipped, 0, float(px + 1), False)
    if clipped:
        clipped = _clip(clipped, 1, float(py), True)
    if clipped:
        clipped = _clip(clipped, 1, float(py + 1), False)
    return _area(clipped) if len(clipped) >= 3 else 0.0


def _tile_coverage(size: int, px: int, py: int) -> float:
    radius = size * CORNER_FRACTION
    near_x = px < radius or px + 1 > size - radius
    near_y = py < radius or py + 1 > size - radius
    if not (near_x and near_y):
        return 1.0
    cx = radius if px < radius else size - radius
    cy = radius if py < radius else size - radius
    inside = 0
    for sy in range(CORNER_SAMPLES):
        y = py + (sy + 0.5) / CORNER_SAMPLES
        for sx in range(CORNER_SAMPLES):
            x = px + (sx + 0.5) / CORNER_SAMPLES
            dx = x - cx if (x < radius or x > size - radius) else 0.0
            dy = y - cy if (y < radius or y > size - radius) else 0.0
            if dx * dx + dy * dy <= radius * radius:
                inside += 1
    return inside / (CORNER_SAMPLES * CORNER_SAMPLES)


def _to_byte(value: float) -> int:
    return max(0, min(255, int(value * 255.0 + 0.5)))


def render(size: int, svg_path: Path = ROOT / MARK_SVG) -> bytes:
    """Draw the icon at ``size`` pixels; return straight-alpha RGBA rows, top row first."""
    view_box, shapes = load_mark(svg_path)
    vx, vy, vw, vh = view_box
    inset = size * INSET_FRACTION
    scale = (size - 2.0 * inset) / max(vw, vh)
    placed: list[tuple[tuple[float, float, float], list[Point], tuple[float, float, float, float]]] = []
    for fill, points in shapes:
        rgb = (fill[0] / 255.0, fill[1] / 255.0, fill[2] / 255.0)
        poly = [(inset + (x - vx) * scale, inset + (y - vy) * scale) for x, y in points]
        xs, ys = [x for x, _ in poly], [y for _, y in poly]
        placed.append((rgb, poly, (min(xs), min(ys), max(xs), max(ys))))
    tile = (TILE_RGB[0] / 255.0, TILE_RGB[1] / 255.0, TILE_RGB[2] / 255.0)
    out = bytearray()
    for py in range(size):
        for px in range(size):
            alpha = _tile_coverage(size, px, py)
            r, g, b = tile[0] * alpha, tile[1] * alpha, tile[2] * alpha
            for rgb, poly, (x0, y0, x1, y1) in placed:
                if px + 1 <= x0 or px >= x1 or py + 1 <= y0 or py >= y1:
                    continue
                cover = _coverage(poly, px, py)
                if cover <= 0.0:
                    continue
                r = rgb[0] * cover + r * (1.0 - cover)
                g = rgb[1] * cover + g * (1.0 - cover)
                b = rgb[2] * cover + b * (1.0 - cover)
                alpha = cover + alpha * (1.0 - cover)
            if alpha > 0.0:
                r, g, b = r / alpha, g / alpha, b / alpha
            out += bytes((_to_byte(r), _to_byte(g), _to_byte(b), _to_byte(alpha)))
    return bytes(out)


def _png(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    stride = size * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (PNG_SIGNATURE + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _bmp(size: int, rgba: bytes) -> bytes:
    """An icon DIB: BITMAPINFOHEADER, BGRA rows bottom-up, then the 1-bit AND mask."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    stride = size * 4
    pixels = bytearray()
    for y in range(size - 1, -1, -1):
        row = rgba[y * stride:(y + 1) * stride]
        for x in range(size):
            r, g, b, a = row[x * 4:x * 4 + 4]
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(mask_stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + bytes(pixels) + bytes(mask)


def build_ico(svg_path: Path = ROOT / MARK_SVG) -> bytes:
    images = []
    for size in SIZES:
        rgba = render(size, svg_path)
        images.append((size, _bmp(size, rgba) if size <= LARGEST_BMP else _png(size, rgba)))
    directory = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for size, data in images:
        dim = 0 if size == 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return directory + entries + b"".join(data for _, data in images)


def _decode_png(data: bytes) -> tuple[int, bytes]:
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise ValueError("PNG is not 8-bit RGBA, non-interlaced")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if width != height:
        raise ValueError("PNG is not square")
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        row = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        if row[0] != 0:
            raise ValueError("PNG row uses a filter this reader does not undo")
        out += row[1:]
    return width, bytes(out)


def _decode_bmp(data: bytes) -> tuple[int, bytes]:
    _, size, height2, _, bits = struct.unpack("<IiiHH", data[:16])
    if bits != 32 or height2 != size * 2:
        raise ValueError("BMP is not a 32-bit square icon image")
    stride = size * 4
    pixels = data[40:40 + size * stride]
    out = bytearray()
    for y in range(size - 1, -1, -1):
        row = pixels[y * stride:(y + 1) * stride]
        for x in range(size):
            b, g, r, a = row[x * 4:x * 4 + 4]
            out += bytes((r, g, b, a))
    return size, bytes(out)


def read_ico(data: bytes) -> list[tuple[int, str, bytes]]:
    """Return (size, "bmp" or "png", RGBA rows) for each image in an .ico."""
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or kind != 1:
        raise ValueError("not an icon file")
    images = []
    for index in range(count):
        dim, _, _, _, planes, bits, length, offset = struct.unpack(
            "<BBBBHHII", data[6 + 16 * index:22 + 16 * index])
        if planes != 1 or bits != 32:
            raise ValueError(f"image {index} is not 32-bit")
        body = data[offset:offset + length]
        if body[:8] == PNG_SIGNATURE:
            size, rgba = _decode_png(body)
            images.append((size, "png", rgba))
        else:
            size, rgba = _decode_bmp(body)
            images.append((size, "bmp", rgba))
        if size != (dim or 256):
            raise ValueError(f"image {index} is {size} px but its entry says {dim or 256}")
    return images


def png_image(data: bytes, size: int) -> bytes:
    """The PNG bytes stored in an .ico for ``size`` pixels (sizes above LARGEST_BMP)."""
    _, _, count = struct.unpack("<HHH", data[:6])
    for index in range(count):
        dim, _, _, _, _, _, length, offset = struct.unpack("<BBBBHHII", data[6 + 16 * index:22 + 16 * index])
        body = data[offset:offset + length]
        if (dim or 256) == size and body[:8] == PNG_SIGNATURE:
            return body
    raise ValueError(f"no {size} px PNG image in the icon")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=ROOT / OUTPUT, help="icon file to write")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the committed icon's pixels differ from a fresh drawing")
    parser.add_argument("--png", nargs=2, metavar=("SIZE", "FILE"),
                        help="also write the icon at SIZE pixels as a PNG")
    args = parser.parse_args(argv)
    if args.png:
        size = int(args.png[0])
        Path(args.png[1]).write_bytes(_png(size, render(size)))
    if args.check:
        committed = read_ico(args.out.read_bytes())
        fresh = [(size, render(size)) for size in SIZES]
        if [(size, rgba) for size, _, rgba in committed] != fresh:
            print(f"make_app_icon: {args.out} is stale; run python tools/make_app_icon.py", file=sys.stderr)
            return 1
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(build_ico())
    return 0


if __name__ == "__main__":
    sys.exit(main())
