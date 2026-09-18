#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""TD-27: opt-in stale translated-code tracking (detection + invalidation half).

Layer 1 (behavioral): compile and run ``src/rt/stale_code_selftest.c``
against the real ``src/rt/stale_code.c`` twice -- gate off (an overwritten
invalidate must stay silent and the dispatch-side query must stay clean) and
SR_STALE_DETECT=1 (an overwritten translated block must fire loudly, set the
per-entry stale flag, and clear it once the bytes are restored; pristine
invalidates must stay silent) -- plus source mutations of the word
comparison, the flag-set, and the query that must each kill the gate-on run,
proving the assertions are load-bearing rather than vacuous.

Layer 2 (wiring/consistency): source checks that the gate is off by default
and off the hot path when disabled (cached flag, early return before any
guest-memory touch), that a firing check names the block address, the first
differing word and expected vs actual before aborting (the SR_BREAK_FATAL /
sr_unimplemented fail-loud convention), that the HLE cache hooks preserve
default dispatch behavior (Icache NIDs stay unregistered unless opted in),
that codegen emits nothing stale-related by default, and that the Makefile
wires the new objects and selftest like the existing native selftests. The
dispatch redirect itself is wired (see src/rt/stale_code.h) and proven by
the dispatch-isolation selftest's synthetic self-modifying program, which
runs the NEW bytes through the interpreter while stale and returns to the
AOT body once restored.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codegen

RT = ROOT / "src" / "rt"
STALE_H = (RT / "stale_code.h").read_text(encoding="utf-8")
STALE_C = (RT / "stale_code.c").read_text(encoding="utf-8")
SELFTEST_C = RT / "stale_code_selftest.c"
SELFTEST_TEXT = SELFTEST_C.read_text(encoding="utf-8")
HLE_C = (RT / "hle.c").read_text(encoding="utf-8")
RECOMP_H = (RT / "recomp.h").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

WORD_CMP_ANCHOR = "if (actual != entry->expected) {"
FLAG_SET_ANCHOR = "->stale = 1;"
FLAG_CLEAR_ANCHOR = "entry->stale = 0;"


def _scrubbed_env():
    return {k: v for k, v in os.environ.items() if k != "SR_STALE_DETECT"}


def _enabled_env():
    env = _scrubbed_env()
    env["SR_STALE_DETECT"] = "1"
    return env


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestStaleCodeSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="stalecode_")
        cls.exe = os.path.join(cls.tmp, "stale_code_selftest.exe")
        result = subprocess.run(
            [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{RT}", "-o", cls.exe,
             str(SELFTEST_C), str(RT / "stale_code.c")],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("stale_code selftest did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_gate_off_overwrite_stays_silent(self):
        result = subprocess.run([self.exe], capture_output=True, text=True,
                                env=_scrubbed_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("stale_code selftest: OK (gate off, silent)", result.stdout)
        self.assertNotIn("STALE_CODE_DETECT", result.stderr)

    def test_gate_on_fires_loudly_with_addresses(self):
        result = subprocess.run([self.exe], capture_output=True, text=True,
                                env=_enabled_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("stale_code selftest: OK", result.stdout)
        self.assertIn("STALE_CODE_DETECT", result.stderr)
        self.assertIn("block=0x08800004", result.stderr)
        self.assertIn("expected=0x22222222", result.stderr)
        self.assertIn("actual=0xdeadbeef", result.stderr)
        self.assertIn("expected_hash=0x12a02059", result.stderr)

    def test_word_comparison_mutation_kills_both_cases(self):
        """Failing-before proof: flipping the word comparison must break the
        gate-on run (pristine ranges fire, overwritten ranges go silent), so
        the selftest cannot pass against a neutered detector."""
        self.assertEqual(STALE_C.count(WORD_CMP_ANCHOR), 2,
                         "mutation anchor drifted; the mutant below would be vacuous")
        with tempfile.TemporaryDirectory(prefix="stalecode_mut_") as tmp_dir:
            mutated = STALE_C.replace(WORD_CMP_ANCHOR, "if (actual == entry->expected) {")
            Path(tmp_dir, "stale_code.c").write_text(mutated, encoding="utf-8")
            Path(tmp_dir, "stale_code.h").write_text(STALE_H, encoding="utf-8")
            shutil.copy(SELFTEST_C, Path(tmp_dir, "stale_code_selftest.c"))
            exe = os.path.join(tmp_dir, "mut_selftest.exe")
            comp = subprocess.run(
                [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 f"-I{tmp_dir}", "-o", exe,
                 os.path.join(tmp_dir, "stale_code_selftest.c"),
                 os.path.join(tmp_dir, "stale_code.c")],
                capture_output=True, text=True,
            )
            self.assertEqual(comp.returncode, 0, "mutant did not compile:\n" + comp.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, env=_enabled_env())
            self.assertNotEqual(run.returncode, 0,
                                "neutered detector still passed; the selftest is vacuous")

    def test_invalidation_half_self_modifying_case(self):
        """The gate-on run must exercise the modeled tier branch: the
        self-modifying block's interpreted result follows the new bytes while
        the stale AOT value still names the old ones, and restoring the bytes
        returns to the AOT path. Assert the probes exist so a deleted case
        cannot silently shrink this test to the detector matrix."""
        for probe in ("test_invalidation_half", "sr_stale_block_is_stale",
                      "interpreted result must differ from stale AOT",
                      "restored dispatch must return to the AOT value",
                      "re-registration must clear the flag"):
            self.assertIn(probe, SELFTEST_TEXT)
        result = subprocess.run([self.exe], capture_output=True, text=True,
                                env=_enabled_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("stale_code selftest: OK", result.stdout)
        # The modeled case fires at least one check (word granularity names
        # the overwritten word) and the query-driven branch keeps running.
        self.assertIn("block=0x08800040", result.stderr)

    def test_flag_set_mutation_kills_gate_on(self):
        """Failing-before proof for the flag (not the return code) driving
        the query: neutering every flag-set keeps the firing return values
        intact, so only the invalidation-half assertions must fail."""
        self.assertEqual(STALE_C.count(FLAG_SET_ANCHOR), 3,
                         "flag-set anchor drifted; the mutant below would be vacuous")
        with tempfile.TemporaryDirectory(prefix="stalecode_flagmut_") as tmp_dir:
            mutated = STALE_C.replace(FLAG_SET_ANCHOR, "->stale = 0;")
            Path(tmp_dir, "stale_code.c").write_text(mutated, encoding="utf-8")
            Path(tmp_dir, "stale_code.h").write_text(STALE_H, encoding="utf-8")
            shutil.copy(SELFTEST_C, Path(tmp_dir, "stale_code_selftest.c"))
            exe = os.path.join(tmp_dir, "mut_selftest.exe")
            comp = subprocess.run(
                [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 f"-I{tmp_dir}", "-o", exe,
                 os.path.join(tmp_dir, "stale_code_selftest.c"),
                 os.path.join(tmp_dir, "stale_code.c")],
                capture_output=True, text=True,
            )
            self.assertEqual(comp.returncode, 0, "mutant did not compile:\n" + comp.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, env=_enabled_env())
            self.assertNotEqual(run.returncode, 0,
                                "flag-neutered tracker still passed; the query is vacuous")

    def test_query_kill_mutation_breaks_both_gates(self):
        """Failing-before proof for the query: forcing it to always report
        stale must break the gate-off run (which requires the AOT path) and
        the gate-on run (which requires clean-when-pristine)."""
        body = STALE_C.split("int sr_stale_block_is_stale(uint32_t addr) {", 1)
        self.assertEqual(len(body), 2, "query body anchor drifted")
        head, tail = body
        query = tail.split("\n}\n", 1)[0]
        self.assertIn("return 1;", query, "query must have a firing return to neuter")
        mutated = head + "int sr_stale_block_is_stale(uint32_t addr) {\n    (void)addr;\n    return 1;\n}\n" + tail.split("\n}\n", 1)[1]
        with tempfile.TemporaryDirectory(prefix="stalecode_qmut_") as tmp_dir:
            Path(tmp_dir, "stale_code.c").write_text(mutated, encoding="utf-8")
            Path(tmp_dir, "stale_code.h").write_text(STALE_H, encoding="utf-8")
            shutil.copy(SELFTEST_C, Path(tmp_dir, "stale_code_selftest.c"))
            exe = os.path.join(tmp_dir, "mut_selftest.exe")
            comp = subprocess.run(
                [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 f"-I{tmp_dir}", "-o", exe,
                 os.path.join(tmp_dir, "stale_code_selftest.c"),
                 os.path.join(tmp_dir, "stale_code.c")],
                capture_output=True, text=True,
            )
            self.assertEqual(comp.returncode, 0, "mutant did not compile:\n" + comp.stderr)
            for env, name in ((_scrubbed_env(), "gate off"), (_enabled_env(), "gate on")):
                run = subprocess.run([exe], capture_output=True, text=True, env=env)
                self.assertNotEqual(run.returncode, 0,
                                    f"always-stale query still passed {name}; "
                                    "the tier branch is vacuous")


class TestStaleCodeWiring(unittest.TestCase):
    def test_gate_is_cached_and_off_by_default(self):
        self.assertIn('getenv(SR_STALE_DETECT_ENV)', STALE_C)
        self.assertIn('static int s_enabled = -1', STALE_C)
        self.assertIn('"0") != 0', STALE_C)

    def test_checks_return_before_touching_memory_when_disabled(self):
        for fn in ("sr_stale_check_range", "sr_stale_check_all"):
            body = STALE_C.split(f"{fn}(", 1)[1].split("\n}\n", 1)[0]
            first_stmt = body.split("{", 1)[1]
            self.assertIn("if (!sr_stale_enabled())", first_stmt.split("if (!read", 1)[0],
                          f"{fn} must consult the gate before any guest-memory touch")

    def test_firing_check_names_block_word_and_values_then_aborts(self):
        self.assertIn("STALE_CODE_DETECT block=0x%08x off=0x%08x expected=0x%08x actual=0x%08x", STALE_C)
        self.assertIn("STALE_CODE_DETECT block=0x%08x nwords=%u expected_hash=0x%08x actual_hash=0x%08x", STALE_C)
        for handler in ("h_CacheInvalidateAll", "h_CacheInvalidateRange", "sr_stale_note_cache_op"):
            self.assertIn(handler, HLE_C)
        self.assertIn("abort();", HLE_C)

    def test_stale_flags_are_per_entry_and_cleared_on_match(self):
        self.assertIn("int sr_stale_block_is_stale(uint32_t addr);", STALE_H)
        for type_name in ("SrStaleWordEntry", "SrStaleBlockEntry"):
            body = STALE_C.split(f"}} {type_name};", 1)[0].rsplit("typedef struct {", 1)[-1]
            self.assertIn("int stale;", body, f"{type_name} must carry a stale flag")
        # Registration installs a fresh expectation: creating and replacing
        # a record both clear its flag.
        self.assertEqual(STALE_C.count("stale = 0;"), 7,
                         "flag-clear anchor drifted")
        # Every examined record updates its flag; an unreadable block keeps
        # its flag (the skip path below touches no stale field).
        mark = STALE_C.split("static int mark_block(", 1)[1].split("SrStaleFire fire;", 1)[0]
        skip = mark.split("if (!read(waddr, &w, ctx)) {", 1)[1].split("}", 1)[0]
        self.assertNotIn("stale", skip)

    def test_query_consults_gate_before_tables(self):
        body = STALE_C.split("int sr_stale_block_is_stale(uint32_t addr) {", 1)[1]
        body = body.split("\n}\n", 1)[0]
        gate_pos = body.find("if (!sr_stale_enabled())")
        self.assertNotEqual(gate_pos, -1, "query must consult the gate")
        for table in ("s_words", "s_blocks"):
            self.assertGreater(body.find(table), gate_pos,
                               f"query must touch {table} only behind the gate")

    def test_dcache_handlers_preserve_success_results(self):
        self.assertIn('sr_hle_register(0x79d1c3fa, "sceKernelDcacheWritebackAll", h_CacheInvalidateAll)', HLE_C)
        self.assertIn('sr_hle_register(0xb435dec5, "sceKernelDcacheWritebackInvalidateAll", h_CacheInvalidateAll)', HLE_C)
        self.assertIn('sr_hle_register(0x3ee30821, "sceKernelDcacheWritebackRange", h_CacheInvalidateRange)', HLE_C)

    def test_icache_nids_register_only_when_opted_in(self):
        low = HLE_C.lower()
        first_reg = low.find('sr_hle_register(0x920f104a')
        self.assertNotEqual(first_reg, -1, "Icache registration missing")
        gate = low.rfind("if (sr_stale_enabled()) {", 0, first_reg)
        self.assertNotEqual(gate, -1, "no opt-in gate precedes the Icache registrations")
        between = HLE_C[gate:first_reg]
        self.assertNotIn("\n    }", between, "the opt-in gate must still be open")
        self.assertIn("void sr_hle_init(void)", HLE_C[:gate])
        self.assertIn("sceKernelDcacheWritebackRange", HLE_C[gate - 2000:gate])
        for nid, name in ((0x920F104A, "sceKernelIcacheInvalidateAll"),
                          (0xD8779AC6, "sceKernelIcacheClearAll"),
                          (0xC2DF770E, "sceKernelIcacheInvalidateRange"),
                          (0xBFA98062, "sceKernelDcacheInvalidateRange"),
                          (0x34B9FA9E, "sceKernelDcacheWritebackInvalidateRange")):
            self.assertIn(f"0x{nid:08x}", low[gate:gate + 2000],
                          f"{name} must register under the opt-in gate")
            self.assertEqual(low.count(f"0x{nid:08x}"), 1,
                             f"{name} NID must appear exactly once (no duplicate registration)")
            self.assertNotIn(f"0x{nid:08x}", low[:gate],
                             f"{name} NID must not appear before the opt-in gate")

    def test_lookup_stays_structural_with_pointer_to_detector(self):
        self.assertIn('#include "stale_code.h"', RECOMP_H)
        self.assertIn("stale_code.h", RECOMP_H.split("sr_lookup(uint32_t addr);", 1)[0].rsplit("/*", 1)[-1])

    def test_makefile_wires_objects_and_selftest(self):
        for var in ("RT_SRCS", "PORTABLE_CORE_SRCS"):
            block = re.search(rf"^{var}\s*:?=\s*(.*?)(?=\n\S|\Z)",
                              MAKEFILE, re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(block, var)
            self.assertIn("src/rt/stale_code.c", block.group(1))
        for recipe in ("hle-thread-selftest-build:", "hle-title-selftest-one:"):
            body = MAKEFILE.split(recipe, 1)[1].split("\n\n", 1)[0]
            self.assertIn("src/rt/stale_code.c", body)
        self.assertIn("stale-code-selftest:", MAKEFILE)
        self.assertIn("src/rt/stale_code_selftest.c", MAKEFILE)
        self.assertIn("SR_STALE_DETECT", MAKEFILE)
        block = re.search(r"(?ms)^PUBLIC_TARGETS := \\\n(.*?)(?=^INTERNAL_TARGETS :=)", MAKEFILE)
        self.assertIsNotNone(block)
        self.assertIn("stale-code-selftest", block.group(1).split())


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


class TestStaleCodegenGate(unittest.TestCase):
    def test_flag_defaults_off(self):
        self.assertFalse(codegen.STALE_DETECT)

    def test_cache_op_is_noop_by_default_and_hook_when_enabled(self):
        cache_word = (0x2F << 26) | (4 << 21) | (2 << 16) | 0x10
        stmt, _, _ = codegen.effect(0x1000, cache_word)
        self.assertEqual(stmt, "(void)0;")
        old = codegen.STALE_DETECT
        codegen.STALE_DETECT = True
        try:
            stmt, _, _ = codegen.effect(0x1000, cache_word)
            self.assertIn("sr_stale_note_cache_op", stmt)
        finally:
            codegen.STALE_DETECT = old

    def test_fnv_vectors_match_runtime(self):
        self.assertEqual(codegen.stale_fnv1a([]), 0x811C9DC5)
        self.assertEqual(codegen.stale_fnv1a([0x00000000]), 0x4B95F515)
        self.assertEqual(codegen.stale_fnv1a([0x00000061]), 0xF5E1D3E4)
        self.assertEqual(codegen.stale_fnv1a([0x11111111, 0x22222222]), 0x12A02059)
        self.assertEqual(codegen.stale_fnv1a([0x24020001, 0x03E00008, 0x00000000]), 0x879F43E7)

    def test_block_record_covers_translated_head_run(self):
        elf = FakeElf({0x1000: 0x24020001, 0x1004: 0x03E00008})
        rec = codegen.stale_block_for_function(elf, 0x1000, [(0x1000, 0x1010)], {0x1000})
        self.assertIsNotNone(rec)
        entry, nwords, digest = rec
        self.assertEqual(entry, 0x24020001)
        self.assertEqual(nwords, 2)
        self.assertEqual(digest, codegen.stale_fnv1a([0x24020001, 0x03E00008]))

    def test_production_fixture_recipes_do_not_opt_in(self):
        for target in ("production-smoke:", "cosim-selftest:"):
            recipe = MAKEFILE.split(target, 1)[1].split("\n\n", 1)[0]
            self.assertNotIn("--stale-detect", recipe)


class TestStaleCodegenEmission(unittest.TestCase):
    """The --stale-detect table is emitted end to end from a synthetic ELF,
    while default output stays free of stale records and deterministic."""

    @staticmethod
    def _write_min_elf(path):
        eh = struct.pack("<16sHHIIIIIHHHHHH",
                         bytes([0x7F]) + b"ELF" + bytes([1, 1, 1]) + bytes(9),
                         2, 8, 1, 0x1000, 52, 0, 0, 52, 32, 1, 0, 0, 0)
        ph = struct.pack("<IIIIIIII", 1, 84, 0x1000, 0x1000, 8, 8, 5, 0x1000)
        code = struct.pack("<II", 0x03E00008, 0x00000000)  # jr ra; nop
        Path(path).write_bytes(eh + ph + code)

    def _run_codegen(self, tmp_dir, stale):
        old = codegen.STALE_DETECT
        codegen.STALE_DETECT = False
        try:
            elf = str(Path(tmp_dir) / "min.elf")
            out = str(Path(tmp_dir) / "min_recomp.c")
            self._write_min_elf(elf)
            args = ["codegen.py", elf, out, "--base=0x0"]
            if stale:
                args.append("--stale-detect")
            self.assertEqual(codegen.main(args), 0)
            return Path(out).read_text(encoding="ascii")
        finally:
            codegen.STALE_DETECT = old

    def test_default_output_has_no_stale_records(self):
        with tempfile.TemporaryDirectory(prefix="staleemit_") as tmp_dir:
            text = self._run_codegen(tmp_dir, False)
            self.assertNotIn("stale", text)
            self.assertNotIn("sr_stale_", text)

    def test_stale_flag_emits_word_and_block_records(self):
        with tempfile.TemporaryDirectory(prefix="staleemit_") as tmp_dir:
            text = self._run_codegen(tmp_dir, True)
            self.assertIn("sr_stale_reset();", text)
            self.assertIn("sr_stale_register_word(0x00001000u, 0x03e00008u);", text)
            digest = codegen.stale_fnv1a([0x03E00008, 0x00000000])
            self.assertIn(f"sr_stale_register_block(0x00001000u, 2u, 0x{digest:08x}u);", text)

    def test_default_output_is_deterministic(self):
        with tempfile.TemporaryDirectory(prefix="staleemit_") as tmp_dir:
            first = self._run_codegen(tmp_dir, False)
            second = self._run_codegen(tmp_dir, False)
            self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
