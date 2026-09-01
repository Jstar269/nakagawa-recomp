# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""PowerShell adapter contract tests that need no private title material.

`tools/title_manager_plan.ps1` is the seam where a validated Python plan becomes a
Make invocation. These tests drive it directly with source-owned public manifests
to prove three properties the manager depends on:

* the plan's build-facing projections are checked *against the plan's own
  semantics*, so PowerShell holds no second copy of any title contract;
* the protected digest is re-derived from the manifest on disk immediately before
  the build, so a manifest edited after planning fails closed;
* manifest paths containing spaces and shell metacharacters are passed as
  arguments, never interpreted.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan  # noqa: E402
import title_manifest  # noqa: E402

HELPER = ROOT / "tools" / "title_manager_plan.ps1"
PLANNER = ROOT / "tools" / "title_codegen_plan.py"
TITLES = ROOT / "assets" / "titles"

# Wholly synthetic spans. The adapter's job is to re-derive a projection from the
# plan's own numbers, so no real title's address range is needed here -- and using
# one would put a title-specific constant into a generic test surface.
SYNTHETIC_SPAN = (0x00400000, 0x00400100)
SYNTHETIC_SPAN_TEXT = "0x00400000,0x00400100"


class TitleManagerAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shell = shutil.which("pwsh")
        if cls.shell is None:
            raise unittest.SkipTest("PowerShell 7.6+ (pwsh) is required")

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-title-adapter-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest_path = TITLES / "synthetic.json"
        self.manifest = title_manifest.load_manifest(self.manifest_path)

    # --- helpers ----------------------------------------------------------

    def plan(self, **overrides) -> dict:
        values = {
            "game_name": "synthetic",
            "game_elf": Path("build/fixtures/synthetic.elf"),
            "build_dir": Path("build/synthetic"),
            "funcs_per_chunk": 64,
        }
        values.update(overrides)
        return title_codegen_plan.build_manager_plan(self.manifest, **values)

    def run_pwsh(self, body: str, plan: dict | None = None) -> subprocess.CompletedProcess:
        """Dot-source the adapter and run `body`, with `$plan` pre-bound if given."""
        plan_path = self.root / "plan.json"
        if plan is not None:
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
        script = "\n".join([
            "$ErrorActionPreference = 'Stop'",
            f". '{HELPER}'",
            (
                f"$plan = Get-Content -LiteralPath '{plan_path.as_posix()}' -Raw | ConvertFrom-Json"
                if plan is not None else ""
            ),
            "try {",
            body,
            "} catch { Write-Output \"THREW: $($_.Exception.Message)\"; exit 3 }",
        ])
        return subprocess.run(
            [self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )

    def assert_rejected(self, plan: dict, expected: str) -> None:
        proc = self.run_pwsh("Assert-TitleManagerPlan $plan | Out-Null", plan)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn(expected, proc.stdout)

    # --- plan validation --------------------------------------------------

    def test_a_freshly_planned_public_manifest_is_accepted(self) -> None:
        proc = self.run_pwsh(
            "Assert-TitleManagerPlan $plan | Out-Null\n"
            "Assert-TitlePlanDerivation $plan | Out-Null\n"
            "Write-Output 'ACCEPTED'",
            self.plan(),
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("ACCEPTED", proc.stdout)

    def test_a_plan_without_a_protected_digest_is_rejected(self) -> None:
        plan = self.plan()
        plan.pop("protected_digest")
        self.assert_rejected(plan, "missing required field(s): protected_digest")

    def test_a_malformed_protected_digest_is_rejected(self) -> None:
        for label, value in (
            ("uppercase", "A" * 64),
            ("short", "ab" * 16),
            ("non-hex", "z" * 64),
            ("empty", ""),
        ):
            with self.subTest(digest=label):
                plan = self.plan()
                plan["protected_digest"] = value
                proc = self.run_pwsh("Assert-TitleManagerPlan $plan | Out-Null", plan)
                self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)

    def test_an_unknown_plan_field_is_rejected(self) -> None:
        plan = self.plan()
        plan["surprise_setting"] = True
        self.assert_rejected(plan, "unknown field(s): surprise_setting")

    # --- derivation, not duplication --------------------------------------

    def test_a_projection_that_disagrees_with_the_plan_is_rejected(self) -> None:
        # Each case tampers with a build-facing projection while leaving the semantic
        # field it derives from intact. The adapter must notice without knowing which
        # title this is -- it re-derives, it does not compare against a stored copy.
        cases = (
            ("environment-base", lambda p: p["environment"].update(GAME_BASE="0x00000000"),
             "does not match the plan executable base/entry"),
            ("environment-entry", lambda p: p["environment"].update(GAME_ENTRY="0xdeadbeef"),
             "does not match the plan executable base/entry"),
            ("environment-span", lambda p: p["environment"].update(TITLE_EXTRA_SPANS=SYNTHETIC_SPAN_TEXT),
             "does not match the plan extra executable spans"),
            ("make-base", lambda p: p["make"].update(game_base="0"),
             "does not match the plan executable base/entry"),
            ("make-profile", lambda p: p["make"].update(codegen_profile_arg="--profile=hst"),
             "does not match the plan codegen profile"),
            ("make-name", lambda p: p["make"].update(game_name="somethingelse"),
             "does not match the plan game name"),
        )
        for label, tamper, expected in cases:
            with self.subTest(case=label):
                plan = self.plan()
                tamper(plan)
                proc = self.run_pwsh("Assert-TitlePlanDerivation $plan | Out-Null", plan)
                self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
                self.assertIn(expected, proc.stdout)

    def test_a_span_bearing_plan_projects_its_own_span(self) -> None:
        # A manifest that owns an extra span must project exactly that span, and the
        # projection is checked against the manifest's numbers, not a constant.
        owned = json.loads(json.dumps(self.manifest))
        # An extra span is only meaningful for an unrebased image, so this variant is
        # zero-based -- the same rule the analyzer enforces.
        owned["executable"]["base"] = 0
        owned["executable"]["entry"] = 0
        owned["executable"]["extra_executable_spans"] = [
            {"start": SYNTHETIC_SPAN[0], "end": SYNTHETIC_SPAN[1]}
        ]
        plan = title_codegen_plan.build_manager_plan(
            owned,
            game_name="synthetic",
            game_elf=Path("build/fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            funcs_per_chunk=64,
        )
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], SYNTHETIC_SPAN_TEXT)
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        proc = self.run_pwsh("Assert-TitlePlanDerivation $plan | Out-Null; Write-Output 'OK'", plan)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        # Clearing only the projection is drift, and is caught.
        plan["environment"]["TITLE_EXTRA_SPANS"] = ""
        proc = self.run_pwsh("Assert-TitlePlanDerivation $plan | Out-Null", plan)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)

    # --- time-of-check / time-of-use --------------------------------------

    def digest_check(self, plan: dict, manifest_path: Path) -> subprocess.CompletedProcess:
        body = (
            "Assert-TitleManifestDigest -Plan $plan "
            f"-PlannerScript '{PLANNER.as_posix()}' "
            f"-ManifestPath '{manifest_path.as_posix()}' "
            f"-PythonCommand '{Path(sys.executable).as_posix()}' | Out-Null\n"
            "Write-Output 'DIGEST-OK'"
        )
        return self.run_pwsh(body, plan)

    def test_digest_check_passes_for_an_unchanged_manifest(self) -> None:
        proc = self.digest_check(self.plan(), self.manifest_path)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DIGEST-OK", proc.stdout)

    def test_a_manifest_edited_after_planning_fails_closed(self) -> None:
        staged = self.root / "synthetic.json"
        staged.write_text(self.manifest_path.read_text(encoding="utf-8"), encoding="utf-8")
        plan = self.plan()
        mutated = json.loads(staged.read_text(encoding="utf-8"))
        mutated["executable"]["base"] = 0x08804000
        mutated["executable"]["entry"] = 0x08804000
        staged.write_text(json.dumps(mutated, indent=2), encoding="utf-8")
        proc = self.digest_check(plan, staged)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("title manifest changed after planning", proc.stdout)

    def test_a_notes_only_edit_after_planning_is_allowed(self) -> None:
        staged = self.root / "synthetic.json"
        staged.write_text(self.manifest_path.read_text(encoding="utf-8"), encoding="utf-8")
        plan = self.plan()
        edited = json.loads(staged.read_text(encoding="utf-8"))
        edited["notes"] = "prose clarification added while the build was queued"
        staged.write_text(json.dumps(edited, indent=4), encoding="utf-8")
        proc = self.digest_check(plan, staged)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DIGEST-OK", proc.stdout)

    def test_a_missing_manifest_fails_closed(self) -> None:
        proc = self.digest_check(self.plan(), self.root / "absent.json")
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("no longer readable", proc.stdout)

    def test_manifest_paths_with_spaces_and_metacharacters_are_not_interpreted(self) -> None:
        hostile = self.root / "title dir; echo pwned & $(exit 7)"
        hostile.mkdir()
        staged = hostile / "syn thetic.json"
        staged.write_text(self.manifest_path.read_text(encoding="utf-8"), encoding="utf-8")
        proc = self.digest_check(self.plan(), staged)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DIGEST-OK", proc.stdout)
        self.assertNotIn("pwned", proc.stdout)


class RunEntryIsPlanOwned(unittest.TestCase):
    """The address a run starts at must come from validated title configuration.

    hst_manager.ps1 carried a bare `0x0029a060` at its Run and DiffFunc call sites --
    the same value the manifest already owns as runtime_bindings.fallback_entry, and
    the same one src/rt/title_config.c compiles in. Three copies of one title address,
    only one of them validated.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.shell = shutil.which("pwsh")
        if cls.shell is None:
            raise unittest.SkipTest("PowerShell 7.6+ (pwsh) is required")

    def build(self, manifest: dict) -> dict:
        return title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic",
            game_elf=Path("build/fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            funcs_per_chunk=64,
        )

    def manifest(self) -> dict:
        return title_manifest.load_manifest(TITLES / "synthetic.json")

    def test_run_entry_is_the_configured_fallback_entry(self) -> None:
        manifest = self.manifest()
        fallback = manifest["runtime_bindings"]["fallback_entry"]
        self.assertNotEqual(fallback, manifest["executable"]["entry"],
                            "fixture cannot distinguish the two if they are equal")
        self.assertEqual(self.build(manifest)["run_entry"], f"0x{fallback:08x}")

    def test_without_runtime_bindings_it_is_the_executable_entry(self) -> None:
        """Not a title default: with no configured fallback the ELF entry is the only
        thing a generic title can be started at."""
        manifest = self.manifest()
        manifest.pop("runtime_bindings", None)
        entry = manifest["executable"]["entry"]
        expected = "0" if entry == 0 else f"0x{entry:08x}"
        self.assertEqual(self.build(manifest)["run_entry"], expected)

    def test_a_changed_fallback_entry_changes_the_run_entry(self) -> None:
        """Vacuity guard: the projection must track the binding, not a constant."""
        manifest = self.manifest()
        manifest["runtime_bindings"]["fallback_entry"] += 8
        self.assertEqual(self.build(manifest)["run_entry"],
                         f"0x{manifest['runtime_bindings']['fallback_entry']:08x}")

    def test_the_adapter_accepts_and_surfaces_it(self) -> None:
        plan = self.build(self.manifest())
        plan_path = Path(tempfile.mkdtemp(prefix="nakagawa-run-entry-")) / "plan.json"
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        script = "\n".join([
            "$ErrorActionPreference = 'Stop'",
            f". '{HELPER}'",
            f"$plan = Get-Content -LiteralPath '{plan_path.as_posix()}' -Raw | ConvertFrom-Json",
            "Assert-TitleManagerPlan $plan | Out-Null",
            "Assert-TitlePlanDerivation $plan | Out-Null",
            "Write-Output \"RUN_ENTRY=$($plan.run_entry)\"",
        ])
        proc = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
                              cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(f"RUN_ENTRY={plan['run_entry']}", proc.stdout)

    def test_a_malformed_run_entry_is_rejected(self) -> None:
        for label, value in (("uppercase", "0X0029A060"), ("short", "0x29a060"),
                             ("decimal", "2728032"), ("empty", ""), ("garbage", "nope")):
            with self.subTest(case=label):
                plan = self.build(self.manifest())
                plan["run_entry"] = value
                plan_path = Path(tempfile.mkdtemp(prefix="nakagawa-run-entry-")) / "plan.json"
                plan_path.write_text(json.dumps(plan), encoding="utf-8")
                script = "\n".join([
                    "$ErrorActionPreference = 'Stop'",
                    f". '{HELPER}'",
                    f"$plan = Get-Content -LiteralPath '{plan_path.as_posix()}' -Raw | ConvertFrom-Json",
                    "try { Assert-TitleManagerPlan $plan | Out-Null }",
                    "catch { Write-Output \"THREW: $($_.Exception.Message)\"; exit 3 }",
                ])
                proc = subprocess.run(
                    [self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
                    cwd=ROOT, capture_output=True, text=True, check=False)
                self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
                self.assertIn("run_entry", proc.stdout)

    def test_a_missing_run_entry_is_rejected(self) -> None:
        """The manager must not silently fall back when the planner stops projecting it."""
        plan = self.build(self.manifest())
        plan.pop("run_entry")
        plan_path = Path(tempfile.mkdtemp(prefix="nakagawa-run-entry-")) / "plan.json"
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        script = "\n".join([
            "$ErrorActionPreference = 'Stop'",
            f". '{HELPER}'",
            f"$plan = Get-Content -LiteralPath '{plan_path.as_posix()}' -Raw | ConvertFrom-Json",
            "try { Assert-TitleManagerPlan $plan | Out-Null }",
            "catch { Write-Output \"THREW: $($_.Exception.Message)\"; exit 3 }",
        ])
        proc = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
                              cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("run_entry", proc.stdout)


class ManagerHoldsNoRunEntryCopy(unittest.TestCase):
    """Source-shape guard on hst_manager.ps1. Tier 4, and it does not pretend
    otherwise -- it asserts where the value comes from, not that a run works."""

    def setUp(self) -> None:
        self.manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8")

    def test_the_run_and_difffunc_call_sites_take_it_from_the_plan(self) -> None:
        sites = [line for line in self.manager.splitlines() if '"--image", $imagePath' in line]
        self.assertGreaterEqual(len(sites), 2, "the driver invocation sites moved")
        for line in sites:
            self.assertIn("(Get-HstRunEntry)", line,
                          f"driver invocation still carries its own entry: {line.strip()}")
            self.assertNotIn("0x0029a060", line)

    def test_the_only_remaining_literal_is_the_legacy_fallback(self) -> None:
        """One copy remains, in one place, and it announces itself when used. Pinning
        the count is what stops a new one being added quietly."""
        self.assertEqual(self.manager.count("0x0029a060"), 1)
        body = self.manager.split("function Get-HstRunEntry", 1)[1].split("\n    function ", 1)[0]
        self.assertIn("0x0029a060", body)
        self.assertIn("Write-Host", body, "using the legacy literal must be reported")

    def test_the_plan_supplied_entry_wins_when_a_manifest_is_bound(self) -> None:
        body = self.manager.split("function Get-HstRunEntry", 1)[1].split("\n    function ", 1)[0]
        guard = body.index("$script:TitleManagerRunEntry")
        self.assertLess(guard, body.index("0x0029a060"),
                        "the literal must only be reached when no plan supplied one")
        self.assertIn("$script:TitleManagerRunEntry = $boundPlan.RunEntry", self.manager)

if __name__ == "__main__":
    unittest.main()
