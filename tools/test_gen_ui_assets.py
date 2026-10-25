#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""``src/ui/nk_ui_assets.c`` is the exact, current embedding of ``third_party/kenney/``."""

from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_ui_assets  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class GenUiAssetsTests(unittest.TestCase):
    def test_committed_file_is_current(self) -> None:
        committed = (ROOT / gen_ui_assets.OUTPUT).read_bytes()
        self.assertEqual(
            committed,
            gen_ui_assets.render(gen_ui_assets.collect()).encode("utf-8"),
            "src/ui/nk_ui_assets.c is stale; run python tools/regen_all.py",
        )

    def test_every_vendored_asset_is_embedded_byte_for_byte(self) -> None:
        text = (ROOT / gen_ui_assets.OUTPUT).read_text(encoding="utf-8")
        assets = gen_ui_assets.collect()
        self.assertGreaterEqual(len(assets), 35)
        for name, data in assets:
            symbol = gen_ui_assets._symbol(name)
            match = re.search(
                rf"static const unsigned char {symbol}\[(\d+)\] = \{{\n(.*?)\n\}};", text, re.S
            )
            self.assertIsNotNone(match, name)
            assert match is not None
            self.assertEqual(int(match.group(1)), len(data), name)
            embedded = bytes(int(token, 16) for token in re.findall(r"0x([0-9A-F]{2})", match.group(2)))
            self.assertEqual(embedded, data, name)
            self.assertIn(f'{{"{name}", {symbol}, {len(data)}u}}', text)

    def test_names_are_pack_relative_and_sorted(self) -> None:
        names = [name for name, _ in gen_ui_assets.collect()]
        prompts = [name for name in names if name.startswith("input-prompts/")]
        sounds = [name for name in names if name.startswith("interface-sounds/")]
        self.assertEqual(names, prompts + sounds)
        self.assertEqual(prompts, sorted(prompts))
        self.assertEqual(sounds, sorted(sounds))
        self.assertTrue(all(name.endswith(".png") for name in prompts))
        self.assertTrue(all(name.endswith(".wav") for name in sounds))

    def test_oversized_or_missing_assets_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prompts = root / "third_party" / "kenney" / "input-prompts"
            sounds = root / "third_party" / "kenney" / "interface-sounds"
            prompts.mkdir(parents=True)
            with self.assertRaises(SystemExit):
                gen_ui_assets.collect(root)
            sounds.mkdir(parents=True)
            (prompts / "a.png").write_bytes(b"\x89PNG")
            (sounds / "a.wav").write_bytes(b"x" * (gen_ui_assets.MAX_ASSET_BYTES + 1))
            with self.assertRaises(SystemExit):
                gen_ui_assets.collect(root)
            (sounds / "a.wav").write_bytes(b"RIFF")
            self.assertEqual(
                [name for name, _ in gen_ui_assets.collect(root)],
                ["input-prompts/a.png", "interface-sounds/a.wav"],
            )

    def test_check_mode_reports_current(self) -> None:
        self.assertEqual(gen_ui_assets.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main()
