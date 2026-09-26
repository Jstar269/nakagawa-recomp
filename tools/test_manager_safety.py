# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Manager filesystem/process/build safety guarantees (#183).

Two layers:

* the behavioral half runs ``tools/test_manager_safety.ps1``, which exercises the real
  safety primitives against temporary directories, junctions and mocked process
  identities (Windows only -- skipped elsewhere);
* the static half pins the fail-closed contracts in ``nk_manager.ps1`` that the
  behavioral tests cannot reach without a real game build: repository-root anchoring,
  OracleName grammar, unknown-exit build truth, and time-input validation.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "nk_manager.ps1"
SAFETY = ROOT / "tools" / "nk_safety.ps1"
PS_TESTS = ROOT / "tools" / "test_manager_safety.ps1"


def _powershell() -> str | None:
    return shutil.which("pwsh")


def _helper_calls(source: str, helper: str) -> list[list[str]]:
    """Named arguments of every ``helper ...`` command call in ``source``.

    PowerShell's grammar is not a regex, so the argument list is scanned rather than
    pattern-matched: a nested ``(Join-Path $root "x")`` is one *value*, and the ``-Path``
    inside it is not an argument of the outer call. Only the leading run of ``-Name value``
    pairs is returned.
    """
    calls: list[list[str]] = []
    for match in re.finditer(r"(?<![\w-])" + re.escape(helper) + r"(?![\w-])", source):
        i = match.end()
        n = len(source)
        named: list[str] = []
        while i < n:
            while i < n and source[i] in " \t":
                i += 1
            if i >= n or source[i] in ")\r\n;|":
                break
            arg = re.match(r"-([A-Za-z_][A-Za-z0-9_]*)", source[i:])
            if not arg:
                break
            named.append(arg.group(1))
            i += arg.end()
            while i < n and source[i] in " \t":
                i += 1
            if i < n and source[i] in "\r\n;|)":
                break
            if i < n and source[i] == "(":
                depth = 0
                while i < n:
                    if source[i] == "(":
                        depth += 1
                    elif source[i] == ")":
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                    i += 1
            elif i < n and source[i] in "\"'":
                quote = source[i]
                i += 1
                while i < n and source[i] != quote:
                    i += 2 if source[i] == "\\" else 1
                i += 1
            else:
                while i < n and source[i] not in " \t\r\n;|)":
                    i += 1
        if named:
            calls.append(named)
    return calls


class ManagerHelpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = MANAGER.read_text(encoding="utf-8-sig")
        match = re.search(r'\[ValidateSet\(([^)]+)\)\]\s*\[string\]\$Action', self.source)
        self.assertIsNotNone(match)
        self.actions = re.findall(r'"([^"]+)"', match.group(1))

    def test_help_actions_match_validateset(self) -> None:
        source = self.source
        match = re.search(r'\[ValidateSet\(([^)]+)\)\]\s*\[string\]\$Action', source)
        self.assertCountEqual(re.findall(r'"([^"]+)"', match.group(1)), self.actions)
        help_text = source.split("<#", 1)[1].split("#>", 1)[0]
        self.assertIn(".PARAMETER Action", help_text)
        entries = re.findall(r"^    (\w+) - (.+)$", help_text, re.MULTILINE)
        self.assertCountEqual([name for name, _ in entries], self.actions)
        self.assertIn("Makefile selftest target (C++ reference-runtime selftest)", dict(entries)["Test"])

    def test_setup_actions_match_validateset(self) -> None:
        setup = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")
        entries = re.findall(r"^\| `(\w+)` \| (.+) \|$", setup, re.MULTILINE)
        self.assertCountEqual([name for name, _ in entries], self.actions)
        self.assertIn("C++ reference-runtime selftest", dict(entries)["Test"])

    def test_no_action_output_matches_validateset(self) -> None:
        shell = _powershell()
        if shell is None:
            self.skipTest("no PowerShell interpreter on PATH")
        proc = subprocess.run(
            [shell, "-NoProfile", "-File", str(MANAGER)],
            cwd=ROOT, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        entries = re.findall(r"^  (\w+) - (.+)$", proc.stdout, re.MULTILINE)
        self.assertCountEqual([name for name, _ in entries], self.actions)
        self.assertIn("Makefile selftest target (C++ reference-runtime selftest)", dict(entries)["Test"])
        self.assertIn("Usage: pwsh -NoProfile -File nk_manager.ps1 -Action <action>", proc.stdout)
        self.assertIn("pwsh -NoProfile -File nk_manager.ps1 -Action BuildFast", proc.stdout)
        self.assertIn("pwsh -NoProfile -File nk_manager.ps1 -Action Run", proc.stdout)
        self.assertNotIn("[FATAL SCRIPT ERROR]", proc.stdout)
        self.assertIn("$MyInvocation.MyCommand.Parameters['Action'].Attributes", self.source)
        self.assertIn("[System.Management.Automation.ValidateSetAttribute]", self.source)
        self.assertIn("$actionChoices.ValidValues", self.source)


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
        self.support = SAFETY.read_text(encoding="utf-8-sig")

    def test_manager_anchors_to_its_own_script_location(self) -> None:
        # The canonical root is $PSScriptRoot, never the caller's CWD.
        # Phase 3 (#196): nk_manager uses Assert-NkWorkspaceRoot and nk_safety.ps1.
        self.assertIn("Assert-NkWorkspaceRoot -Root $PSScriptRoot", self.manager)
        self.assertIn("Set-Location -LiteralPath $script:RepoRoot", self.manager)
        self.assertIn("nk_safety.ps1", self.manager)
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

    def test_safety_helper_calls_only_use_declared_parameters(self) -> None:
        """Every named argument handed to a tools/nk_safety.ps1 helper must exist.

        PowerShell binds a call with an undeclared ``-Name`` at *runtime*, not at
        parse time, so a helper call that names a parameter the helper does not
        declare is invisible until an action that uses it runs -- and then it is a
        ``[FATAL SCRIPT ERROR]`` that aborts the whole manager. The OracleName
        validation passed ``-Label`` to ``Test-SafeComponentName``, which declares
        only ``-Name``: every ``-Action VisualOracle`` run on main died before it
        launched the game, and nothing in CI reached that code path. The
        fail-closed contract is that the binding is checked here instead.
        """
        safety = self.safety.replace("`\r\n", " ").replace("`\n", " ")
        manager = self.manager.replace("`\r\n", " ").replace("`\n", " ")

        declared: dict[str, set[str]] = {}
        for match in re.finditer(r"^function\s+([\w-]+)\s*\{", safety, re.MULTILINE):
            name = match.group(1)
            body = safety[match.end():]
            nxt = re.search(r"^function\s+[\w-]+\s*\{", body, re.MULTILINE)
            if nxt:
                body = body[:nxt.start()]
            param = re.search(r"\bparam\s*\(", body)
            params: set[str] = set()
            if param:
                depth, i = 0, param.end() - 1
                while i < len(body):
                    if body[i] == "(":
                        depth += 1
                    elif body[i] == ")":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                params = {
                    name
                    for name in re.findall(r"\$([A-Za-z_][A-Za-z0-9_]*)", body[param.end():i])
                    if name not in ("true", "false", "null")
                }
            declared[name] = params

        self.assertIn("Test-SafeComponentName", declared)
        for helper, params in sorted(declared.items()):
            if not params:
                continue
            for call in _helper_calls(manager, helper):
                for named in call:
                    self.assertIn(
                        named,
                        params,
                        f"nk_manager.ps1 calls {helper} -{named}, which {helper} does not declare "
                        f"(declared: {sorted(params)})",
                    )

    def test_snap_cleanup_is_workspace_anchored_and_file_scoped(self) -> None:
        self.assertIn('-Filter "snap_*.ppm" -File', self.manager)
        self.assertIn("$script:RepoRoot", self.manager)

    def test_save_sync_is_approved_root_contained_and_transactional(self) -> None:
        self.assertIn("Sync-SaveBase -BasePath $SaveBase", self.manager)
        self.assertIn("-ApprovedRoot $script:RepoRoot", self.manager)
        self.assertIn("-SaveRoot (Join-Path $script:RepoRoot", self.manager)
        # The transactional contract must live in the support file.
        self.assertIn(".nk_savebase_manifest.json", self.support)
        self.assertIn("Remove-SafeDirectory", self.support)
        self.assertIn("Failpoint", self.support)

    def test_build_success_requires_a_known_zero_exit(self) -> None:
        # The stale-exe fallback that turned a null exit code into success is gone.
        self.assertNotIn(
            "$makeExitCode = if (Test-Path $ExePath) { 0 } else { 1 }",
            self.manager,
            "null exit code must not fall back to executable existence",
        )
        self.assertIn("UNKNOWN (null exit code)", self.manager)
        self.assertIn("Get-KnownExitCode -Process $proc", self.manager)
        # The final build result is a tracked success, not Test-Path on the binary.
        self.assertIn("Write-NkBuildManifest", self.manager)
        self.assertNotIn("return (Test-Path $ExePath)", self.manager)

    def test_process_cleanup_requires_full_identity(self) -> None:
        self.assertIn("Get-ProcessIdentityRecord", self.manager)
        self.assertIn("Invoke-StaleBuildCleanup", self.manager)
        self.assertIn("creation_ticks", self.safety)
        self.assertIn("cmd", self.safety)
        self.assertIn("exe", self.safety)

    def test_duration_rejects_negative_values(self) -> None:
        self.assertIn("[ValidateRange(0, 2000000000)]\n    [int]$Duration = 0", self.manager)
        self.assertIn("ConvertTo-SafeTimeoutSeconds", self.safety)

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

    def test_manager_prebuild_and_prerun_fail_fast(self) -> None:
        self.assertIn('Missing required private build inputs', self.manager)
        self.assertIn('executable ELF (', self.manager)
        self.assertIn('no disc image found (declare filesystem.disc_image', self.manager)
        self.assertIn('declare filesystem.psp_header', self.manager)
        self.assertIn('declare filesystem.module_dir', self.manager)
        self.assertIn('Extracted asset tree was not found', self.manager)

    def test_manager_reads_the_declared_executable_input(self) -> None:
        # The executable location is a filesystem declaration like the other private
        # inputs; `executable.path` is not a manifest field and must not be consulted.
        self.assertIn("@{ Key = 'executable'; Var = 'TitleExecutable' }", self.manager)
        self.assertIn('$candidates += $script:TitleExecutable', self.manager)
        self.assertNotIn('$manifestJson.executable.path', self.manager)
        self.assertIn('=== NAKAGAWA RECOMP RUNTIME LAUNCH ===', self.manager)

    def test_runtime_exports_the_selected_manifest_data_root(self) -> None:
        # The native runtime rejects relative data roots. The manager must pass
        # the selected manifest root as an absolute inherited variable,
        # and must not retain a previous title's value when none is selected.
        self.assertIn('$resolvedDataRoot = Resolve-Path -LiteralPath $effectiveDataRoot', self.manager)
        self.assertIn('$env:SR_DATAROOT = $null', self.manager)
        self.assertIn('$env:SR_DATAROOT = $resolvedDataRoot.Path', self.manager)

    def test_generic_paths_make_no_layout_assumptions(self) -> None:
        self.assertNotIn("place_game_here", self.manager)
        self.assertNotIn("LegacyInputLayout", self.manager)

    def test_run_action_guards_against_killing_live_target_session(self) -> None:
        # P-005: Run must not kill a live session. It discovers running target processes
        # using full process identity (Get-ProcessIdentityRecord, canonical path), prints
        # a clear message with PID and advice to use -Action Clean, and exits non-zero
        # without launching.
        self.assertIn("Get-WorkspaceTargetProcesses", self.manager)
        self.assertIn("already running (PID", self.manager)
        self.assertIn("close it or use -Action Clean", self.manager)

        # Run-NkEngine must not call Stop-WorkspaceTarget before launching
        run_engine_body = self.manager[
            self.manager.index("function Run-NkEngine") : self.manager.index("function Analyze-RunLogs")
        ]
        self.assertNotIn("Stop-WorkspaceTarget", run_engine_body)
        self.assertIn("Get-WorkspaceTargetProcesses", run_engine_body)

        # Stop-WorkspaceTarget retains its clean-up capability using full process identity
        stop_target_body = self.manager[
            self.manager.index("function Stop-WorkspaceTarget") : self.manager.index("function Stop-BuildProcesses")
        ]
        self.assertIn("Get-WorkspaceTargetProcesses", stop_target_body)

        get_processes_body = self.manager[
            self.manager.index("function Get-WorkspaceTargetProcesses") : self.manager.index("function Stop-WorkspaceTarget")
        ]
        self.assertIn("Get-ProcessIdentityRecord", get_processes_body)
        self.assertIn("Get-CanonicalPath", get_processes_body)


if __name__ == "__main__":
    unittest.main()
