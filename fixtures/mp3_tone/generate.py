# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the source-authored MP3 tone fixture for the sceMp3 HLE tests.

No encoder and no recorded audio are involved. Each frame is written directly
from the MPEG-1 Layer III syntax (ISO/IEC 11172-3): the side information
declares one long-block granule per channel whose only non-zero spectral line
is coded in the count1 region with Huffman table B, so after the decoder's
hybrid filterbank each channel is a steady tone at ``line * sample_rate / 1152``
Hz (the same coefficient repeated in every granule).

Stereo frames carry line 26 on the left (995.3 Hz at 44.1 kHz, peak about 4096)
and line 52 on the right (1990.6 Hz, peak about 2048), so a test can tell the
channels apart. Mono frames carry line 26 only. Every frame of one layout is
byte-identical (``main_data_begin`` is 0, so nothing leans on the bit
reservoir); the tests assemble a stream from an optional prefix and N copies.

The output is ``mp3_tone.h`` beside this script::

    python fixtures/mp3_tone/generate.py           # rewrite the header
    python fixtures/mp3_tone/generate.py --check   # exit 1 if it is stale
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "mp3_tone.h"

SAMPLE_RATE = 44100
BITRATE_KBPS = 32
# 1152 * kbps * 125 / rate, without the padding slot (every frame unpadded).
FRAME_BYTES = 1152 * BITRATE_KBPS * 125 // SAMPLE_RATE
LEFT_LINE = 26
RIGHT_LINE = 52
LEFT_GLOBAL_GAIN = 198
RIGHT_GLOBAL_GAIN = 194
MONO_GLOBAL_GAIN = 198
# A zero-length ID3v2.3 tag: ten bytes of junk ahead of the first sync word.
ID3_PREFIX = b"ID3\x03\x00\x00\x00\x00\x00\x00"


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def put(self, value: int, width: int) -> None:
        if value < 0 or value >= (1 << width):
            raise ValueError(f"{value} does not fit in {width} bits")
        for shift in range(width - 1, -1, -1):
            self.bits.append((value >> shift) & 1)

    def extend(self, other: "BitWriter") -> None:
        self.bits.extend(other.bits)

    def tobytes(self, size: int) -> bytes:
        if len(self.bits) > size * 8:
            raise ValueError(f"{len(self.bits)} bits overflow {size} bytes")
        padded = self.bits + [0] * (size * 8 - len(self.bits))
        return bytes(
            int("".join(map(str, padded[i : i + 8])), 2) for i in range(0, len(padded), 8)
        )


def count1_line(line: int) -> BitWriter:
    """Huffman table B codes for quadruples up to the one holding ``line``.

    Table B is the fixed 4-bit code ``15 - vwxy``; each non-zero value is
    followed by its sign bit (0 = positive). The region ends where
    ``part2_3_length`` says, so nothing after the last quadruple is coded.
    """
    bw = BitWriter()
    for _ in range(line // 4):
        bw.put(0b1111, 4)
    vwxy = 1 << (3 - line % 4)
    bw.put(15 - vwxy, 4)
    bw.put(0, 1)
    return bw


def granule_side_info(bw: BitWriter, part2_3_length: int, global_gain: int) -> None:
    bw.put(part2_3_length, 12)
    bw.put(0, 9)  # big_values: everything is in the count1 region
    bw.put(global_gain, 8)
    bw.put(0, 4)  # scalefac_compress 0: slen1 = slen2 = 0, no scalefactor bits
    bw.put(0, 1)  # window_switching_flag: long blocks
    for _ in range(3):
        bw.put(0, 5)  # table_select (unused with big_values 0)
    bw.put(0, 4)  # region0_count
    bw.put(0, 3)  # region1_count
    bw.put(0, 1)  # preflag
    bw.put(0, 1)  # scalefac_scale
    bw.put(1, 1)  # count1table_select: table B


def header(mono: bool) -> bytes:
    rate_index = {44100: 0, 48000: 1, 32000: 2}[SAMPLE_RATE]
    bitrate_index = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320].index(
        BITRATE_KBPS
    )
    # sync, MPEG-1, Layer III, no CRC; no padding, no private bit; mode; no emphasis.
    return bytes([0xFF, 0xFB, (bitrate_index << 4) | (rate_index << 2), 0xC0 if mono else 0x00])


def frame(mono: bool) -> bytes:
    channels = [(LEFT_LINE, MONO_GLOBAL_GAIN)] if mono else [
        (LEFT_LINE, LEFT_GLOBAL_GAIN),
        (RIGHT_LINE, RIGHT_GLOBAL_GAIN),
    ]
    side = BitWriter()
    side.put(0, 9)  # main_data_begin: no bit reservoir
    side.put(0, 5 if mono else 3)  # private_bits
    for _ in channels:
        side.put(0, 4)  # scfsi: scalefactors (none) are sent per granule
    main = BitWriter()
    for _granule in range(2):
        for line, gain in channels:
            codes = count1_line(line)
            granule_side_info(side, len(codes.bits), gain)
            main.extend(codes)
    side_bytes = 17 if mono else 32
    hdr = header(mono)
    body = side.tobytes(side_bytes) + main.tobytes(FRAME_BYTES - len(hdr) - side_bytes)
    out = hdr + body
    assert len(out) == FRAME_BYTES
    return out


def c_array(name: str, data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 12):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 12]) + ",")
    return f"static const unsigned char {name}[{len(data)}] = {{\n" + "\n".join(rows) + "\n};\n"


def render() -> str:
    parts = [
        "// SPDX-License-Identifier: GPL-3.0-or-later\n",
        "// Copyright (C) 2026 the Nakagawa Recomp authors\n",
        "\n",
        "/* GENERATED by fixtures/mp3_tone/generate.py -- do not edit. Source-authored\n",
        " * MPEG-1 Layer III tone frames (no encoder, no recorded audio); see the script\n",
        " * for how each frame is built. */\n",
        "\n",
        "#ifndef SR_FIXTURE_MP3_TONE_H\n",
        "#define SR_FIXTURE_MP3_TONE_H\n",
        "\n",
        f"#define MP3_TONE_SAMPLE_RATE {SAMPLE_RATE}\n",
        f"#define MP3_TONE_BITRATE_KBPS {BITRATE_KBPS}\n",
        f"#define MP3_TONE_FRAME_BYTES {FRAME_BYTES}\n",
        "#define MP3_TONE_FRAME_SAMPLES 1152\n",
        f"#define MP3_TONE_LEFT_LINE {LEFT_LINE}\n",
        f"#define MP3_TONE_RIGHT_LINE {RIGHT_LINE}\n",
        "\n",
        c_array("k_mp3_tone_id3_prefix", ID3_PREFIX),
        "\n",
        c_array("k_mp3_tone_stereo_frame", frame(mono=False)),
        "\n",
        c_array("k_mp3_tone_mono_frame", frame(mono=True)),
        "\n",
        "#endif /* SR_FIXTURE_MP3_TONE_H */\n",
    ]
    return "".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if mp3_tone.h is stale")
    args = parser.parse_args(argv)
    text = render()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT.name} is stale; run fixtures/mp3_tone/generate.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
