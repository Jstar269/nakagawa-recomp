# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Assemble player-captured P6 frames into small deterministic looping GIFs.

The frame index is the runtime VCOUNT from SR_FBSNAP_WINDOWS.  The encoder uses
one fixed 3-3-2 global palette and GIF89a LZW, with no timestamps or external
packages, so the same PPM inputs produce byte-identical animations.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parent.parent
FRAME_NAME = re.compile(r"frame_v([0-9]+)\.ppm\Z")
WIDTH = 480
HEIGHT = 272
MAX_GIF_BYTES = 1_000_000
DEFAULT_DELAY_CS = 10
MIN_CODE_SIZE = 8
CLEAR_CODE = 1 << MIN_CODE_SIZE
END_CODE = CLEAR_CODE + 1


def _read_token(data: bytes, offset: int) -> tuple[bytes, int]:
    whitespace = b" \t\r\n\v\f"
    while offset < len(data):
        if data[offset] in whitespace:
            offset += 1
            continue
        if data[offset] == ord("#"):
            while offset < len(data) and data[offset] not in b"\r\n":
                offset += 1
            continue
        break

    start = offset
    while offset < len(data) and data[offset] not in whitespace + b"#":
        offset += 1
    if start == offset or offset >= len(data):
        raise ValueError("truncated PPM header")
    token = data[start:offset]

    if data[offset] == ord("#"):
        while offset < len(data) and data[offset] not in b"\r\n":
            offset += 1
        if offset < len(data) and data[offset] == ord("\r"):
            offset += 1
            if offset < len(data) and data[offset] == ord("\n"):
                offset += 1
        elif offset < len(data):
            offset += 1
    else:
        delimiter = data[offset]
        offset += 1
        if delimiter == ord("\r") and offset < len(data) and data[offset] == ord("\n"):
            offset += 1
    return token, offset


def read_ppm(path: Path) -> bytes:
    data = path.read_bytes()
    offset = 0
    tokens = []
    for _ in range(4):
        token, offset = _read_token(data, offset)
        tokens.append(token)
    if tokens[0] != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM")
    try:
        width, height, maximum = (int(token) for token in tokens[1:])
    except ValueError as exc:
        raise ValueError(f"{path}: invalid PPM dimensions or maximum") from exc
    if (width, height, maximum) != (WIDTH, HEIGHT, 255):
        raise ValueError(f"{path}: expected {WIDTH}x{HEIGHT} P6 with maxval 255")
    expected = WIDTH * HEIGHT * 3
    pixels = data[offset:]
    if len(pixels) != expected:
        raise ValueError(f"{path}: expected {expected} RGB bytes, found {len(pixels)}")
    return pixels


def _frames(directory: Path) -> list[tuple[int, Path]]:
    if not directory.is_dir():
        raise ValueError(f"frame directory is missing: {directory}")
    frames = []
    for path in directory.iterdir():
        match = FRAME_NAME.fullmatch(path.name)
        if match and path.is_file():
            frames.append((int(match.group(1)), path))
    frames.sort(key=lambda item: item[0])
    if len(frames) < 2:
        raise ValueError(f"{directory}: need at least two frame_v*.ppm snapshots")
    return frames


def _palette_332() -> bytes:
    palette = bytearray()
    for index in range(256):
        red = ((index >> 5) & 7) * 255 // 7
        green = ((index >> 2) & 7) * 255 // 7
        blue = (index & 3) * 255 // 3
        palette.extend((red, green, blue))
    return bytes(palette)


def _indices_332(rgb: bytes) -> bytes:
    indices = bytearray(WIDTH * HEIGHT)
    for source in range(0, len(rgb), 3):
        target = source // 3
        indices[target] = (
            ((rgb[source] >> 5) << 5)
            | ((rgb[source + 1] >> 5) << 2)
            | (rgb[source + 2] >> 6)
        )
    return bytes(indices)


def _lzw_encode(indices: bytes) -> bytes:
    """Encode one indexed GIF image data stream with 8-bit minimum codes."""
    code_width = MIN_CODE_SIZE + 1
    next_code = END_CODE + 1
    dictionary: dict[tuple[int, int], int] = {}
    packed = bytearray()
    bit_buffer = 0
    bit_count = 0

    def emit(code: int) -> None:
        nonlocal bit_buffer, bit_count
        bit_buffer |= code << bit_count
        bit_count += code_width
        while bit_count >= 8:
            packed.append(bit_buffer & 0xFF)
            bit_buffer >>= 8
            bit_count -= 8

    emit(CLEAR_CODE)
    if indices:
        prefix = indices[0]
        for symbol in indices[1:]:
            key = (prefix, symbol)
            existing = dictionary.get(key)
            if existing is not None:
                prefix = existing
                continue

            emit(prefix)
            if next_code < 4096:
                dictionary[key] = next_code
                next_code += 1
                # The decoder learns each new entry one emitted code later than
                # the encoder. Keep the current width at the exact power-of-two
                # boundary; grow it after the next entry so both sides switch
                # before the same next code is read/written.
                if next_code > (1 << code_width) and code_width < 12:
                    code_width += 1
            else:
                emit(CLEAR_CODE)
                dictionary.clear()
                code_width = MIN_CODE_SIZE + 1
                next_code = END_CODE + 1
            prefix = symbol
        emit(prefix)
    emit(END_CODE)
    if bit_count:
        packed.append(bit_buffer & 0xFF)
    return bytes(packed)


def _append_subblocks(out: bytearray, payload: bytes) -> None:
    for offset in range(0, len(payload), 255):
        block = payload[offset:offset + 255]
        out.append(len(block))
        out.extend(block)
    out.append(0)


def encode_gif(frames: list[bytes], delay_cs: int) -> bytes:
    if not frames:
        raise ValueError("cannot encode an empty frame sequence")
    if delay_cs < 1 or delay_cs > 65535:
        raise ValueError("frame delay must be between 1 and 65535 centiseconds")

    output = bytearray(b"GIF89a")
    output.extend(struct.pack("<HHBBB", WIDTH, HEIGHT, 0xF7, 0, 0))
    output.extend(_palette_332())
    output.extend(b"!\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00")

    for rgb in frames:
        output.extend(b"!\xF9\x04\x04")
        output.extend(struct.pack("<H", delay_cs))
        output.extend(b"\x00\x00")
        output.append(0x2C)
        output.extend(struct.pack("<HHHHB", 0, 0, WIDTH, HEIGHT, 0))
        output.append(MIN_CODE_SIZE)
        _append_subblocks(output, _lzw_encode(_indices_332(rgb)))

    output.append(0x3B)
    if len(output) >= MAX_GIF_BYTES:
        raise ValueError(f"encoded animation must be smaller than {MAX_GIF_BYTES} bytes")
    return bytes(output)


def assemble(directory: Path, destination: Path, delay_cs: int) -> tuple[int, int]:
    sources = _frames(directory)
    rgb_frames = [read_ppm(path) for _vcount, path in sources]
    encoded = encode_gif(rgb_frames, delay_cs)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encoded)
    return len(sources), len(encoded)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scene-dir", type=Path,
        default=ROOT / "build" / "demos" / "packages" / "TEST00007",
        help="directory containing TEST00007 frame_v*.ppm player snapshots",
    )
    parser.add_argument(
        "--breakout-dir", type=Path,
        default=ROOT / "build" / "demos" / "packages" / "TEST00008",
        help="directory containing TEST00008 frame_v*.ppm player snapshots",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=ROOT / "docs" / "images",
        help="destination directory for the two animated GIFs",
    )
    parser.add_argument(
        "--delay-cs", type=int, default=10,
        help="per-frame delay in centiseconds (default: 10, or 10 fps)",
    )
    args = parser.parse_args()

    outputs = (
        ("3D showcase", args.scene_dir, args.output_dir / "showcase-ge-scene.gif"),
        ("Breakout showcase", args.breakout_dir, args.output_dir / "showcase-breakout.gif"),
    )
    try:
        for label, source, destination in outputs:
            frame_count, size = assemble(source, destination, args.delay_cs)
            print(f"{label}: {frame_count} frames, {size} bytes -> {destination}")
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
