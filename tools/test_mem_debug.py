#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""Unit tests for tools/mem_debug.py (issue #180).

These tests exercise the pure helpers (guest-span validation, region
classification, host-offset mapping, PE image identity, process-candidate
selection, argument parsing) and the mutation gate + simulation behavior of
MemoryDebugger.  No test attaches to, reads, writes, or suspends a real
process: live-process access is never exercised here.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mem_debug as md


class ExecutablePathResolutionTest(unittest.TestCase):
    """The attached executable is resolved before any tool is spawned (#294)."""

    def test_nm_receives_an_absolute_executable_path(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_exe_") as tmp_dir:
            exe_dir = Path(tmp_dir)
            (exe_dir / "game.exe").write_bytes(b"MZ-stub")
            captured = {}

            def fake_run(argv, **kwargs):
                argv = [str(arg) for arg in argv]
                if "--version" in argv:
                    return subprocess.CompletedProcess(argv, 0, stdout="nm", stderr="")
                captured["argv"] = argv
                return subprocess.CompletedProcess(argv, 0, stdout="", stderr="")

            previous_cwd = os.getcwd()
            try:
                os.chdir(exe_dir)
                with mock.patch.dict(os.environ, {"NM": "nm-fake"}):
                    with mock.patch.object(md.subprocess, "run", side_effect=fake_run):
                        _rvas, provenance = md.get_symbol_rvas("game.exe")
            finally:
                os.chdir(previous_cwd)
            self.assertEqual(provenance, "fallback")  # empty nm output
            self.assertEqual(captured["argv"][0], "nm-fake")
            self.assertTrue(os.path.isabs(captured["argv"][1]))
            self.assertEqual(
                os.path.normpath(captured["argv"][1]),
                os.path.normpath(str(exe_dir / "game.exe")),
            )


class GuestSpanValidationTest(unittest.TestCase):
    def test_valid_ram_span(self):
        ok, region = md.guest_span_validate(0x08800000, 16, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "ram")

    def test_exact_arena_end_is_valid(self):
        # Last 4 bytes of RAM: [0x0BFFFFFFC, 0x0C000000) is fully inside.
        ok, _ = md.guest_span_validate(0x0BFFFFFC, 4, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        # One byte past the arena end must fail.
        ok, _ = md.guest_span_validate(0x0BFFFFFD, 4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        ok, _ = md.guest_span_validate(0x0C000000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_below_ram_base_is_unsupported(self):
        # 0x08000000 is the RAM base; anything below is unmapped arena.
        ok, reason = md.guest_span_validate(0x07FFFFFF, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("unsupported", reason)

    def test_vram_span(self):
        ok, region = md.guest_span_validate(0x04000000, 0x00200000, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "vram")
        # Past the 2 MiB VRAM window -> unmapped arena.
        ok, _ = md.guest_span_validate(0x04200000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_scratchpad_span(self):
        ok, region = md.guest_span_validate(0x00010000, 0x1000, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "scratchpad")
        ok, _ = md.guest_span_validate(0x00011000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_wraparound_guest_address(self):
        # 0xFFFFFFFF aliases to phys 0x1FFFFFFF, outside the arena.
        ok, reason = md.guest_span_validate(0xFFFFFFFF, 4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("outside", reason)

    def test_oversized_span_rejected(self):
        ok, reason = md.guest_span_validate(0x08800000, md.MAX_READ_BYTES + 1,
                                            md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("budget", reason)
        ok, _ = md.guest_span_validate(0x08800000, 1, md.MAX_READ_BYTES)
        self.assertTrue(ok)

    def test_zero_and_negative_sizes_rejected(self):
        ok, reason = md.guest_span_validate(0x08800000, 0, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("zero-size", reason)
        ok, reason = md.guest_span_validate(0x08800000, -4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("negative", reason)

    def test_non_integer_size_rejected(self):
        ok, _ = md.guest_span_validate(0x08800000, True, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        ok, _ = md.guest_span_validate(0x08800000, "16", md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_region_boundary_straddle_rejected(self):
        # Span starting inside VRAM (0x041FFFFC) and crossing past its 2 MiB
        # window into unmapped arena must fail.
        ok, reason = md.guest_span_validate(0x041FFFFC, 8, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("boundary", reason)
        # A span starting in unmapped arena is rejected outright.
        ok, reason = md.guest_span_validate(0x07FFFFFC, 8, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("unsupported", reason)

    def test_supported_regions_disjoint(self):
        self.assertNotEqual(md.guest_region(0x08800000), md.guest_region(0x04000000))
        self.assertEqual(md.guest_region(0x08800000), "ram")
        self.assertEqual(md.guest_region(0x04000000), "vram")
        self.assertEqual(md.guest_region(0x00010000), "scratchpad")
        self.assertEqual(md.guest_region(0x00000000), "unmapped-arena")
        self.assertEqual(md.guest_region(0x2C000000), "out-of-arena")


class HostOffsetTest(unittest.TestCase):
    def test_host_offset_matches_sr_host(self):
        # SR_HOST(a) == g_mem + (SR_PHYS(a) - SR_RAM_BASE)
        g_mem = 0x0000000141234000  # g_mem points at guest RAM base
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x08800000),
                         g_mem + 0x00800000)
        # VRAM (0x04000000) maps BELOW g_mem, matching the runtime arena.
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x04000000),
                         g_mem - 0x04000000)

    def test_aliased_guest_high_bits_masked(self):
        g_mem = 0x1000
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x88800000),
                         md.host_offset_for_guest(g_mem, 0x08800000))


class PEImageIdentityTest(unittest.TestCase):
    def test_identity_matches(self):
        self.assertTrue(md.image_identity_matches(b"abc", b"abc"))
        self.assertFalse(md.image_identity_matches(b"abc", b"abd"))
        self.assertFalse(md.image_identity_matches(b"abc", b"abcd"))
        self.assertFalse(md.image_identity_matches(None, b"abc"))
        self.assertFalse(md.image_identity_matches(b"abc", None))

    def test_sha256_file(self):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
            f.write(b"hello world")
            path = f.name
        try:
            digest = md.sha256_file(path)
            self.assertEqual(
                digest,
                "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9")
            self.assertIsNone(md.sha256_file(os.path.join(path, "missing")))
        finally:
            os.unlink(path)

    def test_pe_prefix_bytes(self):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".exe") as f:
            f.write(b"X" * 0x2000)
            path = f.name
        try:
            prefix = md.pe_prefix_bytes(path)
            self.assertEqual(len(prefix), md.PE_PREFIX_BYTES)
            self.assertEqual(md.pe_prefix_bytes(os.path.join(path, "missing")), None)
        finally:
            os.unlink(path)


class ProcessCandidateSelectionTest(unittest.TestCase):
    def test_unique_exact_path(self):
        candidates = [
            {"pid": 100, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0x140000000},
            {"pid": 200, "exe_path": r"C:\other\not_hst.exe", "base_address": 0x140000000},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNotNone(chosen)
        self.assertEqual(chosen["pid"], 100)
        self.assertEqual(reason, "unique-exact-path")

    def test_case_and_slash_normalization(self):
        # Windows-normalize case-insensitive exe names and both slash styles;
        # POSIX paths are case-sensitive and forward-slash by convention.
        candidates = [
            {"pid": 300, "exe_path": "c:/repo/build/hst/HST.EXE", "base_address": 0},
        ]
        expected = r"C:\repo\build\hst\hst.exe"
        if os.name == "nt":
            chosen, _ = md.select_process_candidate(candidates, expected)
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 300)
        else:
            # POSIX: backslash is an ordinary filename character; a Windows-
            # style expected path cannot match a POSIX candidate.  Instead
            # assert equivalent POSIX spellings match exactly.
            posix_candidates = [
                {"pid": 301, "exe_path": "build/hst/hst.exe", "base_address": 0},
            ]
            chosen, _ = md.select_process_candidate(posix_candidates, "build/hst/hst.exe")
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 301)
            chosen, _ = md.select_process_candidate(posix_candidates, "./build/hst/hst.exe")
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 301)

    def test_ambiguous_exact_matches(self):
        candidates = [
            {"pid": 100, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0},
            {"pid": 101, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("ambiguous", reason)

    def test_name_only_match_not_selected(self):
        # A process named hst.exe elsewhere must NOT be auto-selected.
        candidates = [
            {"pid": 400, "exe_path": r"C:\elsewhere\hst.exe", "base_address": 0},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("no process matches", reason)

    def test_empty_candidates(self):
        chosen, reason = md.select_process_candidate([], r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("no process matches", reason)

    def test_missing_expected_path(self):
        chosen, reason = md.select_process_candidate([], None)
        self.assertIsNone(chosen)
        self.assertIn("no expected", reason)


class ParseUint32Test(unittest.TestCase):
    def test_valid_forms(self):
        self.assertEqual(md.parse_uint32_arg("0"), 0)
        self.assertEqual(md.parse_uint32_arg("0x0"), 0)
        self.assertEqual(md.parse_uint32_arg("0xFFFFFFFF"), 0xFFFFFFFF)
        self.assertEqual(md.parse_uint32_arg("4294967295"), 0xFFFFFFFF)
        self.assertEqual(md.parse_uint32_arg("0x08800000"), 0x08800000)

    def test_rejects_junk_and_out_of_range(self):
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x100junk")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("123junk")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("-1")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x100000000")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("4294967296")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("3.5")


class MutationGateTest(unittest.TestCase):
    """The fail-closed mutation gate, exercised without any live process."""

    def _dbg(self, **overrides):
        # simulate=True keeps the constructor away from live processes; the
        # gate tests then override the attributes the gate reads.
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        attrs = {
            "is_simulated": False,
            "is_offline": False,
            "candidate": {"pid": 1, "exe_path": "x", "base_address": 0x140000000},
            "image_verified": True,
            "rva_provenance": "nm",
            "mutate_enabled": True,
        }
        attrs.update(overrides)
        for k, v in attrs.items():
            setattr(dbg, k, v)
        return dbg

    def test_mutation_requires_flag(self):
        dbg = self._dbg(mutate_enabled=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("--mutate", reason)

    def test_mutation_requires_identified_process(self):
        dbg = self._dbg(candidate=None, is_offline=True)
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)

    def test_mutation_requires_verified_image(self):
        dbg = self._dbg(image_verified=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("image identity", reason)

    def test_mutation_refused_on_fallback_rvas(self):
        dbg = self._dbg(rva_provenance="fallback")
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("fallback", reason)

    def test_mutation_allowed_when_all_verified(self):
        dbg = self._dbg()
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertTrue(ok)

    def test_non_mutating_action_rejected_by_gate(self):
        dbg = self._dbg()
        ok, _ = dbg._mutation_allowed("read_mem", needs_rvas=False, needs_image=False)
        self.assertFalse(ok)

    def test_simulation_never_requires_live_identity(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertTrue(ok)

    def test_simulation_without_mutate_still_refused(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("--mutate", reason)


class SimulationBehaviorTest(unittest.TestCase):
    def setUp(self):
        # Point the mock state at a temp file so tests never touch the repo's
        # real build/hst/mock_debug_state.json.
        fd, self.tmp_path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.unlink(self.tmp_path)

    def tearDown(self):
        if os.path.exists(self.tmp_path):
            os.unlink(self.tmp_path)

    def _sim(self, mutate=False):
        orig = md.get_mock_state_path
        md.get_mock_state_path = lambda: self.tmp_path
        try:
            dbg = md.MemoryDebugger(simulate=True, mutate=mutate)
        finally:
            md.get_mock_state_path = orig
        return dbg

    def test_read_mem_valid_span(self):
        dbg = self._sim()
        res = dbg.read_mem(0x08800000, 4, "hex")
        self.assertIn("hex", res)
        self.assertEqual(res["size"], 4)

    def test_read_mem_invalid_span_rejected(self):
        dbg = self._sim()
        res = dbg.read_mem(0x0C000000, 4, "hex")
        self.assertIn("error", res)
        self.assertIn("guest span rejected", res["error"])

    def test_write_mem_requires_mutate_even_in_simulation(self):
        dbg = self._sim(mutate=False)
        res = dbg.write_mem(0x08800000, "deadbeef")
        self.assertFalse(res["success"])
        self.assertIn("--mutate", res["error"])

    def test_write_mem_round_trip_in_simulation(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "deadbeef")
        self.assertTrue(res["success"])
        self.assertEqual(res["bytes_written"], 4)
        rd = dbg.read_mem(0x08800000, 4, "hex")
        self.assertEqual(rd["hex"].replace(" ", ""), "deadbeef")

    def test_write_mem_zero_bytes_refused(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "")
        self.assertFalse(res["success"])
        self.assertIn("zero-byte", res["error"])

    def test_write_mem_invalid_hex_refused(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "zzzz")
        self.assertFalse(res["success"])
        self.assertIn("Invalid hex", res["error"])

    def test_write_mem_invalid_span_rejected(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x0BFFFFFE, "0102030405")  # crosses arena end
        self.assertFalse(res["success"])
        self.assertIn("guest span rejected", res["error"])

    def test_pause_resume_require_mutate(self):
        dbg = self._sim(mutate=False)
        self.assertFalse(dbg.pause()["success"])
        self.assertFalse(dbg.resume()["success"])

    def test_status_reports_mutation_and_provenance(self):
        dbg = self._sim(mutate=True)
        status = dbg.get_status()
        self.assertEqual(status["mode"], "simulation")
        self.assertEqual(status["mutation"], "enabled")
        self.assertEqual(status["rva_provenance"], "fallback")

    def test_simulated_cpu_uses_cpustate_abi_v2_tail(self):
        dbg = self._sim(mutate=True)
        cpu = dbg.read_cpu()["cpu"]
        self.assertEqual(len(cpu["cop0"]), 32)
        self.assertIn("flow_kind", cpu)
        self.assertIn("flow_target", cpu)

        self.assertEqual(md._cpu_field_offset("cop0[0]"), (852, False))
        self.assertEqual(md._cpu_field_offset("cop0[12]"), (900, False))
        self.assertEqual(md._cpu_field_offset("next_pc"), (980, False))
        self.assertEqual(md._cpu_field_offset("in_delay_slot"), (984, False))
        self.assertEqual(md._cpu_field_offset("flow_kind"), (988, False))
        self.assertEqual(md._cpu_field_offset("flow_target"), (992, False))


class ResolverFailureTest(unittest.TestCase):
    def test_guest_memory_resolver_rejects_bad_span(self):
        # No live process is needed: span validation runs before any g_mem read.
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        dbg.is_simulated = False
        host, err = dbg._guest_memory_resolver(0x0C000000, 4, md.MAX_READ_BYTES)
        self.assertIsNone(host)
        self.assertIn("outside", err)

    def test_guest_memory_resolver_missing_g_mem(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        dbg.is_simulated = False
        dbg.pid = 0  # no process handle can be opened
        host, err = dbg._guest_memory_resolver(0x08800000, 4, md.MAX_READ_BYTES)
        self.assertIsNone(host)
        self.assertIn("g_mem", err)


class CpuStateAbiUpgradeTest(unittest.TestCase):
    """ABI v2 must never be applied to state produced by a pre-v2 build."""

    def _header(self, abi=md.CPU_STATE_ABI_VERSION, size=md.CPU_STATE_SIZE,
                stack_len=8, fmt=md.CRASH_DUMP_FORMAT):
        words = (fmt, abi, size, 0x09FF0000, stack_len)
        return md.CRASH_DUMP_MAGIC + b"".join(w.to_bytes(4, "little") for w in words)

    def test_versioned_dump_is_split_at_the_header_sizes(self):
        cpu = bytes(range(256)) * 3 + bytes(md.CPU_STATE_SIZE - 768)
        blob = self._header() + cpu + b""
        parsed, err = md.parse_crash_dump(blob)
        self.assertIsNone(err)
        cpu_bytes, base, stack = parsed
        self.assertEqual(cpu_bytes, cpu)
        self.assertEqual(base, 0x09FF0000)
        self.assertEqual(stack, b"")

    def test_headerless_v1_dump_is_rejected(self):
        # ABI v1: 864-byte CpuState followed by the 64 KiB stack snapshot.
        parsed, err = md.parse_crash_dump(bytes(864 + 0x10000))
        self.assertIsNone(parsed)
        self.assertIn("pre-ABI-v2", err)

    def test_other_abi_or_size_is_rejected(self):
        for header in (self._header(abi=1), self._header(size=864)):
            parsed, err = md.parse_crash_dump(header + bytes(md.CPU_STATE_SIZE + 8))
            self.assertIsNone(parsed)
            self.assertIn("ABI", err)

    def test_truncated_body_is_rejected(self):
        parsed, err = md.parse_crash_dump(self._header() + bytes(md.CPU_STATE_SIZE))
        self.assertIsNone(parsed)
        self.assertIn("size", err)

    def test_pre_v2_mock_state_is_migrated(self):
        old = {"status": "running", "memory": {},
               "cpu": {"r": [0] * 32, "pc": 0x08804000, "cop0": [7]}}
        mock = md._migrate_mock_state(old)
        self.assertEqual(len(mock["cpu"]["cop0"]), md.CPU_STATE_COP0_COUNT)
        self.assertEqual(mock["cpu"]["cop0"][0], 7)
        self.assertEqual(mock["cpu"]["next_pc"], 0x08804004)
        for field in ("in_delay_slot", "flow_kind", "flow_target"):
            self.assertEqual(mock["cpu"][field], 0)

    def test_migrated_mock_accepts_new_cop0_writes(self):
        fd, path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write('{"status": "running", "memory": {}, "cpu": {"r": [0], "pc": 0}}')
            orig = md.get_mock_state_path
            md.get_mock_state_path = lambda: path
            try:
                dbg = md.MemoryDebugger(simulate=True, mutate=True)
                res = dbg.write_cpu("cop0[12]", 0x10)
            finally:
                md.get_mock_state_path = orig
            self.assertTrue(res["success"], res)
            self.assertEqual(dbg.mock["cpu"]["cop0"][12], 0x10)
        finally:
            os.unlink(path)

    def _live(self, **overrides):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        attrs = {
            "is_simulated": False,
            "is_offline": False,
            "candidate": {"pid": 1, "exe_path": "x", "base_address": 0x140000000},
            "base_address": 0x140000000,
            "image_verified": True,
            "rva_provenance": "nm",
            "rvas": {"g_mem": 1, "s_cpu": 2},
        }
        attrs.update(overrides)
        for k, v in attrs.items():
            setattr(dbg, k, v)
        return dbg

    def test_live_cpu_access_requires_exported_abi_marker(self):
        dbg = self._live()
        for res in (dbg.read_cpu(), dbg.write_cpu("cop0[12]", 1)):
            self.assertFalse(res["success"])
            self.assertIn(md.ABI_SYMBOL, res["error"])

    def test_live_cpu_access_rejects_other_abi_version(self):
        dbg = self._live(rvas={"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3})
        dbg._read_process_bytes = lambda addr, size: (1).to_bytes(4, "little")
        res = dbg.write_cpu("cop0[12]", 1)
        self.assertFalse(res["success"])
        self.assertIn("ABI 1", res["error"])

    def test_live_cpu_abi_check_passes_for_v2(self):
        dbg = self._live(rvas={"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3})
        dbg._read_process_bytes = lambda addr, size: (2).to_bytes(4, "little")
        self.assertIsNone(dbg._cpu_abi_check())


class ExeOptionAndDefaultRejectionTest(unittest.TestCase):
    def test_default_without_exe_fails_clearly(self):
        """MemoryDebugger without --simulate or explicit exe must fail clearly instead of assuming HST."""
        with self.assertRaises(ValueError) as ctx:
            md.MemoryDebugger(simulate=False)
        self.assertIn("No target executable", str(ctx.exception))
        self.assertIn("HST", str(ctx.exception))

    def test_explicit_exe_sets_target_identity(self):
        """Passing an explicit exe sets expected_exe and expected_exe_name rather than assuming hst.exe."""
        dbg = md.MemoryDebugger(simulate=True, exe=r"build\custom_title\custom_title.exe")
        self.assertEqual(dbg.expected_exe_name, "custom_title.exe")


if __name__ == "__main__":
    unittest.main()
