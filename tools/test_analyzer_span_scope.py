# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for analyzer executable-span ownership.

An extra executable span (a code range that lives outside the section table) is
*title configuration*. The analyzer therefore applies one only when a caller hands
it over explicitly; there is no built-in default, and ambient process state cannot
reach a library call. The environment seam is read at CLI entry points only, and
only for the primary image, so a rebased extra guest module analyzed in the same
process can never inherit the primary module's span.
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

# A wholly synthetic span standing in for "some other title's configuration". It is
# deliberately NOT any real title's address range: these tests prove isolation and
# ownership, which no real address is needed to demonstrate, and reusing one would
# spread a title-specific constant into generic test surfaces.
FOREIGN_SPAN = (0x00420000, 0x00420400)
FOREIGN_SPAN_TEXT = "0x00420000,0x00420400"
# A second, distinct synthetic span for the cases that need two values to disagree.
RIVAL_SPAN_TEXT = "0x00400000,0x00400100"
PRIMARY_BASE = 0x1000
REBASED_BASE = 0x32200000


def write_elf(path: Path, *, load_addr: int = PRIMARY_BASE, words=(0x03E00008, 0x00000000)) -> None:
    """Fabricate a minimal little-endian ET_EXEC with one executable PT_LOAD.

    `words` are placed at `load_addr` and the entry point is the segment start. No
    section headers are emitted, so the loader reconstructs the executable range from
    the segment exactly like a stripped PRX image would.
    """
    payload_off = 52 + 32
    filesz = len(words) * 4
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, load_addr, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, load_addr, load_addr, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word & 0xFFFFFFFF)
    path.write_bytes(blob)


class AnalyzerSpanScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._saved_environ = os.environ.copy()
        for name in ("TITLE_EXTRA_SPANS", "HST_EXTRA_SPANS", "GAME_BASE"):
            os.environ.pop(name, None)
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-span-scope-")
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self._restore_environ)
        self.root = Path(self.temp.name)
        self.elf = self.root / "primary.elf"
        write_elf(self.elf)

    def _restore_environ(self) -> None:
        os.environ.clear()
        os.environ.update(self._saved_environ)

    def _clean_env(self, **overrides: str) -> dict[str, str]:
        env = os.environ.copy()
        env.pop("TITLE_EXTRA_SPANS", None)
        env.pop("HST_EXTRA_SPANS", None)
        env.update(overrides)
        return env

    # --- no hidden default ------------------------------------------------

    def test_base_zero_image_gets_no_title_specific_span(self) -> None:
        loaded = analyze.Elf(str(self.elf), base=0)
        self.assertEqual(analyze.exec_ranges(loaded), [(PRIMARY_BASE, PRIMARY_BASE + 8)])
        starts, ranges = analyze.analyze(loaded)
        self.assertEqual(ranges, [(PRIMARY_BASE, PRIMARY_BASE + 8)])
        self.assertIn(PRIMARY_BASE, starts)

    def test_generic_analyzer_source_carries_no_title_constant(self) -> None:
        source = (TOOLS / "analyze.py").read_text(encoding="utf-8")
        self.assertNotIn("DEFAULT_HST_EXTRA_SPANS", source)

    def test_ambient_environment_cannot_reach_a_library_call(self) -> None:
        os.environ["TITLE_EXTRA_SPANS"] = FOREIGN_SPAN_TEXT
        loaded = analyze.Elf(str(self.elf), base=0)
        self.assertEqual(analyze.exec_ranges(loaded), [(PRIMARY_BASE, PRIMARY_BASE + 8)])
        _, ranges = analyze.analyze(loaded)
        self.assertEqual(ranges, [(PRIMARY_BASE, PRIMARY_BASE + 8)])
        # Legacy HST must also not reach library (generic analyzer ignores HST entirely)
        os.environ.pop("TITLE_EXTRA_SPANS", None)
        os.environ["HST_EXTRA_SPANS"] = FOREIGN_SPAN_TEXT
        self.assertEqual(analyze.exec_ranges(loaded), [(PRIMARY_BASE, PRIMARY_BASE + 8)])
        _, ranges = analyze.analyze(loaded)
        self.assertEqual(ranges, [(PRIMARY_BASE, PRIMARY_BASE + 8)])

    # --- explicit spans ---------------------------------------------------

    def test_explicit_span_is_applied_to_the_module_that_owns_it(self) -> None:
        loaded = analyze.Elf(str(self.elf), base=0)
        ranges = analyze.exec_ranges(loaded, extra_spans=[FOREIGN_SPAN])
        self.assertEqual(ranges, [(PRIMARY_BASE, PRIMARY_BASE + 8), FOREIGN_SPAN])
        _, from_analyze = analyze.analyze(loaded, extra_spans=[FOREIGN_SPAN])
        self.assertEqual(ranges, from_analyze)

    def test_two_modules_with_different_explicit_spans_stay_isolated(self) -> None:
        other = self.root / "other.elf"
        write_elf(other, load_addr=0x2000)
        first = analyze.exec_ranges(
            analyze.Elf(str(self.elf), base=0), extra_spans=[(0x00400000, 0x00400100)]
        )
        second = analyze.exec_ranges(
            analyze.Elf(str(other), base=0), extra_spans=[(0x00500000, 0x00500100)]
        )
        self.assertIn((0x00400000, 0x00400100), first)
        self.assertNotIn((0x00500000, 0x00500100), first)
        self.assertIn((0x00500000, 0x00500100), second)
        self.assertNotIn((0x00400000, 0x00400100), second)

    def test_explicit_span_fails_closed_on_a_rebased_image(self) -> None:
        rebased = analyze.Elf(str(self.elf), base=REBASED_BASE)
        with self.assertRaisesRegex(RuntimeError, "GAME_BASE != 0"):
            analyze.exec_ranges(rebased, extra_spans=[FOREIGN_SPAN])
        with self.assertRaisesRegex(RuntimeError, "GAME_BASE != 0"):
            analyze.analyze(rebased, extra_spans=[FOREIGN_SPAN])

    # --- span parsing -----------------------------------------------------

    def test_span_parsing_accepts_only_well_formed_ranges(self) -> None:
        self.assertIsNone(analyze.parse_extra_spans(None))
        self.assertIsNone(analyze.parse_extra_spans(""))
        self.assertIsNone(analyze.parse_extra_spans("   "))
        self.assertEqual(analyze.parse_extra_spans(FOREIGN_SPAN_TEXT), [FOREIGN_SPAN])
        # Decimal and whitespace-padded forms name the same range.
        decimal = f" {FOREIGN_SPAN[0]} , {FOREIGN_SPAN[1]} "
        self.assertEqual(analyze.parse_extra_spans(decimal), [FOREIGN_SPAN])
        for malformed, pattern in (
            ("0x10", "look like 'lo,hi'"),
            ("0x10,0x20,0x30", "look like 'lo,hi'"),
            ("0x10,zzz", "numeric"),
            ("-1,0x20", "negative"),
            ("0x20,0x10", "hi > lo"),
            ("0x10,0x10", "hi > lo"),
            ("0,0x100000001", "32-bit"),
        ):
            with self.subTest(value=malformed):
                with self.assertRaisesRegex(RuntimeError, pattern):
                    analyze.parse_extra_spans(malformed)

    def test_environment_and_option_must_agree(self) -> None:
        env = {"TITLE_EXTRA_SPANS": FOREIGN_SPAN_TEXT}
        self.assertEqual(analyze.analyzer_span_from_env(env), [FOREIGN_SPAN])
        self.assertIsNone(analyze.analyzer_span_from_env({}))
        # An explicit option wins over an equal environment value...
        self.assertEqual(
            analyze.resolve_extra_spans(FOREIGN_SPAN_TEXT, env), [FOREIGN_SPAN]
        )
        # ...and a disagreement fails closed instead of silently picking one.
        with self.assertRaisesRegex(RuntimeError, "conflicts with"):
            analyze.resolve_extra_spans(RIVAL_SPAN_TEXT, env)

    # --- generic TITLE is sole authority: HST legacy does not leak --------------

    def test_title_undefined_plus_stale_hst_yields_none_A3(self) -> None:
        # A3 mandatory load-bearing: TITLE undefined + stale HST -> none
        env = {"HST_EXTRA_SPANS": FOREIGN_SPAN_TEXT}
        self.assertIsNone(analyze.analyzer_span_from_env(env))

    def test_title_empty_plus_stale_hst_yields_none_A4(self) -> None:
        env = {"TITLE_EXTRA_SPANS": "", "HST_EXTRA_SPANS": FOREIGN_SPAN_TEXT}
        self.assertIsNone(analyze.analyzer_span_from_env(env))

    def test_title_generic_value_yields_span_A2(self) -> None:
        env = {"TITLE_EXTRA_SPANS": FOREIGN_SPAN_TEXT}
        self.assertEqual(analyze.analyzer_span_from_env(env), [FOREIGN_SPAN])

    def test_title_undefined_empty_yields_none_A1(self) -> None:
        self.assertIsNone(analyze.analyzer_span_from_env({}))
        self.assertIsNone(analyze.analyzer_span_from_env({"TITLE_EXTRA_SPANS": ""}))

    def test_malformed_title_fails_closed_A5(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "look like"):
            analyze.analyzer_span_from_env({"TITLE_EXTRA_SPANS": "bad"})
        with self.assertRaisesRegex(RuntimeError, "numeric"):
            analyze.analyzer_span_from_env({"TITLE_EXTRA_SPANS": "0x10,zzz"})

    def test_unsupported_multi_span_fails_closed_A6(self) -> None:
        # analyzer seam accepts at most one span; two comma-separated pairs is malformed grammar
        with self.assertRaisesRegex(RuntimeError, "look like"):
            analyze.parse_extra_spans("0x1000,0x2000,0x3000,0x4000")

    def test_nonzero_base_plus_extra_span_fails_closed_A7(self) -> None:
        loaded = analyze.Elf(str(self.elf), base=REBASED_BASE)
        with self.assertRaisesRegex(RuntimeError, "GAME_BASE != 0"):
            analyze.exec_ranges(loaded, extra_spans=[FOREIGN_SPAN])

    def test_hst_legacy_translation_outside_analyzer_A8(self) -> None:
        # A8: HST translation must happen before analyzer, not inside it.
        # Simulate HST boundary: HST value translated to TITLE before calling analyzer.
        hst_env = {"HST_EXTRA_SPANS": FOREIGN_SPAN_TEXT}
        # Generic analyzer sees HST and returns None (no leak)
        self.assertIsNone(analyze.analyzer_span_from_env(hst_env))
        # HST boundary translates to TITLE
        translated = {"TITLE_EXTRA_SPANS": hst_env["HST_EXTRA_SPANS"]}
        self.assertEqual(analyze.analyzer_span_from_env(translated), [FOREIGN_SPAN])
        # Analyzer source must not consult HST as implicit fallback (documentation mention allowed)
        source = (TOOLS / "analyze.py").read_text(encoding="utf-8")
        self.assertNotIn("LEGACY_EXTRA_SPAN_ENV", source)
        # The generic helper must not read HST env var code-wise
        self.assertNotIn('LEGACY_EXTRA_SPAN_ENV in environ', source)
        self.assertNotIn('environ.get(LEGACY', source)

    # --- CLI seams --------------------------------------------------------

    def test_analyze_cli_runs_clean_without_an_explicit_span(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "analyze.py"), str(self.elf), "--base=0", "--quiet"],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_codegen_cli_scans_only_the_ranges_it_was_given(self) -> None:
        # codegen reports the ranges it actually scanned, which is the externally
        # visible statement of what the analyzer decided. Without a span it must be
        # the segment alone; with one, exactly the segment plus that span.
        out_c = self.root / "scan.c"
        argv = [
            sys.executable, str(TOOLS / "codegen.py"), str(self.elf), str(out_c),
            "--base=0", "--profile=none", "--funcs-per-chunk=2000",
        ]
        bare = subprocess.run(
            argv, cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(bare.returncode, 0, bare.stdout + bare.stderr)
        self.assertIn(
            f"SCANNING RANGES: [({PRIMARY_BASE}, {PRIMARY_BASE + 8})]",
            bare.stdout + bare.stderr,
        )
        spanned = subprocess.run(
            argv + [f"--extra-span={FOREIGN_SPAN_TEXT}"],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(spanned.returncode, 0, spanned.stdout + spanned.stderr)
        self.assertIn(
            f"SCANNING RANGES: [({PRIMARY_BASE}, {PRIMARY_BASE + 8}), "
            f"({FOREIGN_SPAN[0]}, {FOREIGN_SPAN[1]})]",
            spanned.stdout + spanned.stderr,
        )

    def test_codegen_primary_span_does_not_reach_a_rebased_extra_module(self) -> None:
        # The manager exports the span across the make spawn. codegen must apply it to
        # the primary image only: a rebased extra module must neither gain the span nor
        # abort the build, which is what an ambient, per-ELF environment read did.
        extra = self.root / "extra.prx"
        write_elf(extra, load_addr=REBASED_BASE)
        out_c = self.root / "out.c"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"),
                str(self.elf), str(out_c),
                "--base=0", "--profile=none",
                f"--extra-elf={extra}@0x{REBASED_BASE:08x}",
                "--funcs-per-chunk=2000",
            ],
            cwd=ROOT,
            env=self._clean_env(TITLE_EXTRA_SPANS=FOREIGN_SPAN_TEXT),
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("RuntimeError", proc.stderr)
        self.assertTrue(out_c.is_file())

    def test_codegen_extra_span_option_matches_the_environment_seam(self) -> None:
        # The Make path passes --extra-span rather than a recipe environment prefix.
        # Both routes must produce byte-identical generated code.
        outputs = []
        for label, argv, env in (
            ("option", [f"--extra-span={FOREIGN_SPAN_TEXT}"], self._clean_env()),
            ("environment", [], self._clean_env(TITLE_EXTRA_SPANS=FOREIGN_SPAN_TEXT)),
        ):
            # Same output *name* in different directories: the generated translation
            # unit embeds its own basename, so differing names would mask a real diff.
            out_dir = self.root / label
            out_dir.mkdir()
            out_c = out_dir / "out.c"
            proc = subprocess.run(
                [
                    sys.executable, str(TOOLS / "codegen.py"),
                    str(self.elf), str(out_c),
                    "--base=0", "--profile=none", "--funcs-per-chunk=2000", *argv,
                ],
                cwd=ROOT, env=env, capture_output=True, text=True, check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            outputs.append(out_c.read_bytes())
        self.assertEqual(outputs[0], outputs[1])

    def test_codegen_rejects_a_conflicting_ambient_span(self) -> None:
        out_c = self.root / "conflict.c"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"),
                str(self.elf), str(out_c),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000",
                f"--extra-span={RIVAL_SPAN_TEXT}",
            ],
            cwd=ROOT,
            env=self._clean_env(TITLE_EXTRA_SPANS=FOREIGN_SPAN_TEXT),
            capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("conflicts with", proc.stderr)

    def test_paths_with_spaces_and_shell_metacharacters_are_not_interpreted(self) -> None:
        # codegen receives paths as argv entries, never through a shell. A directory
        # whose name contains spaces and shell metacharacters must be treated as a
        # literal path, and no part of it may be executed.
        hostile = self.root / "a dir; echo pwned & $(id) `id`"
        hostile.mkdir()
        elf = hostile / "in put.elf"
        write_elf(elf)
        out_c = hostile / "out put.c"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"),
                str(elf), str(out_c),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        # The output landed at the exact literal path, and the tool reported that same
        # literal path back: nothing was word-split, expanded, or executed.
        self.assertTrue(out_c.is_file())
        self.assertIn(str(out_c), proc.stdout)
        self.assertFalse(any(self.root.glob("uid=*")))

    # --- deterministic byte-budget chunking ------------------------------

    @staticmethod
    def _run_codegen(elf: Path, out_c: Path, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"), str(elf), str(out_c),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000", *extra,
            ],
            cwd=ROOT, env=None, capture_output=True, text=True, check=False,
        )

    @staticmethod
    def _chunk_names(root: Path, stem: str) -> list[list[str]]:
        import re

        chunks = sorted(root.glob(f"{stem}_[0-9]*.c"))
        names = []
        for chunk in chunks:
            names.append(re.findall(r"void (f_[0-9a-f]{8})", chunk.read_text(encoding="ascii")))
        return names

    def test_byte_budget_splits_contiguously_in_order_without_duplicates(self) -> None:
        # Six uniform reachable 8-byte functions. The byte budget must close a chunk
        # only on a function boundary, preserve emission order, and never move or
        # duplicate a function across chunks.
        words: list[int] = []
        for i in range(6):
            words.append(0x0C000000 | (((0x1000 + (i + 1) * 8) >> 2) & 0x3FFFFFF) if i < 5 else 0x03E00008)
            words.append(0x00000000)
        elf = self.root / "budget.elf"
        write_elf(elf, words=words)

        legacy = self.root / "budget_legacy.c"
        proc = self._run_codegen(elf, legacy)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        legacy_text = (self.root / "budget_legacy_0.c").read_text(encoding="ascii")
        per_func = (len(legacy_text) - legacy_text.index("void f_")) // 6
        self.assertEqual(
            self._chunk_names(self.root, "budget_legacy"),
            [[f"f_{0x1000 + i * 8:08x}" for i in range(6)]],
        )

        # A budget of three function emissions plus a little slack must yield 2
        # contiguous chunks of exactly 3 functions each.
        budget = per_func * 3 + 64
        split = self.root / "budget_split.c"
        proc = self._run_codegen(elf, split, f"--target-chunk-bytes={budget}")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            self._chunk_names(self.root, "budget_split"),
            [
                [f"f_{0x1000 + i * 8:08x}" for i in range(3)],
                [f"f_{0x1000 + (i + 3) * 8:08x}" for i in range(3)],
            ],
        )

    def test_byte_budget_still_respects_the_function_cap(self) -> None:
        # A huge budget must not turn the cap off: the legacy count bound still
        # applies, so 6 functions with a cap of 2 become 3 chunks of 2.
        words: list[int] = []
        for i in range(6):
            words.append(0x0C000000 | (((0x1000 + (i + 1) * 8) >> 2) & 0x3FFFFFF) if i < 5 else 0x03E00008)
            words.append(0x00000000)
        elf = self.root / "cap.elf"
        write_elf(elf, words=words)

        out_c = self.root / "cap_split.c"
        proc = self._run_codegen(elf, out_c, "--funcs-per-chunk=2", "--target-chunk-bytes=1073741824")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            self._chunk_names(self.root, "cap_split"),
            [
                [f"f_{0x1000 + i * 8:08x}" for i in range(2)],
                [f"f_{0x1000 + (i + 2) * 8:08x}" for i in range(2)],
                [f"f_{0x1000 + (i + 4) * 8:08x}" for i in range(2)],
            ],
        )

    def test_absent_byte_budget_keeps_the_legacy_count_partition(self) -> None:
        # Without the flag the emitted partition must stay purely count-based, so a
        # cap of 2 still yields 3 chunks of 2 -- the byte budget must not leak into
        # the default path.
        words: list[int] = []
        for i in range(6):
            words.append(0x0C000000 | (((0x1000 + (i + 1) * 8) >> 2) & 0x3FFFFFF) if i < 5 else 0x03E00008)
            words.append(0x00000000)
        elf = self.root / "legacy.elf"
        write_elf(elf, words=words)

        out_c = self.root / "legacy_split.c"
        proc = self._run_codegen(elf, out_c, "--funcs-per-chunk=2")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            self._chunk_names(self.root, "legacy_split"),
            [
                [f"f_{0x1000 + i * 8:08x}" for i in range(2)],
                [f"f_{0x1000 + (i + 2) * 8:08x}" for i in range(2)],
                [f"f_{0x1000 + (i + 4) * 8:08x}" for i in range(2)],
            ],
        )


class MakefileSpanBindingTests(unittest.TestCase):
    """The direct-Make path must bind a span explicitly, not rely on a default."""

    def setUp(self) -> None:
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_span_is_passed_as_an_argument_not_a_shell_environment_prefix(self) -> None:
        # `VAR=value cmd` needs a POSIX shell; Make falls back to cmd.exe on Windows
        # when sh is absent. The span therefore travels as an argv entry via
        # EFFECTIVE_EXTRA_SPANS derived only from TITLE_EXTRA_SPANS (generic contract).
        self.assertIn("--extra-span=$(strip $(EFFECTIVE_EXTRA_SPANS))", self.makefile)
        self.assertIn("$(EXTRA_SPAN_ARG)", self.makefile)
        self.assertNotIn("HST_EXTRA_SPANS=$(HST_EXTRA_SPANS) $(PYTHON)", self.makefile)

    def test_span_is_empty_for_a_generic_title(self) -> None:
        # Outside the GAME_NAME=hst block the binding must default to empty, so a
        # generic build passes no span at all. Generic contract uses TITLE_EXTRA_SPANS.
        generic = self.makefile.split("ifeq ($(GAME_NAME),hst)", 1)[1].split("endif", 1)[1]
        self.assertIn("TITLE_EXTRA_SPANS ?=\n", generic)
        # Legacy HST variable also defaults to empty but is ignored for generic titles.
        self.assertIn("HST_EXTRA_SPANS ?=\n", generic)

    def test_span_participates_in_the_codegen_profile_hash(self) -> None:
        # Changing the span must invalidate previously generated code.
        self.assertIn('--entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)"', self.makefile)


if __name__ == "__main__":
    unittest.main()
