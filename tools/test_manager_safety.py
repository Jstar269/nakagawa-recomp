# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Manager filesystem/process/build safety guarantees (#183).

Two layers:

* the behavioral half runs ``tools/test_manager_safety.ps1``, which exercises the real
  safety primitives against temporary directories, junctions and mocked process
  identities (Windows only -- skipped elsewhere);
* the static half pins the fail-closed contracts in ``hst_manager.ps1`` that the
  behavioral tests cannot reach without a real game build: repository-root anchoring,
  OracleName grammar, unknown-exit build truth, and time-input validation.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "hst_manager.ps1"
SUPPORT = ROOT / "tools" / "hst_run_support.ps1"
SAFETY = ROOT / "tools" / "hst_safety.ps1"
PS_TESTS = ROOT / "tools" / "test_manager_safety.ps1"


def _powershell() -> str | None:
    return shutil.which("pwsh")


class ManagerSafetyBehaviorTests(unittest.TestCase):
    """Run the PowerShell safety tests (temp dirs, junctions, mocked identities)."""

    @unittest.skipUnless(sys.platform == "win32", "PowerShell safety helpers are Windows-only")
    def test_manager_safety_helpers(self) -> None:
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
            f"tools/test_manager_safety.ps1 failed:\n{proc.stdout}\n{proc.stderr}",
        )


class ManagerSafetyContractTests(unittest.TestCase):
    """Static fail-closed contracts on the manager wiring."""

    def setUp(self) -> None:
        self.manager = MANAGER.read_text(encoding="utf-8-sig")
        self.safety = SAFETY.read_text(encoding="utf-8-sig")
        self.support = SUPPORT.read_text(encoding="utf-8-sig")

    def test_manager_anchors_to_its_own_script_location(self) -> None:
        # The canonical root is $PSScriptRoot, never the caller's CWD.
        self.assertIn("Assert-HstWorkspaceRoot -Root $PSScriptRoot", self.manager)
        self.assertIn("Set-Location -LiteralPath $script:RepoRoot", self.manager)
        self.assertIn("hst_safety.ps1", self.manager)
        # Managed paths derive from the canonical root.
        self.assertIn('$LogDir = Join-Path $script:RepoRoot "logs"', self.manager)

    def test_workspace_identity_anchors_are_validated(self) -> None:
        for anchor in ("Makefile", "AGENTS.md", "src/rt/recomp.c", "tools/codegen.py"):
            self.assertIn(anchor, self.safety, f"anchor {anchor} must be part of the root validation")

    def test_oracle_name_is_a_safe_component_not_a_path(self) -> None:
        self.assertIn("Test-SafeComponentName", self.manager)
        self.assertIn("ValidatePattern", self.manager)
        self.assertIn("oracle_$OracleName", self.manager)
        # The archive reset must be root-contained.
        self.assertIn("Reset-OracleArchive -Path $outDir -AllowedRoot $script:LogDir", self.manager)

    def test_snap_cleanup_is_workspace_anchored_and_file_scoped(self) -> None:
        self.assertIn('-Filter "snap_*.ppm" -File', self.manager)
        self.assertIn("$script:RepoRoot", self.manager)

    def test_save_sync_is_approved_root_contained_and_transactional(self) -> None:
        self.assertIn("Sync-SaveBase -BasePath $SaveBase", self.manager)
        self.assertIn("-ApprovedRoot $script:RepoRoot", self.manager)
        self.assertIn("-SaveRoot (Join-Path $script:RepoRoot", self.manager)
        # The transactional contract must live in the support file.
        self.assertIn(".hst_savebase_manifest.json", self.support)
        self.assertIn("Remove-SafeDirectory", self.support)
        self.assertIn("Failpoint", self.support)

    def test_build_success_requires_a_known_zero_exit(self) -> None:
        # The stale-exe fallback that turned a null exit code into success is gone.
        self.assertNotIn(
            "$makeExitCode = if (Test-Path (Join-Path $buildDir \"hst.exe\")) { 0 } else { 1 }",
            self.manager,
            "null exit code must not fall back to executable existence",
        )
        self.assertIn("UNKNOWN (null exit code)", self.manager)
        self.assertIn("Get-KnownExitCode -Process $proc", self.manager)
        # The final build result is a tracked success, not Test-Path on the binary.
        self.assertIn("Write-HstBuildManifest", self.manager)
        self.assertNotIn("return (Test-Path (Join-Path $buildDir \"hst.exe\"))", self.manager)

    def test_process_cleanup_requires_full_identity(self) -> None:
        self.assertIn("Get-ProcessIdentityRecord", self.manager)
        self.assertIn("Invoke-StaleBuildCleanup", self.manager)
        self.assertIn("creation_ticks", self.safety)
        self.assertIn("cmd", self.safety)
        self.assertIn("exe", self.safety)

    def test_duration_rejects_negative_values(self) -> None:
        self.assertIn("[ValidateRange(0, 2000000000)]\n    [int]$Duration = 0", self.manager)
        self.assertIn("ConvertTo-SafeTimeoutSeconds", self.manager)

    def test_caller_location_is_restored_on_every_exit_path(self) -> None:
        self.assertIn("finally {", self.manager)
        self.assertIn("Set-Location -LiteralPath $script:OriginalLocation", self.manager)
        # Early `exit` must not skip that restoration.
        self.assertIn('if ($Action -and $script:ManagerExitCode -ne 0) {\n    exit $script:ManagerExitCode', self.manager)

    def test_verify_suite_emits_a_machine_readable_gate_summary(self) -> None:
        # The Verify suite must report which subgates executed/passed/skipped and which
        # private-input gates were not run, in a stable machine-checkable line, without
        # changing the existing [PASS]/[FAIL]/exit-code contract.
        self.assertIn("VERIFY_SUMMARY", self.manager)
        self.assertIn("aggregate=", self.manager)
        for gate in (
            "python-unittest",
            "sched-selftest",
            "profiler-selftest",
            "heap-selftest",
            "asset-index-selftest",
            "hle-thread-selftest",
            "fp-convert-selftest",
            "vfpu-tables-selftest",
            "watchpoints-file-selftest",
            "vfpu-interp-selftest",
            "ref-selftest",
            "import-audit-gate",
            "publish-audit-index",
            "publish-audit-worktree",
            "gpu-coherence-selftest",
            "gpu-capture-selftest",
        ):
            self.assertIn(f'"{gate}"', self.manager, f"VERIFY_SUMMARY must cover {gate}")
        for unavailable in (
            "make-verify=NOT_RUN",
            "atrac3p-title-accept=NOT_RUN",
            "visual-oracle=NOT_RUN",
        ):
            self.assertIn(unavailable, self.manager)
        # A SKIP (Vulkan unavailable) must be reported as SKIP, never folded into a pass.
        self.assertIn('$gateStatus["gpu-coherence-selftest"] = "SKIP"', self.manager)
        self.assertIn('$gateStatus["gpu-capture-selftest"] = "SKIP"', self.manager)

    def test_no_early_exit_inside_the_try_body(self) -> None:
        # Every failure inside the action dispatch records a code and breaks, so the
        # finally block runs before the single exit at the end.
        body = self.manager[self.manager.index("if ($Action) {") : self.manager.index("} catch {")]
        self.assertNotIn("exit 1", body, "action dispatch must not exit before the finally block")


if __name__ == "__main__":
    unittest.main()
