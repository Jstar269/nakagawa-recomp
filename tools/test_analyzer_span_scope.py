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

import io
import json
import os
import random
import shutil
from contextlib import redirect_stderr
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import analyze  # noqa: E402
import codegen  # noqa: E402
from nk_core import package_cache  # noqa: E402

# A wholly synthetic span standing in for "some other title's configuration". It is
# deliberately NOT any real title's address range: these tests prove isolation and
# ownership, which no real address is needed to demonstrate, and reusing one would
# spread a title-specific constant into generic test surfaces.
FOREIGN_SPAN = (0x00420000, 0x00420400)
FOREIGN_SPAN_TEXT = "0x00420000,0x00420400"
# A second, distinct synthetic span for the cases that need two values to disagree.
RIVAL_SPAN_TEXT = "0x00400000,0x00400100"
PRIMARY_BASE = 0x1000
REBASED_BASE = 0x09ebfc00
CC = shutil.which("gcc")


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


def write_direct_call_chain_elf(path: Path, function_count: int) -> None:
    """Fabricate a source-owned ELF whose functions form one direct-call chain."""
    base = PRIMARY_BASE
    words = []
    for index in range(function_count):
        target = base + ((index + 1) % function_count) * 20
        words.extend((
            0x27BDFFF0,  # addiu $sp, $sp, -16
            0x0C000000 | ((target >> 2) & 0x03FFFFFF),  # jal next function
            0x00000000,  # delay slot
            0x03E00008,  # jr $ra
            0x00000000,  # delay slot
        ))
    payload = struct.pack(f"<{len(words)}I", *words)
    payload_off = 52 + 32
    blob = bytearray(payload_off + len(payload))
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, len(payload), len(payload), 5, 4,
    )
    blob[payload_off:] = payload
    path.write_bytes(blob)


def write_elf_with_called_code_outside_text(path: Path) -> int:
    """Fabricate executable bytes beyond a deliberately narrow named .text section."""
    base = 0x1000
    target = base + 0x20
    words = [
        0x0C000000 | ((target >> 2) & 0x03FFFFFF),  # jal target
        0x00000000,                                # delay slot
        0x03E00008,                                # jr $ra
        0x00000000,                                # delay slot
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x03E00008,                                # hidden leaf: jr $ra
        0x00000000,                                # delay slot
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
    ]
    payload_off = 52 + 32
    filesz = len(words) * 4
    shstr = b"\x00.text\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 3 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base, 52, shoff, 0, 52, 32, 1, 40, 3, 2,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into("<10I", blob, shoff + 40, 1, 1, 6, base, payload_off, 16, 0, 0, 4, 0)
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)
    return target


def write_elf_with_entry_outside_text(path: Path) -> int:
    """Place the ELF entry in an executable PT_LOAD prefix before named .text."""
    base = 0x1000
    text_addr = base + 0x20
    words = [
        0x03E00008,  # entry: jr $ra
        0x00000000,  # delay slot
        *([0x00000000] * 6),  # executable, file-backed gap before .text
        0x03E00008,  # .text function
        0x00000000,
        0x00000000,
        0x00000000,
    ]
    payload_off = 52 + 32
    filesz = len(words) * 4
    shstr = b"\x00.text\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 3 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base, 52, shoff, 0, 52, 32, 1, 40, 3, 2,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into(
        "<10I", blob, shoff + 40,
        1, 1, 6, text_addr, payload_off + text_addr - base, 16, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)
    return base


def write_elf_with_segment_start_trampoline(path: Path) -> int:
    """Place a framed trampoline at PT_LOAD start while ELF entry is in .text."""
    base = 0x2000
    text_addr = base + 0x20
    words = [
        0x27BDFFF0,  # addiu $sp, $sp, -16
        0x03E00008,  # jr $ra
        0x00000000,  # delay slot
        *([0x00000000] * 5),  # executable, file-backed gap before .text
        0x03E00008,  # .text function / ELF entry
        0x00000000,
        0x00000000,
        0x00000000,
    ]
    payload_off = 52 + 32
    filesz = len(words) * 4
    shstr = b"\x00.text\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 3 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, text_addr, 52, shoff, 0, 52, 32, 1, 40, 3, 2,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into(
        "<10I", blob, shoff + 40,
        1, 1, 6, text_addr, payload_off + text_addr - base, 16, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)
    return base


def write_elf_with_unusual_section_code_pointer(path: Path) -> int:
    """Point to out-of-.text code from a linker-specific data section."""
    base = 0x3000
    text_addr = base + 0x20
    target = base + 0x40
    pointer_addr = base + 0x48
    words = [0x00000000] * 19
    words[8] = 0x03E00008  # ELF entry in .text
    words[16] = 0x03E00008  # address-taken leaf outside .text
    words[18] = target  # callback pointer in .callback_refs
    payload_off = 52 + 32
    filesz = len(words) * 4
    shstr = b"\x00.text\x00.callback_refs\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 4 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, text_addr, 52, shoff, 0, 52, 32, 1, 40, 4, 3,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into(
        "<10I", blob, shoff + 40,
        1, 1, 6, text_addr, payload_off + text_addr - base, 8, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 1, 2, pointer_addr, payload_off + pointer_addr - base, 4, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 120,
        22, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)
    return target


def write_elf_with_data_jal_in_text_to_rodata(path: Path) -> int:
    """Place a data word in .text that decodes as a JAL into same-segment .rodata."""
    base = 0x1000
    text_addr = base + 0x20
    target = base + 0x40
    payload_off = 52 + 32
    filesz = 0x44
    shstr = b"\x00.text\x00.rodata\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 4 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base, 52, shoff, 0, 52, 32, 1, 40, 4, 3,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    struct.pack_into("<II", blob, payload_off, 0x03E00008, 0x00000000)
    jal = 0x0C000000 | ((target >> 2) & 0x03FFFFFF)
    struct.pack_into("<I", blob, payload_off + text_addr - base, jal)
    struct.pack_into("<I", blob, payload_off + target - base, 0x03E00008)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into(
        "<10I", blob, shoff + 40,
        1, 1, 6, text_addr, payload_off + text_addr - base, 4, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 1, 2, target, payload_off + target - base, 4, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 120,
        15, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)
    return target


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

    def test_direct_call_frontier_does_not_rescan_prior_targets(self) -> None:
        function_count = 128
        chain_elf = self.root / "direct-call-chain.elf"
        write_direct_call_chain_elf(chain_elf, function_count)
        image = analyze.Elf(str(chain_elf), base=0)

        builtin_set = set
        tracked_calls = [None]
        scanned_entries = 0

        class CountingSet(builtin_set):
            def __iter__(self):
                nonlocal scanned_entries
                if self is tracked_calls[0]:
                    scanned_entries += len(self)
                return super().__iter__()

        def counted_set(*args):
            return CountingSet(*args)

        original_trace = analyze.trace_function

        def trace_and_record_calls(elf, start, ranges, covered, calls, hc, walked=None):
            if tracked_calls[0] is None:
                tracked_calls[0] = calls
            return original_trace(elf, start, ranges, covered, calls, hc, walked=walked)

        with redirect_stderr(io.StringIO()), \
             mock.patch.object(analyze, "set", counted_set, create=True), \
             mock.patch.object(analyze, "trace_function", trace_and_record_calls):
            starts, _ranges = analyze.analyze(image)

        self.assertEqual(len(starts), function_count)
        self.assertLessEqual(
            scanned_entries, function_count * 2,
            "call discovery must process newly found targets without rescanning the full set",
        )

    def test_direct_call_owns_only_reachable_code_outside_named_text(self) -> None:
        called_elf = self.root / "called-outside-text.elf"
        target = write_elf_with_called_code_outside_text(called_elf)
        loaded = analyze.Elf(str(called_elf), base=0)

        starts, ranges = analyze.analyze(loaded)

        self.assertIn(target, starts)
        self.assertIn((target, target + 8), ranges)
        self.assertFalse(analyze.in_ranges(target + 8, ranges))

    def test_entry_outside_named_text_owns_only_its_reachable_instructions(self) -> None:
        entry_elf = self.root / "entry-outside-text.elf"
        entry = write_elf_with_entry_outside_text(entry_elf)
        loaded = analyze.Elf(str(entry_elf), base=0)

        starts, ranges = analyze.analyze(loaded)

        self.assertIn(entry, starts)
        self.assertIn((entry, entry + 8), ranges)
        self.assertFalse(analyze.in_ranges(entry + 8, ranges))
        self.assertIn((entry + 0x20, entry + 0x30), ranges)

    def test_segment_start_trampoline_is_owned_without_widening_the_gap(self) -> None:
        trampoline_elf = self.root / "segment-start-trampoline.elf"
        trampoline = write_elf_with_segment_start_trampoline(trampoline_elf)
        loaded = analyze.Elf(str(trampoline_elf), base=0)

        starts, ranges = analyze.analyze(loaded)

        self.assertIn(trampoline, starts)
        self.assertIn((trampoline, trampoline + 12), ranges)
        self.assertFalse(analyze.in_ranges(trampoline + 12, ranges))
        self.assertIn((trampoline + 0x20, trampoline + 0x30), ranges)

    def test_unusual_section_pointer_owns_only_out_of_text_target(self) -> None:
        pointer_elf = self.root / "unusual-section-code-pointer.elf"
        target = write_elf_with_unusual_section_code_pointer(pointer_elf)
        loaded = analyze.Elf(str(pointer_elf), base=0)

        starts, ranges = analyze.analyze(loaded)

        self.assertIn(target, starts)
        self.assertIn((target, target + 8), ranges)
        self.assertFalse(analyze.in_ranges(target + 8, ranges))

    def test_linear_sweep_does_not_treat_jal_data_as_rodata_function(self) -> None:
        data_elf = self.root / "data-jal-to-rodata.elf"
        target = write_elf_with_data_jal_in_text_to_rodata(data_elf)
        loaded = analyze.Elf(str(data_elf), base=0)

        starts, _ = analyze.analyze(loaded)

        self.assertNotIn(target, starts)

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

    def test_cfg_report_preserves_continuation_and_call_ownership(self) -> None:
        target = PRIMARY_BASE + 0x10
        tail_source = PRIMARY_BASE + 0x20
        words = [
            0x0C000000 | ((target >> 2) & 0x03FFFFFF),  # jal target
            0x00000000,                                  # delay slot
            0x03E00008,                                  # continuation entry
            0x00000000,                                  # delay slot
            0x00000000,
            0x00000000,
            0x03E00008,                                  # called entry
            0x00000000,                                  # delay slot
            0x08000000 | ((target >> 2) & 0x03FFFFFF),   # direct tail-call to target
            0x00000000,                                  # delay slot
        ]
        elf_path = self.root / "cfg-owned.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + len(words) * 4)],
            entries=[PRIMARY_BASE, PRIMARY_BASE + 8, target, tail_source],
        )

        self.assertEqual(analyze.canonical_cfg_gate(report), [])
        classified = {
            (row["source"], row["target"], row["kind"]): row["classification"]
            for row in report["successor_classifications"]
        }
        self.assertEqual(len(classified), len(report["edges"]))
        self.assertEqual(
            classified[(PRIMARY_BASE, PRIMARY_BASE + 4, "delay-slot")],
            "same-region",
        )
        self.assertEqual(
            classified[(PRIMARY_BASE, PRIMARY_BASE + 8, "fallthrough")],
            "continuation-resume-entry",
        )
        self.assertEqual(
            classified[(PRIMARY_BASE, target, "call")],
            "inter-function-handoff-dispatch",
        )
        self.assertEqual(
            classified[(tail_source, target, "tail-call")],
            "inter-function-handoff-dispatch",
        )
        by_address = {row["address"]: row for row in report["instructions"]}
        self.assertEqual(by_address[PRIMARY_BASE + 8]["classification"], "interior-entry")
        self.assertTrue(
            any(
                edge["source"] == PRIMARY_BASE
                and edge["target"] == PRIMARY_BASE + 8
                for edge in report["continuation_edges"]
            )
        )
        self.assertTrue(
            any(
                edge["source"] == PRIMARY_BASE
                and edge["target"] == target
                and edge["kind"] == "call"
                for edge in report["call_edges"]
            )
        )

    def test_cfg_gate_rejects_a_missing_successor_classification(self) -> None:
        image = analyze.Elf(str(self.elf), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + 8)],
            entries=[PRIMARY_BASE],
        )
        report["successor_classifications"].pop()
        findings = analyze.canonical_cfg_gate(report)
        self.assertIn(
            "successor-classification-coverage",
            {finding["code"] for finding in findings},
        )

    def test_cfg_gate_records_unreached_executable_word(self) -> None:
        # A nonzero word after an early return has no incoming known edge. Keep
        # it visible as unowned instead of treating file adjacency as reachability.
        words = [
            0x03E00008,  # jr $ra
            0x00000000,  # delay slot
            0x3C020001,  # unreachable, non-padding instruction word
            0x00000000,
        ]
        elf_path = self.root / "cfg-gap.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + len(words) * 4)],
            entries=[PRIMARY_BASE],
        )

        self.assertEqual(analyze.canonical_cfg_gate(report), [])
        self.assertIn(PRIMARY_BASE + 8, report["unowned_executable"])

    def test_cfg_gate_kills_owned_edge_into_unowned_callee(self) -> None:
        # Simulate an analyzer regression that drops a direct-call entry from
        # the seed set. The owned caller still has a precise in-image edge to
        # the unowned callee, so the semantic gate must reject the report.
        callee = PRIMARY_BASE + 16
        words = [
            0x0C000404,  # jal 0x00001010
            0x00000000,  # delay slot
            0x03E00008,  # caller return
            0x00000000,  # delay slot
            0x03E00008,  # unseeded callee
            0x00000000,  # delay slot
        ]
        elf_path = self.root / "cfg-unowned-callee.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + len(words) * 4)],
            entries=[PRIMARY_BASE],
        )

        self.assertIn(callee, report["unowned_executable"])
        findings = analyze.canonical_cfg_gate(report)
        self.assertIn("ownership-gap", {finding["code"] for finding in findings})
        self.assertTrue(any("0x00001010" in finding["message"] for finding in findings))

    def test_codegen_gate_kills_analyzer_mutation_before_emission(self) -> None:
        # Force the production codegen seam to lose the direct-call root. The
        # independent CFG walk still sees the in-image call edge and must stop
        # before any generated C is written.
        elf_path = self.root / "cfg-missing-call-root.elf"
        out_c = self.root / "cfg-missing-call-root.c"
        words = (
            0x0C000404, 0x00000000, 0x03E00008,
            0x00000000, 0x03E00008, 0x00000000,
        )
        write_elf(elf_path, words=words)
        stderr = io.StringIO()
        with mock.patch.object(
            codegen,
            "analyze",
            return_value=([PRIMARY_BASE], [(PRIMARY_BASE, PRIMARY_BASE + len(words) * 4)]),
        ):
            with mock.patch.dict(os.environ, self._clean_env(), clear=True):
                with redirect_stderr(stderr):
                    rc = codegen.main([
                        "codegen.py", str(elf_path), str(out_c),
                        "--base=0", "--profile=none", "--cfg-gate",
                    ])

        self.assertEqual(rc, 1, stderr.getvalue())
        self.assertIn("CFG_GATE ownership-gap", stderr.getvalue())
        self.assertIn("0x00001010", stderr.getvalue())
        self.assertFalse(out_c.exists(), "ownership-gap failure must precede C emission")

    def test_cfg_gate_kills_generated_sequential_fallthrough_gap_mutation(self) -> None:
        # A mutant that stops ownership at sequential fallthrough silently
        # leaves the next emitted word outside its callable region. The CFG
        # gate must reject that edge before codegen writes C.
        elf_path = self.root / "cfg-mutant-fallthrough.elf"
        write_elf(elf_path, words=(0x24020001, 0x03E00008, 0x00000000))

        mutant_tools = self.root / "fallthrough_mutant_tools"
        mutant_tools.mkdir()
        analyzer_source = (TOOLS / "analyze.py").read_text(encoding="utf-8")
        mutation_anchor = (
            '                if kind_id in {\n'
            '                    _CFG_KIND_IDS["call"], _CFG_KIND_IDS["tail-call"],\n'
            '                    _CFG_KIND_IDS["unresolved-indirect"],\n'
            '                }:\n'
        )
        self.assertEqual(analyzer_source.count(mutation_anchor), 1)
        mutant_analyzer = analyzer_source.replace(
            mutation_anchor,
            mutation_anchor.replace(
                '_CFG_KIND_IDS["unresolved-indirect"],',
                '_CFG_KIND_IDS["unresolved-indirect"],\n'
                '                    _CFG_KIND_IDS["fallthrough"],',
            ),
            1,
        )
        (mutant_tools / "analyze.py").write_text(
            mutant_analyzer,
            encoding="utf-8",
            newline="\n",
        )
        (mutant_tools / "codegen.py").write_text(
            (TOOLS / "codegen.py").read_text(encoding="utf-8"),
            encoding="utf-8",
            newline="\n",
        )
        out_dir = self.root / "fallthrough_mutant_output"
        out_dir.mkdir()
        output_c = out_dir / "mutant.c"
        report_path = out_dir / "mutant-cfg.json"
        env = self._clean_env(
            PYTHONPATH=os.pathsep.join((str(mutant_tools), str(TOOLS)))
        )
        result = subprocess.run(
            [
                sys.executable, str(mutant_tools / "codegen.py"),
                str(elf_path), str(output_c), "--base=0", "--profile=none",
                "--funcs-per-chunk=2000", "--cfg-gate",
                f"--cfg-report={report_path}",
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("CFG_GATE ownership-gap", result.stderr)
        self.assertIn("0x00001004", result.stderr)
        self.assertFalse(output_c.exists())
        report = package_cache.read_bounded_json(report_path)
        gap = next(
            row for row in report["successor_classifications"]
            if row["source"] == PRIMARY_BASE and row["target"] == PRIMARY_BASE + 4
        )
        self.assertEqual(gap["kind"], "fallthrough")
        self.assertEqual(gap["classification"], "invalid-unowned")

    def test_post_jump_word_is_not_promoted_from_file_adjacency(self) -> None:
        # This mirrors the cosim `jump` cell: the direct jump's delay slot is
        # followed by a nonzero marker that has no incoming control-flow edge.
        # File adjacency after an unconditional transfer is not callable evidence.
        words = [
            0x24020000,  # addiu v0, zero, 0
            0x08000404,  # j 0x00001010
            0x24420001,  # delay slot
            0x24420400,  # unreachable marker at 0x100c
            0x24420002,  # jump target
            0x03E00008,  # jr $ra
            0x24420010,  # return delay slot
        ]
        elf_path = self.root / "cfg-post-jump.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        starts, ranges = analyze.analyze(image, cfg_gate=True)
        self.assertIn(PRIMARY_BASE, starts)
        self.assertNotIn(PRIMARY_BASE + 8, starts)
        self.assertNotIn(PRIMARY_BASE + 12, starts)

        report = analyze.canonical_cfg_report(image, ranges=ranges, entries=starts)
        by_address = {row["address"]: row for row in report["instructions"]}
        self.assertEqual(by_address[PRIMARY_BASE + 12]["owners"], [])
        self.assertEqual(by_address[PRIMARY_BASE + 12]["classification"], "unowned-executable")

    @unittest.skipUnless(CC, "gcc is required for the analyzer semantic mutation proof")
    def test_cfg_gate_kills_generated_direct_call_root_mutation(self) -> None:
        words = (
            0x0C000404, 0x00000000, 0x03E00008,
            0x00000000, 0x03E00008, 0x00000000,
        )
        target = PRIMARY_BASE + 16
        elf_path = self.root / "cfg-mutant-direct-call.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        starts, _ = analyze.analyze(image)
        self.assertIn(target, starts)

        mutant_tools = self.root / "mutant_tools"
        mutant_tools.mkdir()
        analyzer_source = (TOOLS / "analyze.py").read_text(encoding="utf-8")
        mutation_anchor = (
            "                    if target not in calls:\n"
            "                        calls.add(target)\n"
            "                        new_calls.append(target)\n"
            "                covered.add(pc + 4)"
        )
        self.assertEqual(analyzer_source.count(mutation_anchor), 1)
        (mutant_tools / "analyze.py").write_text(
            analyzer_source.replace(
                mutation_anchor,
                "                    pass\n                covered.add(pc + 4)",
                1,
            ),
            encoding="utf-8",
            newline="\n",
        )
        (mutant_tools / "codegen.py").write_text(
            (TOOLS / "codegen.py").read_text(encoding="utf-8"),
            encoding="utf-8",
            newline="\n",
        )
        out_dir = self.root / "mutant_output"
        out_dir.mkdir()
        out_c = out_dir / "mutant.c"
        env = self._clean_env(
            PYTHONPATH=os.pathsep.join((str(mutant_tools), str(TOOLS)))
        )
        generated = subprocess.run(
            [
                sys.executable, str(mutant_tools / "codegen.py"),
                str(elf_path), str(out_c), "--base=0", "--profile=none",
                "--funcs-per-chunk=2000",
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
        self.assertTrue(out_c.is_file(), generated.stdout + generated.stderr)

        gated_out = out_dir / "gated.c"
        mutant_report_path = out_dir / "mutant-cfg.json"
        gated = subprocess.run(
            [
                sys.executable, str(mutant_tools / "codegen.py"),
                str(elf_path), str(gated_out), "--base=0", "--profile=none",
                "--funcs-per-chunk=2000", "--cfg-gate",
                f"--cfg-report={mutant_report_path}",
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(gated.returncode, 1, gated.stdout + gated.stderr)
        self.assertIn("CFG_GATE ownership-gap", gated.stderr)
        self.assertIn("0x00001010", gated.stderr)
        self.assertFalse(gated_out.exists())
        mutant_report = package_cache.read_bounded_json(mutant_report_path)
        gap = next(
            row for row in mutant_report["successor_classifications"]
            if row["source"] == PRIMARY_BASE and row["target"] == target
        )
        self.assertEqual(gap["classification"], "invalid-unowned")

        config = subprocess.run(
            [
                sys.executable, str(TOOLS / "title_runtime_config.py"),
                "--output", str(self.root / "sr_title_config.h"),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(config.returncode, 0, config.stdout + config.stderr)

        harness = self.root / "analyzer_mutation_harness.c"
        harness.write_text(
            "#define main embedded_dispatch_isolation_main\n"
            "#include \"dispatch_isolation_selftest.c\"\n"
            "#undef main\n"
            "extern void sr_register_all(void);\n"
            "int main(void) {\n"
            "    sr_register_all();\n"
            "    if (sr_lookup(0x00001010u) == NULL) {\n"
            "        fprintf(stderr, \"ANALYZER_MUTANT_DROPPED_CALL_TARGET\\n\");\n"
            "        return 17;\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
            newline="\n",
        )
        exe = self.root / "analyzer_mutation_harness.exe"
        compiled = subprocess.run(
            [
                CC, "-std=c11", "-O0", "-fno-strict-aliasing", "-Wall", "-Wextra",
                "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
                "-I", str(self.root), "-I", str(ROOT / "src" / "rt"),
                str(harness), str(out_c), str(out_dir / "mutant_0.c"),
                str(ROOT / "src" / "rt" / "guest_interp.c"),
                str(ROOT / "src" / "rt" / "perf.c"),
                str(ROOT / "src" / "rt" / "cpu_lle.c"),
                str(ROOT / "src" / "rt" / "domain_mode.c"),
                str(ROOT / "src" / "rt" / "stale_code.c"),
                str(ROOT / "src" / "rt" / "title_config.c"),
                str(ROOT / "src" / "rt" / "vfpu_tables.c"),
                "-lm", "-o", str(exe),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        mutated_run = subprocess.run(
            [str(exe)], cwd=ROOT, capture_output=True, text=True, check=False
        )
        self.assertEqual(mutated_run.returncode, 17, mutated_run.stdout + mutated_run.stderr)
        self.assertIn("ANALYZER_MUTANT_DROPPED_CALL_TARGET", mutated_run.stderr)

    def test_cfg_gate_treats_jalr_zero_as_tail_transfer(self) -> None:
        # Codegen dispatches jalr with rd=$zero without a resume path. The
        # following word is therefore not a callable fallthrough successor.
        words = [
            0x01000009,  # jalr $zero, $t0
            0x00000000,  # delay slot
            0x3C020001,  # unreachable marker after non-returning dispatch
            0x00000000,
        ]
        elf_path = self.root / "cfg-jalr-zero-tail.elf"
        write_elf(elf_path, words=words)
        image = analyze.Elf(str(elf_path), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + len(words) * 4)],
            entries=[PRIMARY_BASE],
        )

        marker = next(
            row for row in report["ownership_map"]
            if row["address"] == PRIMARY_BASE + 8
        )
        self.assertEqual(marker["classification"], "unowned-executable")
        self.assertIn(
            {"source": PRIMARY_BASE, "target": None, "kind": "unresolved-indirect",
             "detail": "computed-tail-transfer"},
            report["edges"],
        )
        self.assertIn(
            {
                "source": PRIMARY_BASE,
                "target": None,
                "kind": "unresolved-indirect",
                "classification": "interpreter-fail-closed-boundary",
                "detail": "computed-tail-transfer",
            },
            report["successor_classifications"],
        )
        self.assertFalse(
            any(
                edge["source"] == PRIMARY_BASE
                and edge["target"] == PRIMARY_BASE + 8
                and edge["kind"] == "fallthrough"
                for edge in report["edges"]
            )
        )
        self.assertEqual(analyze.canonical_cfg_gate(report), [])

    def test_codegen_cfg_gate_keeps_unreached_word_visible(self) -> None:
        elf_path = self.root / "cfg-gap-cli.elf"
        out_c = self.root / "cfg-gap-cli.c"
        report_path = self.root / "cfg-gap-cli.json"
        write_elf(
            elf_path,
            # A direct jump skips the nonzero word at 0x100c after its delay
            # slot. The CFG retains it as unowned, without inventing a root.
            words=(
                0x24020000, 0x08000404, 0x24420001, 0x24420400,
                0x24420002, 0x03E00008, 0x24420010,
            ),
        )
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"), str(elf_path), str(out_c),
                "--base=0", "--profile=none", "--cfg-gate",
                f"--cfg-report={report_path}",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("CFG_GATE PASS", proc.stdout)
        self.assertIn("unowned words recorded", proc.stdout)
        self.assertTrue(out_c.exists())
        self.assertIn(
            PRIMARY_BASE + 12,
            json.loads(report_path.read_text(encoding="ascii"))["unowned_executable"],
        )

    def test_analyze_cfg_report_alone_does_not_change_entry_inventory(self) -> None:
        elf_path = self.root / "analyze-report-only.elf"
        write_elf(
            elf_path,
            words=(0x08000404, 0, 0x03E00008, 0, 0, 0, 0x03E00008, 0),
        )

        def inventory(name: str, *flags: str) -> str:
            toml_path = self.root / f"{name}.toml"
            proc = subprocess.run(
                [
                    sys.executable, str(TOOLS / "analyze.py"), str(elf_path),
                    "--base=0", f"--toml={toml_path}", *flags,
                ],
                cwd=ROOT, env=self._clean_env(), capture_output=True, text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return toml_path.read_text(encoding="utf-8")

        default = inventory("analyze-default")
        report_only = inventory(
            "analyze-report-only", f"--cfg-report={self.root / 'analyze-report.json'}"
        )
        self.assertEqual(report_only, default)
        self.assertNotEqual(inventory("analyze-gated", "--cfg-gate"), default)

    def test_codegen_cfg_report_alone_does_not_change_generated_code(self) -> None:
        elf_path = self.root / "cfg-report-only.elf"
        # A direct jump followed by a `jr $ra`: the default lane seeds 0x1008 as
        # an entry, the gate lane does not, so the two outputs differ.
        write_elf(
            elf_path,
            words=(0x08000404, 0, 0x03E00008, 0, 0, 0, 0x03E00008, 0),
        )

        def generate(name: str, *flags: str) -> dict[str, bytes]:
            out_dir = self.root / name
            out_dir.mkdir()
            proc = subprocess.run(
                [
                    sys.executable, str(TOOLS / "codegen.py"), str(elf_path),
                    str(out_dir / "out.c"), "--base=0", "--profile=none",
                    "--funcs-per-chunk=2000", *flags,
                ],
                cwd=ROOT, env=self._clean_env(), capture_output=True, text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return {path.name: path.read_bytes() for path in sorted(out_dir.glob("*.c"))}

        default = generate("cfg-default")
        report_path = self.root / "cfg-report-only.json"
        report_only = generate("cfg-report-only", f"--cfg-report={report_path}")
        gated_path = self.root / "cfg-gated-report.json"
        gated = generate("cfg-gated", f"--cfg-report={gated_path}", "--cfg-gate")

        self.assertEqual(report_only, default)
        self.assertNotEqual(gated, default, "fixture must distinguish the gate lane")
        # Both reports share the gate-mode CFG; their codegen ownership maps
        # intentionally describe the different entry inventories actually emitted.
        report_only_document = package_cache.read_bounded_json(report_path)
        gated_document = package_cache.read_bounded_json(gated_path)
        report_only_map = report_only_document.pop("emitted_ownership_map")
        gated_map = gated_document.pop("emitted_ownership_map")
        self.assertEqual(report_only_document, gated_document)
        self.assertEqual(
            report_only_map["source_cfg_schema_version"], analyze.CFG_SCHEMA_VERSION,
        )
        self.assertEqual(
            gated_map["source_cfg_schema_version"], analyze.CFG_SCHEMA_VERSION,
        )

    def test_codegen_cfg_gate_records_unreached_extra_elf_word(self) -> None:
        primary = self.root / "cfg-primary.elf"
        extra = self.root / "cfg-extra-gap.elf"
        out_c = self.root / "cfg-extra-gap.c"
        write_elf(primary)
        write_elf(
            extra,
            load_addr=0x2000,
            words=(0x08000000, 0x00000000, 0x3C020001, 0x00000000),
        )
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"), str(primary), str(out_c),
                "--base=0", "--profile=none", "--cfg-gate",
                f"--extra-elf={extra}@0x2000",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("CFG_GATE PASS: extra module", proc.stdout)
        self.assertTrue(out_c.exists())

    def test_codegen_rejects_overlapping_primary_and_extra_exec_spans(self) -> None:
        primary = self.root / "cfg-primary-overlap.elf"
        extra = self.root / "cfg-extra-overlap.elf"
        out_c = self.root / "cfg-overlap.c"
        write_elf(primary)
        write_elf(extra)
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"), str(primary), str(out_c),
                "--base=0", "--profile=none", "--cfg-gate",
                f"--extra-elf={extra}@0x1000",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("CODEGEN_EXEC_SPAN_OVERLAP", proc.stderr)
        self.assertFalse(out_c.exists(), "overlap must be rejected before C emission")

    def test_cfg_cli_writes_machine_report_and_runs_gate(self) -> None:
        report_path = self.root / "reports" / "cfg.json"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "analyze.py"), str(self.elf),
                "--base=0", f"--cfg-report={report_path}", "--cfg-gate",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        report = json.loads(report_path.read_text(encoding="ascii"))
        self.assertEqual(report["schema_version"], analyze.CFG_SCHEMA_VERSION)
        self.assertEqual(
            [row["address"] for row in report["ownership_map"]],
            [row["address"] for row in report["instructions"]],
        )
        self.assertEqual(analyze.canonical_cfg_gate(report), [])
        self.assertIn("CFG_GATE PASS", proc.stdout)

    def test_codegen_cfg_gate_is_opt_in_and_writes_the_same_report(self) -> None:
        out_c = self.root / "cfg-gated.c"
        report_path = self.root / "reports" / "codegen-cfg.json"
        proc = subprocess.run(
            [
                sys.executable, str(TOOLS / "codegen.py"), str(self.elf), str(out_c),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000",
                f"--cfg-report={report_path}", "--cfg-gate",
            ],
            cwd=ROOT, env=self._clean_env(), capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(out_c.is_file())
        report = json.loads(report_path.read_text(encoding="ascii"))
        self.assertEqual(analyze.canonical_cfg_gate(report), [])
        self.assertIn("CFG_GATE PASS", proc.stdout)
        bounded_report = package_cache.read_bounded_json(report_path)
        emitted_map = bounded_report["emitted_ownership_map"]
        self.assertEqual(emitted_map["schema_version"], codegen.EMITTED_OWNERSHIP_SCHEMA_VERSION)
        regions = {row["start"]: row for row in emitted_map["regions"]}
        self.assertEqual(regions[PRIMARY_BASE]["classification"], "callable")
        self.assertTrue(regions[PRIMARY_BASE]["emitted"])
        self.assertEqual(regions[PRIMARY_BASE + 4]["classification"], "interior")

    def test_emitted_ownership_map_preserves_resume_owner(self) -> None:
        report = {
            "schema_version": analyze.CFG_SCHEMA_VERSION,
            "ownership_map": [
                {"address": PRIMARY_BASE, "classification": "owned", "owners": [PRIMARY_BASE]},
                {"address": PRIMARY_BASE + 4, "classification": "owned", "owners": [PRIMARY_BASE]},
                {"address": PRIMARY_BASE + 8, "classification": "interior-entry", "owners": [PRIMARY_BASE + 8]},
            ],
            "byte_classification": [
                {"start": PRIMARY_BASE, "end": PRIMARY_BASE + 4, "classification": "owned"},
                {"start": PRIMARY_BASE + 4, "end": PRIMARY_BASE + 8, "classification": "owned"},
                {"start": PRIMARY_BASE + 8, "end": PRIMARY_BASE + 12, "classification": "interior-entry"},
            ],
        }
        catalog = {
            PRIMARY_BASE: codegen.EntryInfo(PRIMARY_BASE, True, False, None, frozenset()),
            PRIMARY_BASE + 8: codegen.EntryInfo(
                PRIMARY_BASE + 8, False, True, PRIMARY_BASE, frozenset(),
            ),
        }
        emitted_map = codegen.build_emitted_ownership_map(
            report, catalog, [PRIMARY_BASE, PRIMARY_BASE + 8],
        )
        regions = {row["start"]: row for row in emitted_map["regions"]}
        self.assertEqual(regions[PRIMARY_BASE]["classification"], "callable")
        self.assertEqual(regions[PRIMARY_BASE + 4]["classification"], "interior")
        self.assertEqual(regions[PRIMARY_BASE + 8]["classification"], "resume")
        self.assertEqual(regions[PRIMARY_BASE + 8]["owners"], [PRIMARY_BASE])
        self.assertEqual(regions[PRIMARY_BASE + 8]["symbol"], "r_00001008")

    def test_emitted_ownership_map_keeps_callable_conflicts_ambiguous(self) -> None:
        report = {
            "schema_version": analyze.CFG_SCHEMA_VERSION,
            "ownership_map": [{
                "address": PRIMARY_BASE,
                "classification": "owner-conflict",
                "owners": [PRIMARY_BASE, PRIMARY_BASE + 4],
            }],
            "byte_classification": [{
                "start": PRIMARY_BASE,
                "end": PRIMARY_BASE + 4,
                "classification": "owner-conflict",
            }],
        }
        catalog = {
            PRIMARY_BASE: codegen.EntryInfo(
                PRIMARY_BASE, True, False, None, frozenset(),
            ),
        }
        emitted_map = codegen.build_emitted_ownership_map(
            report, catalog, [PRIMARY_BASE],
        )
        region = emitted_map["regions"][0]
        self.assertEqual(region["classification"], "ambiguous")
        self.assertEqual(region["owners"], [PRIMARY_BASE, PRIMARY_BASE + 4])

    def test_cfg_report_verifier_kills_missing_projection(self) -> None:
        image = analyze.Elf(str(self.elf), base=0)
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(PRIMARY_BASE, PRIMARY_BASE + 8)],
            entries=[PRIMARY_BASE],
        )
        report["byte_classification"].pop()
        findings = analyze.canonical_cfg_gate(report)
        self.assertTrue(any(item["code"] == "executable-coverage-gap" for item in findings))

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
        self.assertIn("TITLE_EXTRA_SPANS ?=\n", self.makefile)
        self.assertNotIn("HST_EXTRA_SPANS", self.makefile)

    def test_span_participates_in_the_codegen_profile_hash(self) -> None:
        # Changing the span must invalidate previously generated code. The codegen
        # profile's entries are NK_CODEGEN_PROFILE_ENTRIES; the span is one of them,
        # the parse-time hash reads them through profile_hash and the record recipe
        # from its environment.
        export_line = next(
            line for line in self.makefile.splitlines()
            if line.startswith("export NK_CODEGEN_PROFILE_ENTRIES :=")
        )
        self.assertIn("EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)", export_line)
        hash_line = next(
            line for line in self.makefile.splitlines()
            if line.startswith("CODEGEN_PROFILE_HASH :=")
        )
        self.assertIn("$(call profile_hash,NK_CODEGEN_PROFILE_ENTRIES,", hash_line)
        record_line = next(
            line for line in self.makefile.splitlines() if "--section codegen" in line
        )
        self.assertIn("--entries-env NK_CODEGEN_PROFILE_ENTRIES", record_line)


MIPS_JR_RA = 0x03E00008
MIPS_NOP = 0x00000000
MIPS_ADDU_T0 = 0x01094021  # addu $t0, $t0, $t1
MIPS_PROLOGUE = 0x27BDFFF0  # addiu $sp, $sp, -16
RANDOM_CODE_IMAGES = 48


def write_words_elf(path: Path, words, *, base: int = PRIMARY_BASE, entry: int | None = None,
                    text_words: int | None = None) -> None:
    """Fabricate a little-endian ET_EXEC whose one executable PT_LOAD holds `words`.

    The section table names .text over the first `text_words` words only, so the words after
    it are executable file bytes outside the named code section. That is the case which gives
    a trace merged ranges.
    """
    if text_words is None:
        text_words = len(words)
    payload_off = 52 + 32
    filesz = len(words) * 4
    shstr = b"\x00.text\x00.shstrtab\x00"
    shstr_off = payload_off + filesz
    shoff = shstr_off + len(shstr)
    blob = bytearray(shoff + 3 * 40)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base if entry is None else entry, 52, shoff, 0, 52, 32, 1, 40, 3, 2,
    )
    struct.pack_into("<8I", blob, 52, 1, payload_off, base, base, filesz, filesz, 5, 4)
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word & 0xFFFFFFFF)
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    struct.pack_into(
        "<10I", blob, shoff + 40,
        1, 1, 6, base, payload_off, text_words * 4, 0, 0, 4, 0,
    )
    struct.pack_into(
        "<10I", blob, shoff + 80,
        7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0,
    )
    path.write_bytes(blob)


def random_code_words(rng: random.Random, base: int, count: int) -> list[int]:
    """Words drawn from the shapes the trace branches on, with targets around the image."""

    def target() -> int:
        # Mostly inside the image, sometimes just past it.
        return base + 4 * rng.randrange(0, count + 16)

    words: list[int] = []
    while len(words) < count:
        pick = rng.random()
        if pick < 0.14:
            words.append(0x0C000000 | ((target() >> 2) & 0x03FFFFFF))  # jal
        elif pick < 0.24:
            words.append(0x08000000 | ((target() >> 2) & 0x03FFFFFF))  # j
        elif pick < 0.36:
            if rng.random() < 0.25:
                words.append(0x10000000 | (rng.randrange(-24, 25) & 0xFFFF))  # b
            else:
                op = rng.choice((0x04, 0x05, 0x06, 0x07))  # beq, bne, blez, bgtz
                rs, rt = rng.randrange(4), rng.randrange(4)
                words.append((op << 26) | (rs << 21) | (rt << 16)
                             | (rng.randrange(-24, 25) & 0xFFFF))
        elif pick < 0.40:
            rt = rng.choice((0x00, 0x01, 0x10, 0x11))  # bltz, bgez, bltzal, bgezal
            words.append((0x01 << 26) | (rt << 16) | (rng.randrange(-24, 25) & 0xFFFF))
        elif pick < 0.50:
            words.append(rng.choice((MIPS_JR_RA, 0x03200008, 0x0040F809)))  # jr $ra, jr $t9, jalr
        elif pick < 0.62:
            words.append(0x27BD0000 | ((-8 * rng.randrange(1, 17)) & 0xFFFF))  # addiu $sp, -N
        elif pick < 0.70:
            words.append(MIPS_NOP)
        elif pick < 0.80:
            reg = rng.randrange(1, 8)
            address = target()
            words.append(0x3C000000 | (reg << 16) | (address >> 16))  # lui
            words.append(0x24000000 | (reg << 21) | (reg << 16) | (address & 0xFFFF))  # addiu
        else:
            words.append(rng.getrandbits(32))
    return words[:count]


class TraceMemoTests(unittest.TestCase):
    """The memoized trace walk reproduces the uncached walk, and expands each word once."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-trace-memo-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def _walk(self, image_bytes: bytes, *, trace_memo: bool) -> dict:
        """Every trace's start, owned ranges and call results, plus the analysis outputs."""
        image = analyze.Elf(image_bytes, base=0)
        steps = []
        final: dict = {}
        original = analyze.trace_function

        def recording_trace(elf, start, ranges, covered, calls, hc, walked=None):
            new_calls = original(elf, start, ranges, covered, calls, hc, walked=walked)
            steps.append((start, tuple(ranges), tuple(new_calls)))
            final["covered"] = covered
            final["calls"] = calls
            return new_calls

        with redirect_stderr(io.StringIO()), \
             mock.patch.object(analyze, "trace_function", recording_trace):
            starts, ranges = analyze.analyze(image, trace_memo=trace_memo)
        return {
            "steps": steps,
            "covered": sorted(final.get("covered", ())),
            "calls": sorted(final.get("calls", ())),
            "starts": sorted(starts),
            "ranges": list(ranges),
        }

    def test_memoized_walk_matches_uncached_walk_on_fixtures(self) -> None:
        for writer in (
            write_elf_with_called_code_outside_text,
            write_elf_with_entry_outside_text,
            write_elf_with_segment_start_trampoline,
            write_elf_with_unusual_section_code_pointer,
            write_elf_with_data_jal_in_text_to_rodata,
        ):
            path = self.root / f"{writer.__name__}.elf"
            writer(path)
            image_bytes = path.read_bytes()
            with self.subTest(fixture=writer.__name__):
                self.assertEqual(
                    self._walk(image_bytes, trace_memo=True),
                    self._walk(image_bytes, trace_memo=False),
                )

    def test_memoized_walk_matches_uncached_walk_on_random_code(self) -> None:
        merged_steps = 0
        for seed in range(RANDOM_CODE_IMAGES):
            rng = random.Random(seed)
            count = rng.randrange(40, 129)
            words = random_code_words(rng, PRIMARY_BASE, count)
            path = self.root / f"random-{seed}.elf"
            write_words_elf(path, words, text_words=rng.randrange(count // 2, count + 1))
            image_bytes = path.read_bytes()
            with self.subTest(seed=seed):
                memoized = self._walk(image_bytes, trace_memo=True)
                self.assertEqual(memoized, self._walk(image_bytes, trace_memo=False))
                primary = tuple(analyze.exec_ranges(analyze.Elf(image_bytes, base=0)))
                merged_steps += sum(1 for _start, ranges, _calls in memoized["steps"]
                                    if ranges != primary)
        # The corpus must exercise traces over merged ranges, not only the primary ones.
        self.assertGreater(merged_steps, 0)

    def test_shared_tail_is_expanded_once_across_starts(self) -> None:
        """Every function jumps into one shared tail; the tail is walked once, not per function."""
        shared_words = 256
        functions = 32
        shared = PRIMARY_BASE
        words = [MIPS_ADDU_T0] * shared_words + [MIPS_JR_RA, MIPS_NOP]
        function_starts = []
        for _ in range(functions):
            function_starts.append(PRIMARY_BASE + 4 * len(words))
            words += [MIPS_PROLOGUE, 0x08000000 | ((shared >> 2) & 0x03FFFFFF), MIPS_NOP]
        path = self.root / "shared-tail.elf"
        write_words_elf(path, words, entry=function_starts[0])
        image = analyze.Elf(str(path), base=0)
        reads = 0
        read_word = image.read_at_vaddr

        def counting_read(vaddr, n):
            nonlocal reads
            reads += 1
            return read_word(vaddr, n)

        image.read_at_vaddr = counting_read
        with redirect_stderr(io.StringIO()):
            starts, _ranges = analyze.analyze(image)
        for function_start in function_starts:
            self.assertIn(function_start, starts)
        self.assertLessEqual(
            reads, 4 * (shared_words + 3 * functions) + 64,
            "a shared tail must be expanded once, not once per function",
        )


class SpanIndexMembershipTests(unittest.TestCase):
    """The trace's indexed membership answers exactly as the linear in_ranges scan."""

    CASES = (
        [],
        [(0, 0)],
        [(16, 16), (20, 12)],
        [(0x100, 0x110)],
        [(0x100, 0x110), (0x110, 0x120)],
        [(0x100, 0x140), (0x120, 0x130), (0x200, 0x204)],
        [(0x200, 0x204), (0x100, 0x110), (0x108, 0x104)],
        [(0x100, 0x110), (0x100, 0x110)],
        [(0x0, 0x8), (0x8, 0x10), (0x18, 0x20)],
    )

    def test_membership_matches_in_ranges_at_every_boundary(self) -> None:
        for ranges in self.CASES:
            index = analyze._SpanIndex(ranges)
            probes = {-1, 0, 0xFFFFFFFF + 1}
            for lo, hi in ranges:
                probes.update({lo - 1, lo, lo + 1, hi - 1, hi, hi + 1})
            for addr in sorted(probes):
                with self.subTest(ranges=ranges, addr=addr):
                    self.assertEqual(
                        addr in index,
                        analyze.in_ranges(addr, ranges),
                    )

    def test_in_ranges_accepts_an_index_as_its_range_set(self) -> None:
        ranges = [(0x100, 0x110), (0x200, 0x204)]
        index = analyze._SpanIndex(ranges)
        for addr in (0xFF, 0x100, 0x10F, 0x110, 0x200, 0x204):
            self.assertEqual(analyze.in_ranges(addr, index), analyze.in_ranges(addr, ranges))


if __name__ == "__main__":
    unittest.main()
