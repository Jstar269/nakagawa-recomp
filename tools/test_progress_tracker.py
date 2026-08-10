#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for tools/progress_tracker.py evidence checks (issue #48).

These assert the strengthened, revision/run-aware measurement model: the tracker
selects runtime evidence by modification time (not filename order), requires the
full causal sequence an item claims, treats a watchdog abort as a regression,
uses a real PowerShell parse when available, and refuses to credit VFPU from mere
asset-directory existence. Deliberately stale/conflicting logs prove it neither
selects the wrong run nor false-positives a milestone.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import progress_tracker as pt  # noqa: E402


class TrackerTestBase(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile

        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.logs = root / "logs"
        self.build = root / "build" / "hst"
        self.logs.mkdir(parents=True)
        self.build.mkdir(parents=True)
        # Redirect the module's LOGS/BUILD at the resolved-path globals used by the
        # checkers; restored in tearDown.
        self._saved = (pt.LOGS, pt.BUILD)
        pt.LOGS = self.logs
        pt.BUILD = self.build

    def tearDown(self) -> None:
        pt.LOGS, pt.BUILD = self._saved
        self._tmp.cleanup()

    def write_log(self, name: str, lines, mtime: float | None = None) -> Path:
        p = self.logs / name
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        if mtime is not None:
            os.utime(p, (mtime, mtime))
        return p


class TestLatestLog(TrackerTestBase):
    def test_selects_by_mtime_not_filename_order(self):
        now = time.time()
        # Lexicographically LAST name, but OLDER -> must NOT be chosen.
        self.write_log("stderr_run_2000.log", ["old run"], mtime=now - 5000)
        # Lexicographically EARLIER name, but NEWER -> must be chosen.
        self.write_log("stderr_run_1000.log", ["new run"], mtime=now)
        self.assertEqual(pt.latest_log(), "stderr_run_1000.log")

    def test_none_when_no_logs(self):
        self.assertIsNone(pt.latest_log())


class TestSequencePredicate(TrackerTestBase):
    def test_verified_only_when_ordered_sequence_present(self):
        self.write_log("stderr_run1.log", [
            "boot", "DISPLAY_SET_FB: 0x04000000", "vkCmdDraw: 12", "vkQueuePresent ok",
        ])
        self.assertEqual(
            pt._check_sequence(last_log="stderr_run1.log",
                               ordered=[r"DISPLAY_SET_FB:", r"vkQueuePresent"]),
            "verified",
        )

    def test_pending_when_out_of_order(self):
        # Present happens BEFORE the framebuffer set -> the claimed causality is absent.
        self.write_log("stderr_run1.log", [
            "vkQueuePresent ok", "DISPLAY_SET_FB: 0x04000000",
        ])
        self.assertEqual(
            pt._check_sequence(last_log="stderr_run1.log",
                               ordered=[r"DISPLAY_SET_FB:", r"vkQueuePresent"]),
            "pending",
        )

    def test_pending_when_second_marker_missing(self):
        self.write_log("stderr_run1.log", ["DISPLAY_SET_FB: only"])
        self.assertEqual(
            pt._check_sequence(last_log="stderr_run1.log",
                               ordered=[r"DISPLAY_SET_FB:", r"vkQueuePresent"]),
            "pending",
        )


class TestWatchdog(TrackerTestBase):
    def test_watchdog_line_is_regression_not_credit(self):
        self.write_log("stderr_run1.log", ["frame 0", "WATCHDOG: no frame in 5s, aborting"])
        self.assertEqual(pt._check_watchdog_abort(last_log="stderr_run1.log"), "regressed")

    def test_absent_watchdog_is_pending_not_verified(self):
        self.write_log("stderr_run1.log", ["frame 0", "frame 1"])
        self.assertEqual(pt._check_watchdog_abort(last_log="stderr_run1.log"), "pending")


class TestVfpuEvidence(TrackerTestBase):
    def test_verified_from_fuzz_pass_marker(self):
        self.write_log("vfpu.log", ["vfpu_fuzz: 4096 cases, 0 mismatch PASS"])
        self.assertEqual(pt._check_vfpu(), "verified")

    def test_pending_without_verification_evidence(self):
        # An unrelated log present, but no VFPU pass marker anywhere.
        self.write_log("stderr_run1.log", ["boot", "some unrelated line"])
        self.assertEqual(pt._check_vfpu(), "pending")


class TestPs1Parse(TrackerTestBase):
    def _write_ps1(self, name: str, body: str) -> Path:
        p = Path(self._tmp.name) / name
        p.write_text(body, encoding="utf-8")
        return p

    def test_valid_script_verifies(self):
        good = self._write_ps1("good.ps1",
                               "function Invoke-HstBuild { param([string]$Mode) Write-Host $Mode }\n")
        self.assertEqual(pt._check_ps1_parse(good), "verified")

    def test_broken_script_regresses_with_real_parser(self):
        if pt._find_powershell() is None:
            self.skipTest("no PowerShell interpreter available for a real parse")
        bad = self._write_ps1("bad.ps1", "function Broken { param( \n")  # unterminated
        self.assertEqual(pt._check_ps1_parse(bad), "regressed")

    def test_missing_file_is_pending(self):
        self.assertEqual(pt._check_ps1_parse(Path(self._tmp.name) / "nope.ps1"), "pending")


class TestRunMetadata(TrackerTestBase):
    def test_metadata_shape_and_stale_flag(self):
        now = time.time()
        # Build binary newer than the run -> the run is stale relative to the build.
        (self.build / "hst.exe").write_bytes(b"\x00")
        os.utime(self.build / "hst.exe", (now, now))
        self.write_log("stderr_run1.log", ["x"], mtime=now - 10000)
        meta = pt._run_metadata("stderr_run1.log")
        self.assertEqual(meta["selected_log"], "stderr_run1.log")
        self.assertIsInstance(meta["selected_log_mtime"], int)
        self.assertTrue(meta["stale_vs_build"])
        self.assertIn("source_commit", meta)

    def test_no_log_metadata(self):
        meta = pt._run_metadata(None)
        self.assertIsNone(meta["selected_log"])
        self.assertIsNone(meta["stale_vs_build"])

    def test_metadata_carries_identity_fields(self):
        now = time.time()
        (self.build / "hst.exe").write_bytes(b"\x00")
        os.utime(self.build / "hst.exe", (now, now))
        self.write_log("stderr_run1.log", ["x"], mtime=now - 10000)
        meta = pt._run_metadata("stderr_run1.log")
        self.assertRegex(meta["generated_at"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertIsInstance(meta["identity_bound"], bool)
        self.assertIn(
            meta["evidence_grade"],
            {"unknown", "executed", "stale", "freshness-bound"},
        )
        identity = meta["identity"]
        if identity is not None:
            self.assertEqual(identity["generated_at"], meta["generated_at"])

    def test_metadata_bound_and_fresh_when_log_not_older_than_binary(self):
        now = time.time()
        (self.build / "hst.exe").write_bytes(b"binary")
        os.utime(self.build / "hst.exe", (now, now))
        self.write_log("stderr_run1.log", ["x"], mtime=now)
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            meta = pt._run_metadata("stderr_run1.log")
        finally:
            pt._git_commit = saved  # type: ignore[assignment]
        self.assertFalse(meta["stale_vs_build"])
        self.assertTrue(meta["identity_bound"])
        self.assertEqual(meta["evidence_grade"], "freshness-bound")

    def test_metadata_unbound_without_binary(self):
        self.write_log("stderr_run1.log", ["x"], mtime=time.time())
        meta = pt._run_metadata("stderr_run1.log")
        self.assertFalse(meta["identity_bound"])
        self.assertIsNone(meta["identity"])
        self.assertEqual(meta["evidence_grade"], "executed")


class TestEvidenceHelpers(TrackerTestBase):
    def test_sha256_file_hash_and_missing(self):
        p = self.logs / "probe.bin"
        p.write_bytes(b"hello evidence")
        self.assertEqual(
            pt._sha256_file(p),
            hashlib.sha256(b"hello evidence").hexdigest(),
        )
        self.assertIsNone(pt._sha256_file(self.logs / "missing.bin"))

    def test_profile_descriptor_is_deterministic_route_profile(self):
        first = pt._profile_descriptor()
        second = pt._profile_descriptor()
        self.assertEqual(first, {"game": "hst", "base": 0, "entry": 0})
        self.assertEqual(first, second)

    def test_build_run_identity_none_without_binary(self):
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            self.assertIsNone(pt._build_run_identity(generated_at="2026-08-05T00:00:00Z"))
        finally:
            pt._git_commit = saved  # type: ignore[assignment]

    def test_build_run_identity_none_without_commit(self):
        (self.build / "hst.exe").write_bytes(b"binary")
        saved = pt._git_commit
        pt._git_commit = lambda: None  # type: ignore[assignment]
        try:
            self.assertIsNone(pt._build_run_identity(generated_at="2026-08-05T00:00:00Z"))
        finally:
            pt._git_commit = saved  # type: ignore[assignment]

    def test_build_run_identity_bound_round_trips_evidence_model(self):
        (self.build / "hst.exe").write_bytes(b"binary-bytes")
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            identity = pt._build_run_identity(generated_at="2026-08-05T00:00:00Z")
        finally:
            pt._git_commit = saved  # type: ignore[assignment]
        self.assertIsNotNone(identity)
        self.assertEqual(identity["source_commit"], "1" * 40)
        self.assertEqual(identity["binary_sha256"], hashlib.sha256(b"binary-bytes").hexdigest())
        self.assertRegex(identity["profile_sha256"], r"^[0-9a-f]{64}$")
        self.assertIsNone(identity["input_manifest_sha256"])
        self.assertEqual(identity["generated_at"], "2026-08-05T00:00:00Z")
        # The identity must satisfy the shared model's strict parser (single authority).
        import evidence_model as em

        em.EvidenceIdentity.from_mapping(identity)

    def test_build_run_identity_includes_input_manifest_hash(self):
        (self.build / "hst.exe").write_bytes(b"binary")
        toml = self.build / "hst_imports.toml"
        toml.write_bytes(b"[[import]]\n")
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            identity = pt._build_run_identity(generated_at="2026-08-05T00:00:00Z")
        finally:
            pt._git_commit = saved  # type: ignore[assignment]
        self.assertEqual(
            identity["input_manifest_sha256"],
            hashlib.sha256(b"[[import]]\n").hexdigest(),
        )

    def test_evidence_grade_mapping(self):
        self.assertEqual(
            pt._evidence_grade("pending", kind="executed", identity_bound=True, stale=False),
            "unknown",
        )
        self.assertEqual(
            pt._evidence_grade("verified", kind="source-shape", identity_bound=True, stale=False),
            "heuristic",
        )
        self.assertEqual(
            pt._evidence_grade("verified", kind="executed", identity_bound=True, stale=True),
            "stale",
        )
        self.assertEqual(
            pt._evidence_grade("verified", kind="executed", identity_bound=True, stale=False),
            "content-validated",
        )
        self.assertEqual(
            pt._evidence_grade("verified", kind="executed", identity_bound=False, stale=None),
            "executed",
        )
        self.assertEqual(
            pt._evidence_grade("regressed", kind="executed", identity_bound=True, stale=None),
            "executed",
        )

    def test_run_evidence_grade_mapping(self):
        self.assertEqual(pt._run_evidence_grade(None, True, False), "unknown")
        self.assertEqual(pt._run_evidence_grade("x", False, None), "executed")
        self.assertEqual(pt._run_evidence_grade("x", True, True), "stale")
        self.assertEqual(pt._run_evidence_grade("x", True, False), "freshness-bound")

    def test_item_kind_classification(self):
        self.assertEqual(pt._item_kind("P1.4"), "source-shape")
        self.assertEqual(pt._item_kind("P2.7"), "source-shape")
        self.assertEqual(pt._item_kind("P3.8"), "source-shape")
        self.assertEqual(pt._item_kind("P2.1"), "executed")
        self.assertEqual(pt._item_kind("P3.3"), "executed")
        self.assertEqual(pt._item_kind("P5.3"), "executed")


class TestItemEvidenceGrades(TrackerTestBase):
    def _bound_run(self, log_mtime: float, exe_mtime: float) -> None:
        (self.build / "hst.exe").write_bytes(b"B" * 1024)
        os.utime(self.build / "hst.exe", (exe_mtime, exe_mtime))
        self.write_log(
            "stderr_run1.log",
            [
                "THREAD_SEED_OK",
                "create thread #1",
                "start thread",
                "DISPLAY_SET_FB: 0x04000000",
                "vkCmdDraw: 12",
                "vkQueuePresent ok",
                "DISPLAY_SET_FB: 0x04000000",
            ],
            mtime=log_mtime,
        )

    def _items_with_commit(self, log_mtime: float, exe_mtime: float):
        self._bound_run(log_mtime, exe_mtime)
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            items = pt.verify_all()
        finally:
            pt._git_commit = saved  # type: ignore[assignment]
        return {it.id: it for it in items}

    def test_bound_fresh_run_grades_log_items_content_validated(self):
        now = time.time()
        by_id = self._items_with_commit(log_mtime=now, exe_mtime=now)
        # Log-derived, bound to the current binary, log not older than binary.
        self.assertEqual(by_id["P2.1"].evidence, "content-validated")
        self.assertEqual(by_id["P6.1"].evidence, "content-validated")
        self.assertEqual(by_id["P5.3"].evidence, "content-validated")
        # Pending items stay unknown even on a bound run.
        self.assertEqual(by_id["P7.1"].evidence, "unknown")
        self.assertEqual(by_id["P1.6"].evidence, "unknown")
        # Artifact-existence checks never claim execution.
        self.assertEqual(by_id["P1.4"].evidence, "unknown")  # pending (no manifest)

    def test_stale_run_marks_log_items_stale(self):
        now = time.time()
        by_id = self._items_with_commit(log_mtime=now - 10000, exe_mtime=now)
        self.assertEqual(by_id["P2.1"].evidence, "stale")
        self.assertEqual(by_id["P6.1"].evidence, "stale")

    def test_unbound_run_keeps_executed_grade(self):
        now = time.time()
        # No hst.exe in the temp build: identity cannot bind.
        self.write_log(
            "stderr_run1.log",
            ["THREAD_SEED_OK", "DISPLAY_SET_FB: 0x04000000", "vkQueuePresent ok"],
            mtime=now,
        )
        items = pt.verify_all()
        by_id = {it.id: it for it in items}
        self.assertEqual(by_id["P2.1"].evidence, "executed")
        self.assertEqual(by_id["P7.1"].evidence, "unknown")

    def test_aggregate_emits_evidence_per_item(self):
        now = time.time()
        by_id = self._items_with_commit(log_mtime=now, exe_mtime=now)
        agg = pt.aggregate(list(by_id.values()))
        serialized = {item["id"]: item for item in agg["items"]}
        self.assertEqual(serialized["P2.1"]["evidence"], "content-validated")
        self.assertEqual(serialized["P7.1"]["evidence"], "unknown")

    def test_render_markdown_includes_identity(self):
        now = time.time()
        by_id = self._items_with_commit(log_mtime=now, exe_mtime=now)
        milestone = pt.aggregate(list(by_id.values()))
        milestone["latest_log"] = "stderr_run1.log"
        saved = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        try:
            milestone["run"] = pt._run_metadata("stderr_run1.log")
        finally:
            pt._git_commit = saved  # type: ignore[assignment]
        doc = pt.render_progress_markdown(pt.measure_all_axes(), milestone)
        self.assertIn("Evidence grade", doc)
        self.assertIn("freshness-bound", doc)

    def test_verify_main_emits_identity_bound_run_to_progress_json(self):
        now = time.time()
        self._bound_run(log_mtime=now, exe_mtime=now)
        saved_commit = pt._git_commit
        pt._git_commit = lambda: "1" * 40  # type: ignore[assignment]
        saved_out = pt.OUT
        out_path = Path(self._tmp.name) / "progress.json"
        pt.OUT = out_path
        try:
            self.assertEqual(pt.main(["verify"]), 0)
        finally:
            pt._git_commit = saved_commit  # type: ignore[assignment]
            pt.OUT = saved_out
        emitted = json.loads(out_path.read_text(encoding="utf-8"))
        identity = emitted["run"]["identity"]
        self.assertTrue(emitted["run"]["identity_bound"])
        self.assertEqual(emitted["run"]["evidence_grade"], "freshness-bound")
        self.assertEqual(identity["source_commit"], "1" * 40)
        self.assertEqual(identity["binary_sha256"], hashlib.sha256(b"B" * 1024).hexdigest())
        self.assertRegex(identity["profile_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(identity["generated_at"], emitted["run"]["generated_at"])
        # Per-item grades ride along in the emitted items array.
        by_id = {item["id"]: item for item in emitted["items"]}
        self.assertEqual(by_id["P2.1"]["evidence"], "content-validated")
        self.assertEqual(by_id["P7.1"]["evidence"], "unknown")


if __name__ == "__main__":
    unittest.main()
