# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""#181 evidence-integrity regressions for tools/progress_tracker.py.

Each test supplies a synthetic fixture that used to produce a false PASS (an empty
chunk directory, an unrelated ``for (;;)`` loop, a non-exercising log, a stale binary
without a build manifest, a heuristic in place of a real parse) and asserts the item
now stays pending/unknown instead of being marked verified.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import progress_tracker as pt  # noqa: E402


class EvidenceBase(unittest.TestCase):
    """Redirect the module's LOGS/BUILD/SRC_RT globals at a temp tree."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="nakagawa-evidence-")
        self.logs = Path(self.tmp.name) / "logs"
        self.build = Path(self.tmp.name) / "build" / "hst"
        self.src_rt = Path(self.tmp.name) / "src" / "rt"
        self.tools = Path(self.tmp.name) / "tools"
        self.logs.mkdir(parents=True)
        self.build.mkdir(parents=True)
        self.src_rt.mkdir(parents=True)
        self.tools.mkdir(parents=True)
        self._saved = (pt.LOGS, pt.BUILD, pt.SRC_RT, pt.TOOLS)
        pt.LOGS = self.logs
        pt.BUILD = self.build
        pt.SRC_RT = self.src_rt
        pt.TOOLS = self.tools

    def tearDown(self) -> None:
        pt.LOGS, pt.BUILD, pt.SRC_RT, pt.TOOLS = self._saved
        self.tmp.cleanup()

    def touch(self, path: Path, *, old: bool = False) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x")
        stamp = time.time() - 4000 if old else time.time()
        os.utime(path, (stamp, stamp))
        return path

    def make_sources(self, *names: str) -> None:
        """Create fresh pipeline sources so freshness checks have a deterministic basis."""
        for name in names:
            self.touch(self.tools / name)


class SplitFunctionEvidenceTests(EvidenceBase):
    def test_empty_chunk_directory_is_pending_not_verified(self) -> None:
        # The old behavior scanned zero chunks, fell through and returned "verified".
        self.assertEqual(pt._check_split_function(), "pending")

    def test_missing_missplit_in_real_chunks_verifies(self) -> None:
        self.touch(self.build / "hst_recomp_0.c")
        self.assertEqual(pt._check_split_function(), "verified")


class CustomStubsEvidenceTests(EvidenceBase):
    def test_unrelated_for_loop_does_not_verify_custom_stubs(self) -> None:
        # Every generated function carries the `for (;;)` dispatch loop, so matching it
        # used to verify custom-stub injection from an unrelated loop.
        chunk = self.build / "hst_recomp_0.c"
        self.touch(chunk)
        chunk.write_text("void f(void *s) { for (;;) { SR_YIELD(s); } }", encoding="utf-8")
        self.assertEqual(pt._check_custom_stubs(), "pending")

    def test_empty_chunk_directory_is_pending(self) -> None:
        self.assertEqual(pt._check_custom_stubs(), "pending")

    def test_real_custom_stub_marker_verifies(self) -> None:
        chunk = self.build / "hst_recomp_0.c"
        self.touch(chunk)
        chunk.write_text("void f(void *s) { /* custom stub: bypass */ for (;;) { } }", encoding="utf-8")
        self.assertEqual(pt._check_custom_stubs(), "verified")


class UmdWakeupEvidenceTests(EvidenceBase):
    def _log(self, text: str) -> None:
        (self.logs / "stderr_run.log").write_text(text, encoding="utf-8")
        os.utime(self.logs / "stderr_run.log", (time.time(), time.time()))

    def test_log_without_umd_route_is_pending_not_regressed(self) -> None:
        # A headless scheduler run never touches UMD; its log must not regress P2.5.
        self._log("create thread #1\nstart thread\nsched: idle\n")
        self.assertEqual(pt._check_umd_wakeup(last_log="stderr_run.log"), "pending")

    def test_umd_route_exercised_without_wakeup_is_regressed(self) -> None:
        self._log("sceUmdWaitDriveStat: waiting\nsceUmd: status poll\n")
        self.assertEqual(pt._check_umd_wakeup(last_log="stderr_run.log"), "regressed")

    def test_wakeup_marker_verifies(self) -> None:
        self._log("sceUmdWaitDriveStat: waiting\nWakeupThread: woke 0x2\n")
        self.assertEqual(pt._check_umd_wakeup(last_log="stderr_run.log"), "verified")


class HstExeEvidenceTests(EvidenceBase):
    def test_stale_exe_without_build_manifest_is_pending(self) -> None:
        exe = self.touch(self.build / "hst.exe")
        exe.write_bytes(b"b" * (100_000_001))  # large enough to be a "real link"
        self.assertEqual(pt._check_hst_exe(), "pending")

    def test_exe_bound_to_fresh_manifest_hash_verifies(self) -> None:
        exe = self.touch(self.build / "hst.exe")
        exe.write_bytes(b"b" * (100_000_001))
        digest = hashlib.sha256(exe.read_bytes()).hexdigest().upper()
        manifest = self.logs / "build_manifest.json"
        manifest.write_text(json.dumps({"exe_sha256": digest}), encoding="utf-8")
        self.assertEqual(pt._check_hst_exe(), "verified")

    def test_exe_with_mismatched_manifest_hash_is_pending(self) -> None:
        exe = self.touch(self.build / "hst.exe")
        exe.write_bytes(b"b" * (100_000_001))
        manifest = self.logs / "build_manifest.json"
        manifest.write_text(json.dumps({"exe_sha256": "0" * 64}), encoding="utf-8")
        self.assertEqual(pt._check_hst_exe(), "pending")


class FreshnessBoundEvidenceTests(EvidenceBase):
    def test_stale_prxload_image_is_pending(self) -> None:
        self.make_sources("prxload.py", "imports.py", "analyze.py", "codegen.py")
        self.touch(self.build / "hst_image.bin", old=True)  # older than the sources
        self.assertEqual(pt._check_prxload(), "pending")

    def test_fresh_prxload_image_verifies(self) -> None:
        self.make_sources("prxload.py", "imports.py", "analyze.py")
        self.touch(self.build / "hst_image.bin")
        self.assertEqual(pt._check_prxload(), "verified")

    def test_imports_toml_requires_canonical_fresh_file(self) -> None:
        self.make_sources("imports.py", "analyze.py")
        # A stray *_imports.toml anywhere in the tree must not earn credit.
        stray = Path(self.tmp.name) / "somewhere" / "random_imports.toml"
        self.touch(stray)
        self.assertEqual(pt._check_imports_toml(), "pending")
        self.touch(self.build / "hst_imports.toml")
        self.assertEqual(pt._check_imports_toml(), "verified")

    def test_stale_imports_toml_is_pending(self) -> None:
        self.make_sources("imports.py", "analyze.py")
        self.touch(self.build / "hst_imports.toml", old=True)
        self.assertEqual(pt._check_imports_toml(), "pending")

    def test_stale_recomp_chunks_are_pending(self) -> None:
        self.make_sources("codegen.py", "analyze.py", "imports.py")
        for i in range(9):
            self.touch(self.build / f"hst_recomp_{i}.c", old=True)
        self.assertEqual(pt._check_recomp_chunks(), "pending")

    def test_fresh_recomp_chunks_verify(self) -> None:
        self.make_sources("codegen.py", "analyze.py", "imports.py")
        for i in range(9):
            self.touch(self.build / f"hst_recomp_{i}.c")
        self.assertEqual(pt._check_recomp_chunks(), "verified")


class Ps1ParseEvidenceTests(EvidenceBase):
    def test_source_heuristic_without_pwsh_is_pending(self) -> None:
        # The old fallback returned "verified" from a source-marker heuristic, awarding
        # the same weight as an executed parse.
        saved = pt._find_powershell
        pt._find_powershell = lambda: None  # type: ignore[assignment]
        try:
            fake = self.build / "hst_manager.ps1"
            fake.write_text("function Invoke-HstBuild { }\n", encoding="utf-8")
            self.assertEqual(pt._check_ps1_parse(fake), "pending")
        finally:
            pt._find_powershell = saved  # type: ignore[assignment]


class SubsystemAndNidEvidenceTests(EvidenceBase):
    def test_scheduler_matrix_checks_sr_coro_not_sched_comments(self) -> None:
        # sched.c only mentions fibers in comments; the check must look at sr_coro.c.
        self.assertEqual(
            pt._SUBSYSTEM_CHECKS["sched"][0].name,
            "sr_coro.c",
            "scheduler evidence must be sought in sr_coro.c (#181)",
        )
        # The real repo's sr_coro.c contains the actual fiber calls.
        result = pt.axis_subsystem_matrix()["sched"]
        self.assertIn("REAL", result)

    def test_nid_coverage_first_registration_wins(self) -> None:
        hle = self.src_rt / "hle.c"
        hle.write_text(
            'sr_hle_register(0xdeadbeef, "first", h_First);\n'
            'sr_hle_register(0xdeadbeef, "dup", h_LateDup);\n',
            encoding="utf-8",
        )
        imports = self.build / "hst_imports.toml"
        imports.write_text(
            '[[import]]\nstub = 0x80000000\nnid = 0xDEADBEEF\nlib = "SceMystery"\n',
            encoding="utf-8",
        )
        result = pt.axis_nid_coverage()
        self.assertEqual(result["registered_nids"], 1)
        # First registration wins, matching hle.c's duplicate rejection.
        self.assertEqual(result["registered_nonstub_nids"], 1)

    def test_nid_coverage_empty_imports_is_unknown_not_zero_success(self) -> None:
        # An empty/absent imports file must read as unknown (None), not as a verified
        # measurement of zero imports.
        imports = self.build / "hst_imports.toml"
        imports.write_text("", encoding="utf-8")
        result = pt.axis_nid_coverage()
        self.assertIsNone(result["imported_nids"])


if __name__ == "__main__":
    unittest.main()
