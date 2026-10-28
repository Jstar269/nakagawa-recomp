#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for tools/progress_tracker.py evidence checks (issues #48 and #1376).

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

    def test_watchdog_abort_form_is_regression(self):
        self.write_log("stderr_run1.log", [
            "frame 0",
            "WATCHDOG: no new frame presented for 600 vblanks (~10s) - neutral NO-NEW-FLIP observation, not by itself a hang/stall verdict",
            "WATCHDOG: aborting after 600 vblanks with no new frame (SR_WATCHDOG_EXIT=600)",
        ])
        self.assertEqual(pt._check_watchdog_abort(last_log="stderr_run1.log"), "regressed")

    def test_neutral_no_new_frame_observation_is_not_a_regression(self):
        self.write_log("stderr_run1.log", [
            "frame 0",
            "WATCHDOG: no new frame presented for 600 vblanks (~10s) - neutral NO-NEW-FLIP observation, not by itself a hang/stall verdict",
            "frame 1",
        ])
        self.assertEqual(pt._check_watchdog_abort(last_log="stderr_run1.log"), "pending")

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


class TestUmdWakeupCorrelation(TrackerTestBase):
    """P2.5 credits a UMD wakeup only from the ordered, uid-correlated chain (#1376): a
    thread enters a sleep, the registered UMD callback is notified with a drive state
    carrying READY, and that callback's owner thread wakes the same sleeping thread."""

    CHAIN = [
        "CBLOG: UMD callback registered uid=0x5",
        "DEBUG_SLEEP: thread=0x111 wakeups=0 state=2",
        "CBLOG: NotifyCallback uid=0x5 count=1 arg=0x00000032 owner=0x10f",
        "WAKEUP target=0x111 (from 0x10f)",
        "DEBUG_WAKEUP: target=0x111 sleeping=1 state=4 wait_obj=0x111 wakeups=0",
    ]

    def check(self, lines) -> str:
        self.write_log("stderr_run1.log", lines)
        return pt._check_umd_wakeup(last_log="stderr_run1.log")

    def test_correlated_chain_verifies(self):
        self.assertEqual(self.check(self.CHAIN), "verified")

    def test_bare_wakeup_line_is_not_a_umd_linkage(self):
        # Any WakeupThread line used to verify P2.5, with nothing tying it to a UMD wait.
        self.assertEqual(self.check([
            "HLE: calling sceUmdWaitDriveStat (0x8ef08fce) from 0x111",
            "HLE: calling sceKernelWakeupThread (0xd59ead2f) from 0x10f",
            "WakeupThread: woke 0x111",
        ]), "pending")

    def test_umd_activity_without_wakeup_is_not_a_regression(self):
        # A drive that is already ready needs no wakeup; absence proves no failure.
        self.assertEqual(self.check([
            "HLE: calling sceUmdActivate (0xc6183d47) from 0x10f",
            "HLE: calling sceUmdWaitDriveStat (0x8ef08fce) from 0x10f",
        ]), "pending")

    def test_wake_before_the_drive_became_ready_is_refused(self):
        chain = list(self.CHAIN)
        chain.append(chain.pop(2))  # the READY notify now follows the wake
        self.assertEqual(self.check(chain), "pending")

    def test_wake_from_a_thread_other_than_the_callback_owner_is_refused(self):
        chain = list(self.CHAIN)
        chain[3] = "WAKEUP target=0x111 (from 0x222)"
        self.assertEqual(self.check(chain), "pending")

    def test_thread_that_slept_only_after_the_state_change_is_refused(self):
        chain = list(self.CHAIN)
        chain[1], chain[2] = chain[2], chain[1]
        self.assertEqual(self.check(chain), "pending")

    def test_sleep_satisfied_by_a_banked_wakeup_is_not_a_wait(self):
        chain = list(self.CHAIN)
        chain[1] = "DEBUG_SLEEP: thread=0x111 wakeups=1 state=2"
        self.assertEqual(self.check(chain), "pending")

    def test_wakeup_that_only_banks_is_not_a_woken_thread(self):
        chain = list(self.CHAIN)
        chain[4] = "DEBUG_WAKEUP: target=0x111 sleeping=0 state=4 wait_obj=0x554d44 wakeups=0"
        self.assertEqual(self.check(chain), "pending")

    def test_notify_of_another_callback_is_not_a_drive_state_change(self):
        chain = list(self.CHAIN)
        chain[2] = "CBLOG: NotifyCallback uid=0x6 count=1 arg=0x00000032 owner=0x10f"
        self.assertEqual(self.check(chain), "pending")

    def test_notify_without_drive_ready_is_not_the_ready_transition(self):
        chain = list(self.CHAIN)
        chain[2] = "CBLOG: NotifyCallback uid=0x5 count=1 arg=0x00000012 owner=0x10f"
        self.assertEqual(self.check(chain), "pending")

    def test_thread_woken_and_asleep_again_is_a_new_wait(self):
        # The wait that spanned the state change ended before the owner's wake.
        chain = list(self.CHAIN)
        chain[3:3] = [
            "WAKEUP target=0x111 (from 0x333)",
            "DEBUG_WAKEUP: target=0x111 sleeping=1 state=4 wait_obj=0x111 wakeups=0",
            "DEBUG_SLEEP: thread=0x111 wakeups=0 state=2",
        ]
        self.assertEqual(self.check(chain), "pending")

    def test_ready_state_value_matches_the_scheduler_enum(self):
        # DEBUG_WAKEUP prints sched.c's numeric thread state; the readied branch needs
        # TH_WAIT_OBJ, so the tracker's constant must follow that enum.
        sched = Path(__file__).resolve().parent.parent / "src" / "rt" / "sched.c"
        match = re.search(r"enum\s*\{\s*(TH_DORMANT\s*=\s*0\s*,[^}]*)\}", sched.read_text(encoding="utf-8"))
        self.assertIsNotNone(match)
        names = [part.split("=")[0].strip() for part in match.group(1).split(",") if part.strip()]
        self.assertEqual(names.index("TH_WAIT_OBJ"), pt._TH_WAIT_OBJ)


class TestFrameRenderingMeasurements(TrackerTestBase):
    """P5.1 (GE submission), P5.2 (command recording) and P6.3 (more than one draw) are
    separate measurements over the records the runtime writes (#1376)."""

    GE_ENQ = "GE_ENQ: list_id=0x35000000 list=0x09a4b000 stall=0x09a4b100 cbid=0 -> next_pc=0x00000000 (DONE)"

    def statuses(self, lines) -> dict:
        self.write_log("stderr_run1.log", lines)
        saved = pt._find_powershell
        pt._find_powershell = lambda: None  # type: ignore[assignment]
        try:
            items = pt.verify_all()
        finally:
            pt._find_powershell = saved  # type: ignore[assignment]
        return {it.id: it.status for it in items}

    def test_enqueue_call_and_refusal_lines_are_not_a_submission(self):
        status = self.statuses([
            "HLE: calling sceGeListEnQueue (0xab49e76a) from 0x10f",
            "UNSUPPORTED_IMPORT: sceGeListEnQueueHead queue ordering is not supported yet.",
            "GE_UPDATE_STALL: ge_run_list returned 0x00000000 (COMPLETED)",
        ])
        self.assertEqual(status["P5.1"], "pending")

    def test_accepted_enqueue_is_a_submission(self):
        self.assertEqual(self.statuses([self.GE_ENQ])["P5.1"], "verified")
        deferred = ("GE_ENQ: list_id=0x35000001 list=0x09a4b000 stall=0x09a4b000 cbid=0 "
                    "-> DEFERRED (stall==list)")
        self.assertEqual(self.statuses([deferred])["P5.1"], "verified")

    def test_null_list_enqueue_is_not_a_display_list(self):
        line = "GE_ENQ: list_id=0x35000000 list=0x00000000 stall=0x00000000 cbid=0 -> next_pc=0x00000000 (DONE)"
        self.assertEqual(self.statuses([line])["P5.1"], "pending")

    def test_enqueue_trace_needs_a_matching_result(self):
        trace = ("GE_ENQUEUE_TRACE frame=3 op=enqueue thread=0x10f call_pc=0x0880a000 ra=0x0880a010 "
                 "list_id=0x35000002 list=0x09a4b000 stall=0x09a4b100 cbid=0x00000000 "
                 "prev_hle=none/0x00000000@0x00000000,ra=0x00000000,frame=0,seq=0 "
                 "prev_cb=0x00000000@0x00000000,frame=0,seq=0")
        result = ("GE_ENQUEUE_RESULT frame=3 op=enqueue thread=0x10f call_pc=0x0880a000 "
                  "list_id=0x35000002 outcome=done next_pc=0x00000000 complete=1 sig=0x00000000 prims=4 "
                  "through=0/0/0 transform=0/0/0 writes=0 nonblack=0 clearpx=0")
        self.assertEqual(self.statuses([trace, result])["P5.1"], "verified")
        self.assertEqual(self.statuses([trace])["P5.1"], "pending")
        other = result.replace("list_id=0x35000002", "list_id=0x35000009")
        self.assertEqual(self.statuses([trace, other])["P5.1"], "pending")
        stall = result.replace("op=enqueue", "op=update_stall")
        self.assertEqual(self.statuses([trace, stall])["P5.1"], "pending")

    def test_submission_alone_records_no_command_buffer(self):
        status = self.statuses([self.GE_ENQ])
        self.assertEqual(status["P5.2"], "pending")
        self.assertEqual(status["P6.3"], "pending")

    def test_recorded_batch_is_a_command_buffer_but_one_draw_is_not_multiple(self):
        status = self.statuses(["GEGPU submit #1 batches=1 verts=6 tgt=0x04000000/512 fmt=3"])
        self.assertEqual(status["P5.2"], "verified")
        self.assertEqual(status["P5.1"], "pending")
        self.assertEqual(status["P6.3"], "pending")

    def test_single_draw_line_does_not_verify_multiple_draws(self):
        # One legacy draw line used to verify both P5.2 and "Multiple vkCmdDraw"; the
        # runtime writes no such record, and one line is never more than one draw.
        status = self.statuses(["vkCmdDraw: 12"])
        self.assertEqual(status["P5.2"], "pending")
        self.assertEqual(status["P6.3"], "pending")

    def test_draws_counted_across_recorded_batches(self):
        status = self.statuses([
            "GEGPU submit #1 batches=1 verts=6 tgt=0x04000000/512 fmt=3",
            "GEGPU submit #2 batches=1 verts=6 tgt=0x04000000/512 fmt=3",
        ])
        self.assertEqual(status["P6.3"], "verified")
        stats = ("GEGPU stats: submits=2 batches=7 verts=84 avg_batches=3.50 tris=10 spr=4 lines=0 "
                 "nonfinite=0 rtt=0 snap=0 present[gpu=2 cpu=0] upload=0 readback=0 texup=1 dirty=0 "
                 "xferblit=0 pipes=2 texs=1 shadow[invalidations=0 checks=0 hits=0 misses=0 bytes=0 "
                 "avoided=0 required=0] final=1")
        status = self.statuses([stats])
        self.assertEqual(status["P5.2"], "verified")
        self.assertEqual(status["P6.3"], "verified")

    def test_dropped_batch_leaves_the_draw_count_unknown(self):
        status = self.statuses([
            "GEGPU submit #1 batches=5 verts=30 tgt=0x04000000/512 fmt=3",
            "gegpu: pipeline creation failed; a batch was dropped",
        ])
        self.assertEqual(status["P5.2"], "verified")
        self.assertEqual(status["P6.3"], "pending")


class TestDispatchTableContract(TrackerTestBase):
    """P2.7 checks the live dispatch-table contract (dispatch_table.h, issue #45), not the
    g_exact_hooks/DispatchHook markers removed in #566 (#1376)."""

    HEADER = (
        "typedef struct { uint32_t addr; uintptr_t fn; uint32_t state; } SrDispatchEntry;\n"
        "typedef struct {\n    SrDispatchEntry *l1[256];\n} SrDispatchTable;\n"
        "static inline void sr_dtab_register(SrDispatchTable *t, uint32_t addr, uintptr_t fn) { }\n"
        "static inline uintptr_t sr_dtab_lookup(SrDispatchTable *t, uint32_t addr) { return 0; }\n"
    )
    RECOMP = (
        '#include "dispatch_table.h"\n'
        "static SrDispatchTable g_dtab;\n"
        "void sr_register(uint32_t addr, RecompFn fn) {\n"
        "    sr_dtab_register(&g_dtab, addr, (uintptr_t)fn);\n}\n"
        "RecompFn sr_lookup(uint32_t addr) {\n"
        "    return (RecompFn)sr_dtab_lookup(&g_dtab, addr);\n}\n"
    )

    def src(self, header: str, recomp: str) -> Path:
        root = Path(self._tmp.name) / "src" / "rt"
        root.mkdir(parents=True, exist_ok=True)
        (root / "dispatch_table.h").write_text(header, encoding="utf-8")
        (root / "recomp.c").write_text(recomp, encoding="utf-8")
        return root

    def test_live_source_meets_the_dispatch_table_contract(self):
        # The removed markers kept P2.7 pending forever against the real source.
        self.assertEqual(pt._check_dispatch_table(), "verified")

    def test_removed_hook_markers_prove_nothing(self):
        root = self.src("", "static DispatchHook g_exact_hooks[16];\n")
        self.assertEqual(pt._check_dispatch_table(src_rt=root), "pending")

    def test_contract_shape_verifies(self):
        self.assertEqual(pt._check_dispatch_table(src_rt=self.src(self.HEADER, self.RECOMP)), "verified")

    def test_lookup_that_bypasses_the_table_is_refused(self):
        recomp = self.RECOMP.replace("(RecompFn)sr_dtab_lookup(&g_dtab, addr)", "s_linear_find(addr)")
        self.assertEqual(pt._check_dispatch_table(src_rt=self.src(self.HEADER, recomp)), "pending")

    def test_register_that_bypasses_the_table_is_refused(self):
        recomp = self.RECOMP.replace("sr_dtab_register(&g_dtab, addr, (uintptr_t)fn);", "s_list_add(addr, fn);")
        self.assertEqual(pt._check_dispatch_table(src_rt=self.src(self.HEADER, recomp)), "pending")


class TestPs1Parse(TrackerTestBase):
    def _write_ps1(self, name: str, body: str) -> Path:
        p = Path(self._tmp.name) / name
        p.write_text(body, encoding="utf-8")
        return p

    def test_valid_script_verifies(self):
        if pt._find_powershell() is None:
            self.skipTest("no PowerShell interpreter available for a real parse")
        good = self._write_ps1("good.ps1",
                               "function Invoke-NkBuild { param([string]$Mode) Write-Host $Mode }\n")
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
