# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The committed sceMp3 tone fixture header is exactly what its generator writes.

fixtures/mp3_tone/mp3_tone.h feeds the sceMp3 HLE tests in src/rt/hle_thread_selftest.c.
It is generated, not hand-edited, and it must stay source-authored: frames built from the
MPEG-1 Layer III syntax by fixtures/mp3_tone/generate.py, with no encoder output or recorded
audio in the repository.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "fixtures" / "mp3_tone" / "generate.py"
HEADER = ROOT / "fixtures" / "mp3_tone" / "mp3_tone.h"


def _load_generator():
    spec = importlib.util.spec_from_file_location("mp3_tone_generate", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _array(text: str, name: str) -> bytes:
    match = re.search(rf"{name}\[(\d+)\] = \{{(.*?)\}};", text, re.S)
    assert match is not None, name
    values = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", match.group(2)))
    assert len(values) == int(match.group(1)), name
    return values


class Mp3ToneFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.gen = _load_generator()
        self.text = HEADER.read_text(encoding="utf-8")

    def test_header_is_fresh(self) -> None:
        self.assertEqual(self.text, self.gen.render(), "run python fixtures/mp3_tone/generate.py")
        self.assertEqual(self.gen.main(["--check"]), 0)

    def test_frames_are_mpeg1_layer3_at_the_declared_size(self) -> None:
        for name, mode in (("k_mp3_tone_stereo_frame", 0x00), ("k_mp3_tone_mono_frame", 0xC0)):
            frame = _array(self.text, name)
            self.assertEqual(len(frame), self.gen.FRAME_BYTES)
            # Sync, MPEG-1, Layer III, no CRC; 32 kbps at 44.1 kHz, no padding; channel mode.
            self.assertEqual(frame[:4], bytes([0xFF, 0xFB, 0x10, mode]), name)
            # main_data_begin is 0: no frame leans on another's bit reservoir.
            self.assertEqual(frame[4] << 1 | frame[5] >> 7, 0, name)

    def test_count1_codes_place_one_line_per_channel(self) -> None:
        bits = self.gen.count1_line(26).bits
        # Six all-zero quadruples (table B code 1111), then 0010 coded as 1101, then a + sign.
        self.assertEqual(bits, [1, 1, 1, 1] * 6 + [1, 1, 0, 1] + [0])
        self.assertEqual(len(self.gen.count1_line(52).bits), 13 * 4 + 4 + 1)

    def test_id3_prefix_is_an_empty_tag(self) -> None:
        self.assertEqual(_array(self.text, "k_mp3_tone_id3_prefix"), b"ID3\x03" + bytes(6))


if __name__ == "__main__":
    unittest.main()
