# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Batch-extract ClapHanz XB archives with parallel execution, unswizzling, and PNG validation.

Walks a directory tree for .xb / .xb0 / .xb2 / .xb3 files, extracts each
into <output>/<relative-path>.xb.d/ using the vendored libxb library,
converts Swizzled GIM textures into PNG images, and generates an inventory map.

Usage:
    python tools/extract_xb.py <xbdata-dir> [--output <out-dir>] [--workers <n>]
"""

import os
import sys
import argparse
import time
import struct
import zlib
import json
from concurrent.futures import ProcessPoolExecutor

XB_EXTENSIONS = (".xb", ".xb0", ".xb2", ".xb3")

GIM_MAX_DIMENSION = 4096
GIM_MAX_PIXELS = 16 * 1024 * 1024
GIM_MAX_RAW_BYTES = 64 * 1024 * 1024
GIM_MAX_BLOCKS = 1 << 20
GIM_MAX_NESTING = 32


def unswizzle(src, pitch, height):
    """PSP GE texture unswizzle: 16-byte x 8-line tiles."""
    if pitch % 16 != 0 or height % 8 != 0:
        return src  # Can't unswizzle non-aligned
    dst = bytearray(len(src))
    rowblocks = pitch // 16
    for y in range(height):
        for x in range(pitch):
            bx, by = x // 16, y // 8
            px, py = x % 16, y % 8
            block_idx = bx + by * rowblocks
            src_off = block_idx * 128 + py * 16 + px
            dst_off = y * pitch + x
            if src_off < len(src) and dst_off < len(dst):
                dst[dst_off] = src[src_off]
    return bytes(dst)


def decode_gim_data(data):
    """Decode GIM data to RGBA pixels (width, height, pixels_bytes)."""
    if not isinstance(data, (bytes, bytearray, memoryview)):
        return None
    if len(data) < 16 or data[0:11] != b'MIG.00.1PSP':
        return None

    image_block = None
    palette_data = None
    block_count = 0
    malformed = False

    def checked_range(start, size, limit):
        if start < 0 or size < 0 or start > limit or size > limit - start:
            return None
        return start + size

    def scan_blocks(offset, limit, depth=0):
        nonlocal palette_data, image_block, block_count, malformed
        if depth > GIM_MAX_NESTING or offset < 0 or limit < offset or limit > len(data):
            malformed = True
            return False
        while offset < limit:
            if limit - offset < 16:
                malformed = True
                return False
            block_id = struct.unpack_from('<H', data, offset)[0]
            block_size = struct.unpack_from('<I', data, offset + 4)[0]
            hdr_size = struct.unpack_from('<I', data, offset + 12)[0]
            block_end = checked_range(offset, block_size, limit)
            if block_size < 16 or hdr_size < 16 or hdr_size > block_size or block_end is None:
                malformed = True
                return False
            content = offset + hdr_size
            content_limit = block_end
            block_count += 1
            if block_count > GIM_MAX_BLOCKS:
                malformed = True
                return False
            if block_id == 0x0004:
                if content_limit - content < 36:
                    malformed = True
                    return False
                fmt = struct.unpack_from('<H', data, content + 4)[0]
                swiz = struct.unpack_from('<H', data, content + 6)[0]
                w = struct.unpack_from('<H', data, content + 8)[0]
                h = struct.unpack_from('<H', data, content + 10)[0]
                d_off = struct.unpack_from('<I', data, content + 28)[0]
                d_end = struct.unpack_from('<I', data, content + 32)[0]
                raw_len = d_end - d_off if d_end >= d_off else -1
                if (w == 0 or h == 0 or w > GIM_MAX_DIMENSION or h > GIM_MAX_DIMENSION or
                    w * h > GIM_MAX_PIXELS or raw_len < 0 or raw_len > GIM_MAX_RAW_BYTES or
                    d_end > content_limit - content):
                    malformed = True
                    return False
                if image_block is None:
                    image_block = (fmt, swiz, w, h, data[content + d_off:content + d_end])
            elif block_id == 0x0005:
                if content_limit - content < 36:
                    malformed = True
                    return False
                p_off = struct.unpack_from('<I', data, content + 28)[0]
                p_end = struct.unpack_from('<I', data, content + 32)[0]
                palette_len = p_end - p_off if p_end >= p_off else -1
                if palette_len < 0 or palette_len > GIM_MAX_RAW_BYTES or p_end > content_limit - content:
                    malformed = True
                    return False
                palette_data = data[content + p_off:content + p_end]
            elif block_id in (0x0002, 0x0003):
                if not scan_blocks(content, content_limit, depth + 1):
                    return False
            offset = block_end
        return True

    if not scan_blocks(16, len(data)) or malformed or image_block is None:
        return None

    fmt, swiz, w, h, pix_data = image_block

    # Build palette for indexed formats
    clut = None
    if palette_data and len(palette_data) >= 4:
        num_entries = len(palette_data) // 4
        clut = []
        for i in range(min(num_entries, 256)):
            val = struct.unpack_from('<I', palette_data, i * 4)[0]
            clut.append(val)

    # Determine bytes per pixel element
    bpp_map = {0: 2, 1: 2, 2: 2, 3: 4, 4: 0, 5: 1}
    bpp = bpp_map.get(fmt, 4)

    if fmt in (0, 1, 2, 3):
        pitch = w * bpp
    elif fmt == 4:
        pitch = (w + 1) // 2
    elif fmt == 5:
        pitch = w
    else:
        return None

    raw_size = pitch * h
    pixel_size = w * h * 4
    if (pitch <= 0 or raw_size > GIM_MAX_RAW_BYTES or len(pix_data) < raw_size or
        pixel_size > GIM_MAX_RAW_BYTES):
        return None

    # Unswizzle if needed
    if swiz == 1 and pitch > 0 and h > 0:
        pix_data = unswizzle(pix_data[:raw_size], pitch, h)

    # Convert to RGBA8888
    pixels = bytearray(pixel_size)

    if fmt == 3:  # RGBA8888 direct (or BGRA, depending on layout, we output RGBA for PNG)
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 4
                di = (y * w + x) * 4
                if si + 4 <= len(pix_data) and di + 4 <= len(pixels):
                    # GIM stores BGRA usually, let's map to RGBA
                    pixels[di] = pix_data[si+2] # R
                    pixels[di+1] = pix_data[si+1] # G
                    pixels[di+2] = pix_data[si] # B
                    pixels[di+3] = pix_data[si+3] # A

    elif fmt == 5:  # T8 indexed
        if clut:
            for y in range(h):
                for x in range(w):
                    si = y * pitch + x
                    di = (y * w + x) * 4
                    if si < len(pix_data) and di + 4 <= len(pixels):
                        idx = pix_data[si]
                        if idx < len(clut):
                            val = clut[idx]
                            # Clut is BGRA, swap to RGBA
                            pixels[di] = (val >> 16) & 0xFF # R
                            pixels[di+1] = (val >> 8) & 0xFF # G
                            pixels[di+2] = val & 0xFF # B
                            pixels[di+3] = (val >> 24) & 0xFF # A

    elif fmt == 4:  # T4 indexed
        if clut:
            for y in range(h):
                for x in range(w):
                    si = y * pitch + x // 2
                    di = (y * w + x) * 4
                    if si < len(pix_data) and di + 4 <= len(pixels):
                        byte = pix_data[si]
                        idx = (byte & 0x0F) if (x % 2 == 0) else (byte >> 4)
                        if idx < len(clut):
                            val = clut[idx]
                            pixels[di] = (val >> 16) & 0xFF # R
                            pixels[di+1] = (val >> 8) & 0xFF # G
                            pixels[di+2] = val & 0xFF # B
                            pixels[di+3] = (val >> 24) & 0xFF # A

    elif fmt == 0:  # BGR5650
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = (val & 0x1F); r = (r << 3) | (r >> 2)
                    g = (val >> 5) & 0x3F; g = (g << 2) | (g >> 4)
                    b = (val >> 11) & 0x1F; b = (b << 3) | (b >> 2)
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = 0xFF

    elif fmt == 1:  # ABGR5551
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = (val & 0x1F); r = (r << 3) | (r >> 2)
                    g = (val >> 5) & 0x1F; g = (g << 3) | (g >> 2)
                    b = (val >> 10) & 0x1F; b = (b << 3) | (b >> 2)
                    a = 0xFF if (val & 0x8000) else 0x00
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = a

    elif fmt == 2:  # ABGR4444
        for y in range(h):
            for x in range(w):
                si = y * pitch + x * 2
                di = (y * w + x) * 4
                if si + 2 <= len(pix_data) and di + 4 <= len(pixels):
                    val = struct.unpack_from('<H', pix_data, si)[0]
                    r = val & 0x0F; r |= r << 4
                    g = (val >> 4) & 0x0F; g |= g << 4
                    b = (val >> 8) & 0x0F; b |= b << 4
                    a = (val >> 12) & 0x0F; a |= a << 4
                    pixels[di] = r; pixels[di+1] = g; pixels[di+2] = b; pixels[di+3] = a

    return (w, h, bytes(pixels))


def save_png(w, h, rgba_pixels, out_path):
    """Write standard PNG from raw RGBA pixels without any external library."""
    raw = b"".join(b"\x00" + rgba_pixels[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) # Color type 6 = RGBA
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))

    with open(out_path, "wb") as f:
        f.write(png)


def process_extracted_directory(dest_dir):
    """Scan extracted directory to classify files and convert GIMs to PNGs."""
    inventory = {
        "textures": [],
        "sounds": [],
        "scene_graphs": [],
        "other": []
    }

    for dirpath, _, filenames in os.walk(dest_dir):
        for fn in filenames:
            fpath = os.path.join(dirpath, fn)
            rel_fpath = os.path.relpath(fpath, dest_dir)
            sz = os.path.getsize(fpath)

            # Skip generated pngs themselves
            if fn.lower().endswith(".png"):
                continue

            is_gim = fn.lower().endswith(".gim")
            if not is_gim:
                try:
                    with open(fpath, "rb") as f:
                        magic = f.read(11)
                        if magic == b'MIG.00.1PSP':
                            is_gim = True
                except Exception:
                    pass

            if is_gim:
                png_fn = os.path.splitext(fn)[0] + ".png"
                png_path = os.path.join(dirpath, png_fn)
                rel_png_path = os.path.relpath(png_path, dest_dir)
                try:
                    with open(fpath, "rb") as f:
                        gim_data = f.read()
                    decoded = decode_gim_data(gim_data)
                    if decoded:
                        w, h, rgba = decoded
                        save_png(w, h, rgba, png_path)
                        inventory["textures"].append({
                            "name": fn,
                            "path": rel_fpath,
                            "png_path": rel_png_path,
                            "width": w,
                            "height": h,
                            "size_bytes": sz
                        })
                        continue
                except Exception as e:
                    pass

            # Check sounds
            if fn.lower().endswith((".sgd", ".sgb", ".vag", ".at3", ".wav")):
                inventory["sounds"].append({
                    "name": fn,
                    "path": rel_fpath,
                    "size_bytes": sz
                })
                continue

            # Check layouts/scene-graphs
            is_layout = fn.lower().endswith((".xb0", ".xb1", ".xb2", ".xb3", ".dec"))
            if not is_layout:
                try:
                    with open(fpath, "rb") as f:
                        header = f.read(128)
                        if b"MAP1" in header or b"LAY1" in header:
                            is_layout = True
                except Exception:
                    pass

            if is_layout:
                inventory["scene_graphs"].append({
                    "name": fn,
                    "path": rel_fpath,
                    "size_bytes": sz
                })
                continue

            # Other files
            inventory["other"].append({
                "name": fn,
                "path": rel_fpath,
                "size_bytes": sz
            })

    # Write inventory map
    inv_path = os.path.join(dest_dir, "inventory_map.json")
    with open(inv_path, "w", encoding="utf-8") as f:
        json.dump(inventory, f, indent=4)


def find_xb_files(root):
    """Recursively find all XB archive files under root."""
    matches = []
    for dirpath, _, filenames in os.walk(root):
        for fn in sorted(filenames):
            if fn.lower().endswith(XB_EXTENSIONS):
                matches.append(os.path.join(dirpath, fn))
    return matches


def extract_one(archive_path, out_dir, verbose=False):
    """Extract a single XB archive. Returns (archive_path, ok, error_msg)."""
    # Add libxb to path inside child process
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, os.path.join(ROOT, "third_party", "libxb", "src"))

    try:
        from libxb import XBArchive, XBOpenMode, XBEndian  # type: ignore
        with XBArchive(archive_path, XBOpenMode.READ, XBEndian.LITTLE, verbose) as arc:
            arc.extract_all(path=out_dir)
        process_extracted_directory(out_dir)
        return archive_path, True, None
    except Exception as e:
        return archive_path, False, str(e)


def main():
    parser = argparse.ArgumentParser(
        description="Batch-extract ClapHanz XB archives with multiprocessing and validation"
    )
    parser.add_argument(
        "xbdata_dir",
        help="Root directory to scan for .xb/.xb0/.xb2/.xb3 files",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output directory (default: <xbdata-dir>_extracted)",
    )
    parser.add_argument(
        "-w", "--workers",
        type=int,
        default=None,
        help="Number of parallel worker processes (default: CPU count)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print each file as it is extracted",
    )
    args = parser.parse_args()

    xbdata_dir = os.path.abspath(args.xbdata_dir)
    if not os.path.isdir(xbdata_dir):
        sys.stderr.write(f"error: {xbdata_dir} is not a directory\n")
        return 1

    out_dir = os.path.abspath(args.output) if args.output else xbdata_dir + "_extracted"

    files = find_xb_files(xbdata_dir)
    if not files:
        print(f"No XB archives found under {xbdata_dir}")
        return 0

    print(f"Found {len(files)} XB archives under {xbdata_dir}")
    print(f"Extracting and validating to {out_dir}")

    ok_count = 0
    fail_count = 0
    t0 = time.time()

    # Map each archive path to its destination directory
    tasks = []
    for fpath in files:
        rel = os.path.relpath(fpath, xbdata_dir)
        # Preserve the complete archive suffix.  `.xb0`, `.xb2`, and `.xb3` are
        # localized variants with the same internal paths; collapsing all of them to
        # `<name>.xb.d` made parallel extraction overwrite languages nondeterministically.
        dest = os.path.join(out_dir, rel + ".d")
        tasks.append((fpath, dest))

    # Parallel execution using ProcessPoolExecutor
    max_workers = args.workers
    print(f"Starting ProcessPoolExecutor with {max_workers or 'default'} workers...")
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = [
            executor.submit(extract_one, fpath, dest, args.verbose)
            for fpath, dest in tasks
        ]

        for i, fut in enumerate(futures, 1):
            fpath, ok, err = fut.result()
            rel = os.path.relpath(fpath, xbdata_dir)
            if ok:
                ok_count += 1
                if not args.verbose:
                    print(f"  [{i}/{len(files)}] OK  {rel}")
            else:
                fail_count += 1
                print(f"  [{i}/{len(files)}] FAIL {rel}: {err}")

    elapsed = time.time() - t0
    print(f"\nDone: {ok_count} extracted, {fail_count} failed ({elapsed:.1f}s)")
    return 1 if fail_count else 0


if __name__ == "__main__":
    sys.exit(main())
