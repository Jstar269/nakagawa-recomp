# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for issue #151: the analyzer must not silently inherit
HST-only executable spans.

`analyze.exec_ranges()`/`analyze()` apply extra executable spans only when the
caller supplies them explicitly; the environment override (`HST_EXTRA_SPANS`)
is read solely by the CLI entry points, and only for the primary image.
"""

from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import analyze  # noqa: E402

HST_SPAN = (0x00303194, 0x00306E24)
HST_SPAN_TEXT = "0x00303194,0x00306e24"


def make_elf(path: Path, *, base_addr: int = 0x1000, words=(0x03E00008, 0x00000000)) -> None:
    """Fabricate a minimal ET_EXEC with one executable PT_LOAD segment.

    `words` are placed little-endian at `base_addr`; the entry point is the
    segment start. No section headers are emitted, so `Elf` reconstructs the
    executable section from the segment (`.text`), exactly like a stripped PRX.
    """
    payload_off = 52 + 32
    filesz = len(words) * 4
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<HHIIIIIHHHHHH", blob, 16,
                     2, 8, 1, base_addr, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base_addr, base_addr, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word & 0xFFFFFFFF)
    path.write_bytes(blob)


class AnalyzerSpanScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._saved_environ = os.environ.copy()
        os.environ.pop("HST_EXTRA_SPANS", None)
        os.environ.pop("GAME_BASE", None)
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-issue151-")
        self.root = Path(self.temp.name)
        self.elf = self.root / "module.elf"
        make_elf(self.elf)

    def tearDown(self) -> None:
        os.environ.clear()
        os.environ.update(self._saved_environ)
        self.temp.cleanup()

    # --- raw library usage ------------------------------------------------

    def test_raw_base_zero_elf_gets_no_title_specific_span(self) -> None:
        loaded = analyze.Elf(str(self.elf), base=0)
        self.assertEqual(analyze.exec_ranges(loaded), [(0x1000, 0x1008)])
        # The environment being unset must mean "no extra spans", never a default.
        starts, ranges = analyze.analyze(loaded)
        self.assertEqual(ranges, [(0x1000, 0x1008)])
        self.assertIn(0x1000, starts)

    def test_raw_analyze_cli_gets_no_hst_span_without_explicit_override(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "analyze.py"), str(self.elf), "--base=0", "--quiet"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("0x00303194", proc.stdout + proc.stderr)

    def test_analyze_cli_applies_span_only_when_explicitly_supplied(self) -> None:
        env = os.environ.copy()
        env.pop("HST_EXTRA_SPANS", None)
        env["HST_EXTRA_SPANS"] = HST_SPAN_TEXT
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "analyze.py"), str(self.elf), "--base=0", "--quiet"],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("3158420", proc.stdout + proc.stderr)
        self.assertIn("3173924", proc.stdout + proc.stderr)

    def test_explicit_extra_spans_are_applied_and_merged(self) -> None:
        loaded = analyze.Elf(str(self.elf), base=0)
        ranges = analyze.exec_ranges(loaded, extra_spans=[HST_SPAN])
        self.assertEqual(ranges, [(0x1000, 0x1008), HST_SPAN])
        starts, ranges_from_analyze = analyze.analyze(loaded, extra_spans=[HST_SPAN])
        self.assertEqual(ranges, ranges_from_analyze)
        self.assertIn(0x1000, starts)

    def test_explicit_extra_spans_fail_closed_on_a_rebased_image(self) -> None:
        # A rebased guest lives at different addresses; the explicit HST span must
        # never be applied to it, and the attempt must fail loudly.
        rebased = analyze.Elf(str(self.elf), base=0x32200000)
        with self.assertRaisesRegex(RuntimeError, "GAME_BASE != 0"):
            analyze.exec_ranges(rebased, extra_spans=[HST_SPAN])
        with self.assertRaisesRegex(RuntimeError, "GAME_BASE != 0"):
            analyze.analyze(rebased, extra_spans=[HST_SPAN])

    def test_environment_never_leaks_into_library_usage(self) -> None:
        # The regression that issue #151 is about: even with the override present in
        # the environment, calling exec_ranges()/analyze() directly must not inherit it.
        os.environ["HST_EXTRA_SPANS"] = HST_SPAN_TEXT
        loaded = analyze.Elf(str(self.elf), base=0)
        self.assertEqual(analyze.exec_ranges(loaded), [(0x1000, 0x1008)])
        _, ranges = analyze.analyze(loaded)
        self.assertEqual(ranges, [(0x1000, 0x1008)])

    # --- environment parsing ----------------------------------------------

    def test_analyzer_span_from_env_parsing(self) -> None:
        self.assertIsNone(analyze.analyzer_span_from_env({}))
        self.assertIsNone(analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": ""}))
        self.assertIsNone(analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": "  "}))
        self.assertEqual(
            analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": HST_SPAN_TEXT}),
            [HST_SPAN],
        )
        self.assertEqual(
            analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": "3158420, 3173924"}),
            [HST_SPAN],
        )
        with self.assertRaisesRegex(RuntimeError, "look like 'lo,hi'"):
            analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": "0x10"})
        with self.assertRaisesRegex(RuntimeError, "numeric"):
            analyze.analyzer_span_from_env({"HST_EXTRA_SPANS": "0x10,zzz"})

    # --- codegen integration ----------------------------------------------

    def test_codegen_primary_span_does_not_poison_extra_guest_modules(self) -> None:
        # The HST manager exports HST_EXTRA_SPANS across the make spawn. codegen must
        # apply it to the primary image only; a rebased extra module must never raise
        # (this used to crash the real -TitleManifest BuildFull path) and never gain
        # the span.
        extra = self.root / "extra.prx"
        make_elf(extra, base_addr=0x32200000, words=(0x03E00008, 0x00000000))
        out_c = self.root / "out.c"
        env = os.environ.copy()
        env.pop("HST_EXTRA_SPANS", None)
        env["HST_EXTRA_SPANS"] = HST_SPAN_TEXT
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"),
                str(self.elf), str(out_c),
                "--base=0", "--profile=none",
                f"--extra-elf={extra}@0x32200000",
                "--funcs-per-chunk=2000",
            ],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("RuntimeError", proc.stderr)
        self.assertTrue(out_c.exists())
        combined = (out_c.read_text(encoding="ascii")
                    if out_c.exists() else "") + proc.stderr
        # The extra module is rebased to its load address; the HST primary-image span
        # must not appear as a guest range for it.
        self.assertNotIn("0x32203000", combined)

    def test_codegen_without_override_runs_clean_on_generic_image(self) -> None:
        out_c = self.root / "out.c"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"),
                str(self.elf), str(out_c),
                "--base=0x08804000", "--profile=none",
                "--funcs-per-chunk=2000",
            ],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(out_c.exists())


if __name__ == "__main__":
    unittest.main()
