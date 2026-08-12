# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Visual-oracle runner guarantees.

Two layers:

* the behavioral half runs ``tools/test_visual_oracle.ps1``, which exercises the real
  helpers against real processes and directories (Windows only -- skipped elsewhere);
* the static half pins the contract in ``hst_manager.ps1`` that those helpers exist to
  enforce, so the manager cannot quietly go back to sleeping a whole deadline or writing
  a second run on top of a first.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "hst_manager.ps1"
SUPPORT = ROOT / "tools" / "hst_run_support.ps1"
PS_TESTS = ROOT / "tools" / "test_visual_oracle.ps1"
HLE = ROOT / "src" / "rt" / "hle.c"


def _powershell() -> str | None:
    return shutil.which("pwsh")


class VisualOracleBehaviorTests(unittest.TestCase):
    """Run the PowerShell helper tests, which need real processes to be meaningful."""

    @unittest.skipUnless(sys.platform == "win32", "PowerShell helpers are Windows-only")
    def test_run_support_helpers(self) -> None:
        shell = _powershell()
        if shell is None:
            self.skipTest("no PowerShell interpreter on PATH")
        proc = subprocess.run(
            [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PS_TESTS)],
            capture_output=True,
            text=True,
            timeout=600,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"tools/test_visual_oracle.ps1 failed:\n{proc.stdout}\n{proc.stderr}",
        )


class VisualOracleContractTests(unittest.TestCase):
    """Static guards on the manager wiring the behavioral tests cannot reach."""

    def setUp(self) -> None:
        self.manager = MANAGER.read_text(encoding="utf-8-sig")
        self.support = SUPPORT.read_text(encoding="utf-8-sig")

    def test_runner_waits_on_the_process_not_the_clock(self) -> None:
        # The original defect: Start-Sleep -Seconds $RunDuration ran to completion even
        # after hst.exe had exited, adding ~50 idle minutes to a long oracle run.
        self.assertNotIn(
            "Start-Sleep -Seconds $RunDuration",
            self.manager,
            "the run deadline must not be an unconditional sleep",
        )
        self.assertIn("Wait-ProcessOrKill -Process $proc", self.manager)

    def test_run_support_is_dot_sourced(self) -> None:
        self.assertIn("hst_run_support.ps1", self.manager)
        for fn in ("Wait-ProcessOrKill", "Reset-OracleArchive", "Sync-SaveBase", "Get-OracleVerdict"):
            self.assertIn(f"function {fn}", self.support, f"{fn} must live in the support file")

    def test_oracle_archive_is_reset_before_a_run(self) -> None:
        self.assertIn("Reset-OracleArchive -Path $outDir", self.manager)
        self.assertIn("OverwriteOracle", self.manager)

    def test_oracle_records_a_provenance_manifest(self) -> None:
        # An evidence set that cannot be traced to a build, a route and a verdict is not
        # evidence. Each of these was requested explicitly after #141 review.
        for field in (
            "git_head",
            "exe_sha256",
            "route_sha256",
            "exit_code",
            "capture_count",
            "wall_seconds",
            "requested_vblank",
            "observed_vblank",
            "complete",
        ):
            self.assertIn(field, self.manager, f"manifest must record {field}")
        self.assertIn("oracle_manifest.json", self.manager)

    def test_oracle_supports_profiles_without_a_second_runner(self) -> None:
        # #33 baselines need the Benchmark profile; they must not fork the runner.
        self.assertIn("-RunProfile", self.manager)
        oracle = self.manager[self.manager.index("function Invoke-VisualOracle") :]
        oracle = oracle[: oracle.index("function Invoke-DiffFunc")]
        self.assertIn("Run-HstEngine -Profile $RunProfile", oracle)
        self.assertNotIn(
            "Start-Process",
            oracle,
            "the oracle must go through Run-HstEngine, not launch its own runner",
        )

    def test_save_state_can_be_held_still_across_runs(self) -> None:
        # Deterministic inputs are not enough: the game persists a save, so without this two
        # replays of one route are two different experiments.
        self.assertIn("-SaveBase", self.manager)
        self.assertIn("Sync-SaveBase -BasePath $SaveBase", self.manager)
        self.assertIn("save_base_action", self.manager)
        self.assertIn("GAMEDATA", self.support, "the install must be explicitly excluded")

    def test_snap_windows_do_not_overwrite_each_other(self) -> None:
        # The whole point of a two-window capture is comparing both ends of a transition
        # from the same run. The rotating 8-slot name would let the late window overwrite
        # the early one, silently destroying the reference the comparison depends on.
        hle = HLE.read_text(encoding="utf-8")
        self.assertIn('getenv("SR_FBSNAP_WINDOWS")', hle)
        self.assertIn('"snap_v%u.ppm"', hle)
        self.assertIn("SnapWindows", self.manager)
        self.assertIn("SR_FBSNAP_WINDOWS", self.manager)

    def test_exit_at_vblank_is_the_last_statement_of_the_tick(self) -> None:
        # The stop point must be a fully completed vblank tick: the controller sample for
        # vblank V has to be latched before the process exits, or a route's final press
        # can be dropped. Assert placement structurally, not by comment.
        hle = HLE.read_text(encoding="utf-8")
        start = hle.index("void sr_vblank_tick(void) {")
        # The function ends at the first line-start closing brace after it.
        end = hle.index("\n}\n", start)
        body = hle[start:end]
        self.assertIn("SR_EXIT_AT_VBLANK", body, "the exit control must live in the tick")
        self.assertLess(
            body.index("sr_ctrl_sample()"),
            body.index("SR_EXIT_AT_VBLANK"),
            "the controller sample for this vblank must be latched before the exit check",
        )
        tail = body[body.index('getenv("SR_EXIT_AT_VBLANK")'):]
        self.assertNotIn(
            "sr_ctrl_sample",
            tail,
            "no tick work may follow the exit check",
        )
        # Nothing but the closing of the exit block may follow it.
        self.assertIsNotNone(
            re.search(r"_Exit\(0\);\s*\}\s*\}\s*$", body),
            "the exit check must be the final statement of sr_vblank_tick",
        )


if __name__ == "__main__":
    unittest.main()
