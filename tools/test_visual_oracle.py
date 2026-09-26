# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""Visual-oracle runner guarantees.

Two layers:

* the behavioral half runs ``tools/test_visual_oracle.ps1``, which exercises the real
  helpers against real processes and directories (Windows only -- skipped elsewhere);
* the static half pins the contract in ``nk_manager.ps1`` that those helpers exist to
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
MANAGER = ROOT / "nk_manager.ps1"
SUPPORT = ROOT / "tools" / "nk_safety.ps1"
PS_TESTS = ROOT / "tools" / "test_visual_oracle.ps1"
HLE = ROOT / "src" / "rt" / "hle.c"


def _powershell() -> str | None:
    return shutil.which("pwsh")


def _paren_list(text: str, open_at: int) -> str:
    """Return the contents of the (...) whose opening paren is at `open_at`."""
    depth = 0
    i = open_at
    quote = ""
    while i < len(text):
        c = text[i]
        if quote:
            if c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
        elif c == "#":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[open_at + 1 : i]
        i += 1
    return ""


def _declared_parameters(source: str) -> dict[str, set[str]]:
    """Map every PowerShell function in `source` to the parameters its param() block declares."""
    declared: dict[str, set[str]] = {}
    for match in re.finditer(r"^function\s+([A-Za-z][\w-]*)", source, re.M):
        name = match.group(1)
        end = source.find("\n}\n", match.end())
        body = source[match.end() : end if end > 0 else len(source)]
        open_at = re.search(r"(?m)^[ \t]*param[ \t]*\(", body)
        if not open_at:
            declared[name] = set()
            continue
        params: set[str] = set()
        for argument in _paren_list(body, body.index("(", open_at.start())).split(","):
            names = re.findall(r"\$(\w+)", re.sub(r"\[[^\]]*\]", "", argument))
            params.update(names)
        declared[name] = params
    return declared


def _call_segments(source: str, names: set[str]) -> list[tuple[str, str]]:
    """Yield (helper name, argument text) for every `Helper -Argument` call in `source`."""
    segments: list[tuple[str, str]] = []
    for match in re.finditer(r"\b(" + "|".join(sorted(map(re.escape, names))) + r")\b", source):
        text = match.end()
        while True:
            newline = source.find("\n", text)
            line = source[text : newline if newline >= 0 else len(source)]
            segments.append((match.group(1), line))
            if not line.rstrip().endswith("`") or newline < 0:
                break
            text = newline + 1
    return segments


def _named_arguments(segment: str) -> set[str]:
    """Return the -Named arguments in one call segment, ignoring strings and comments."""
    line = re.sub(r"#.*$", "", segment, flags=re.M)
    line = re.sub(r"\"[^\"]*\"|'[^']*'", " ", line)
    return set(re.findall(r"(?<![\w-])-([A-Za-z]\w*)\s*:?\s", line))


class VisualOracleBehaviorTests(unittest.TestCase):
    """Run the PowerShell helper tests, which need real processes to be meaningful."""

    @unittest.skipUnless(sys.platform == "win32", "PowerShell helpers are Windows-only")
    def test_safety_and_run_support_helpers(self) -> None:
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

    def test_run_support_is_owned_by_the_canonical_safety_module(self) -> None:
        self.assertIn("nk_safety.ps1", self.manager)
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
        self.assertIn("Run-NkEngine -Profile $RunProfile", oracle)
        self.assertNotIn(
            "Start-Process",
            oracle,
            "the oracle must go through the engine runner, not launch its own runner",
        )

    def test_manager_binds_only_declared_helper_parameters(self) -> None:
        # A helper called with a parameter it does not declare is a runtime parameter-binding
        # error, not a validation failure: PowerShell aborts the whole script, so the action
        # dies on its first call with a message that names neither the caller nor the action.
        # -Action VisualOracle shipped that way since #196, which left the only scripted-input
        # route in the product unusable.
        declared = _declared_parameters(self.support)
        self.assertIn("Test-SafeComponentName", declared)
        unknown = [
            f"{name} -{param}"
            for name, segment in _call_segments(self.manager, set(declared))
            for param in _named_arguments(segment)
            if param not in declared[name]
        ]
        self.assertEqual(
            [],
            unknown,
            "nk_manager.ps1 binds parameters its tools/nk_safety.ps1 helpers do not declare: "
            + ", ".join(sorted(set(unknown))),
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
