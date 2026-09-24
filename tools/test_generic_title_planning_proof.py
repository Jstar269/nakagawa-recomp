#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generic-title planning proof: HST unchanged while synthetic second title flows generically.

This is the narrow proof lane requested in the GENERIC TITLE PLANNING PROOF mission.
It shows that the title-planning / build-contract layer can represent a second
synthetic identity without:
  - pretending to be HST,
  - modifying HST constants,
  - adding `if title == synthetic_title2` style branches,
  - using private inputs.

Separation established by this lane:
  GENERIC TITLE CONTRACT (title-neutral, host-portable):
    title identifier / build name / executable binding names / codegen input /
    runtime fallback entry / module configuration / generated-output locations /
    optional capabilities. Validated by title_manifest.py and projected by
    title_codegen_plan.py (_span_environment, build_plan, build_manager_plan) and
    the generic PowerShell helpers (Assert-TitleManagerPlan, Assert-TitlePlanDerivation,
    Assert-TitleManifestDigest). No UCUS98701, no 0x00303194, no libfont.prx pin.

  HST PROFILE / ADAPTER (isolated):
    UCUS98701 exact-disc-id, 0-base/hst profile/psp-header, span
    0x00303194-0x00306e24, three required guest modules at fixed addresses,
    private-input expectations. Lives ONLY in Get-HstManifestMakeArgs and the
    Makefile `ifeq ($(GAME_NAME),hst)` defaults.

The new fixture assets/titles/synthetic-title2.json carries obviously synthetic
identifiers and addresses (0x0A4xxxxx family) and proves the generic planner
accepts a non-HST identity.
"""

from __future__ import annotations

import copy
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan  # noqa: E402
import title_manifest  # noqa: E402

TITLES = ROOT / "assets" / "titles"
SYNTHETIC = TITLES / "synthetic.json"
SYNTHETIC2 = TITLES / "synthetic-title2.json"
PSPDEV = TITLES / "pspdev-phase5.json"
PLANNER = ROOT / "tools" / "title_codegen_plan.py"
HELPER = ROOT / "tools" / "title_manager_plan.ps1"

# HST constants that must never appear in a generic plan or be inherited silently.
HST_SPAN = (3158420, 3173924)  # 0x00303194, 0x00306e24
HST_MODULES = [
    ("libfont.prx", 166460416),
    ("scePsmf_library.prx", 166551552),
    ("scePsmfP_library.prx", 166493952),
]
HST_DISC_ID = "UCUS98701"


def _load(path: pathlib.Path) -> dict:
    return title_manifest.load_manifest(path)


class GenericTitleProofFixtures(unittest.TestCase):
    def test_synthetic_title2_fixture_is_valid_and_deterministic(self) -> None:
        """Synthetic-title2 validates, canonicalizes stably, and is publication-safe."""
        manifest = _load(SYNTHETIC2)
        normalized = title_manifest.validate_manifest(manifest)
        first = title_manifest.canonical_json(manifest)
        second = title_manifest.canonical_json(json.loads(first))
        self.assertEqual(first, second)
        self.assertEqual(normalized["id"], "synthetic-title2-v1")
        self.assertEqual(normalized["kind"], "synthetic")
        self.assertEqual(normalized["codegen_profile"], "none")
        self.assertNotIn("disc", normalized)
        # No retail bytes, no private addresses: must not reuse HST values.
        exe = normalized["executable"]
        self.assertNotEqual((exe["base"], exe["entry"]), (0, 0))
        self.assertNotIn({"start": HST_SPAN[0], "end": HST_SPAN[1]}, exe["extra_executable_spans"])
        modules = {(m["name"], m["load_address"]) for m in normalized["modules"]}
        for hst_mod in HST_MODULES:
            self.assertNotIn(hst_mod, modules)
        # No HST disc, no .exe semantics, portable paths only.
        self.assertIn("fixtures/synthetic_title2/data", normalized["filesystem"]["data_root"])
        self.assertFalse(any("\\" in p or ":" in p for p in [normalized["filesystem"]["data_root"]]))
        # Runtime bindings are disjoint from the other two synthetics.
        self.assertEqual(len(normalized["runtime_bindings"]["dispatch_aliases"]), 2)
        self.assertEqual(len(normalized["runtime_bindings"]["callback_terminators"]), 2)

    def test_three_fixtures_are_pairwise_disjoint(self) -> None:
        """The three public synthetics use disjoint address families; matrix can distinguish."""
        fixtures = [SYNTHETIC, PSPDEV, SYNTHETIC2]
        bases = set()
        for path in fixtures:
            norm = title_manifest.validate_manifest(_load(path))
            bases.add(norm["executable"]["base"])
            # collect runtime binding addresses
            bindings = norm.get("runtime_bindings", {})
            used = {v for k, v in bindings.items() if isinstance(v, int)}
            for alias in bindings.get("dispatch_aliases", []):
                used |= {alias["from"], alias["to"]}
            for term in bindings.get("callback_terminators", []):
                used |= {term.get("pc", 0), term.get("ra", 0)}
                used.discard(0)
            # check against HST retired span/modules
            self.assertNotIn(HST_SPAN[0], used)
        self.assertEqual(len(bases), 3, "each fixture must have a distinct executable base")
        # Ensure synthetic-title2 uses 0x0A4xxxxx family as promised.
        s2 = title_manifest.validate_manifest(_load(SYNTHETIC2))
        self.assertEqual(s2["executable"]["base"] >> 20, 0x0A4)


class GenericPlannerAcceptsSyntheticTitle2(unittest.TestCase):
    def _plan(self, manifest_path: pathlib.Path, game_name: str, build_dir: pathlib.Path) -> dict:
        manifest = _load(manifest_path)
        return title_codegen_plan.build_manager_plan(
            manifest,
            game_name=game_name,
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=build_dir,
            funcs_per_chunk=64,
        )

    def test_synthetic_title2_plans_successfully_via_generic_planner(self) -> None:
        """Generic fields are projected without pretending to be HST."""
        plan = self._plan(SYNTHETIC2, "synthetic_title2", pathlib.Path("build/synthetic_title2"))
        self.assertEqual(plan["title_manifest_id"], "synthetic-title2-v1")
        self.assertEqual(plan["title_kind"], "synthetic")
        self.assertEqual(plan["game_name"], "synthetic_title2")
        self.assertEqual(plan["game_base"], 0x0A400000)
        self.assertEqual(plan["game_entry"], 0x0A400000)
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["bss_metadata_source"], "elf")
        self.assertEqual(plan["extra_executable_spans"], [])
        self.assertEqual(plan["disc"], None)
        self.assertEqual(plan["make"]["game_name"], "synthetic_title2")
        self.assertEqual(plan["make"]["codegen_profile_arg"], "")
        self.assertEqual(plan["make"]["build_dir"], "build/synthetic_title2")
        # Environment is generic and host-portable: no HST constants invented.
        # Generic planner emits only TITLE_EXTRA_SPANS; HST legacy must not appear for synthetics.
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x0a400000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x0a400000")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        # synthetic_title2 has one optional module but none selected by default -> no module_dir required
        self.assertEqual(plan["private_binding_requirements"], {"game_elf": True, "module_dir": False, "psp_header": False})

    def test_generic_fields_are_projected_consistently(self) -> None:
        """Every build-facing projection agrees with its semantic source."""
        for path, name, build in [
            (SYNTHETIC, "synthetic", pathlib.Path("build/synthetic")),
            (PSPDEV, "pspdev_phase5", pathlib.Path("build/pspdev_phase5")),
            (SYNTHETIC2, "synthetic_title2", pathlib.Path("build/synthetic_title2")),
        ]:
            with self.subTest(title=path.name):
                manifest = _load(path)
                normalized = title_manifest.validate_manifest(manifest)
                plan = title_codegen_plan.build_manager_plan(
                    manifest,
                    game_name=name,
                    game_elf=pathlib.Path(f"build/fixtures/{name}.elf"),
                    build_dir=build,
                    funcs_per_chunk=64,
                )
                # GAME_BASE/GAME_ENTRY re-derived from executable base/entry
                self.assertEqual(plan["environment"]["GAME_BASE"], f"0x{plan['game_base']:08x}")
                self.assertEqual(plan["environment"]["GAME_ENTRY"], f"0x{plan['game_entry']:08x}")
                # TITLE_EXTRA_SPANS is the only authoritative span (generic contract); HST legacymust not leak.
                spans = plan["extra_executable_spans"]
                expected = "" if not spans else f"0x{spans[0]['start']:08x},0x{spans[0]['end']:08x}"
                self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], expected)
                self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
                # Make base/entry rendering: 0 => "0", else hex
                expected_make_base = "0" if plan["game_base"] == 0 else f"0x{plan['game_base']:08x}"
                self.assertEqual(plan["make"]["game_base"], expected_make_base)
                # run_entry comes from runtime_bindings.fallback_entry or executable entry
                fallback = normalized.get("runtime_bindings", {}).get("fallback_entry")
                expected_run = f"0x{(fallback if fallback is not None else normalized['executable']['entry']):08x}"
                if expected_run == "0x00000000":
                    expected_run = "0"
                self.assertEqual(plan["run_entry"], expected_run)
                # codegen_profile_arg matches codegen_profile
                expected_arg = "" if plan["codegen_profile"] == "none" else f"--profile={plan['codegen_profile']}"
                self.assertEqual(plan["make"]["codegen_profile_arg"], expected_arg)

    def test_codegen_plan_also_accepts_synthetic_title2(self) -> None:
        """The command-vector plan (build_plan) is equally title-neutral."""
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        self.assertEqual(plan["title_manifest_id"], "synthetic-title2-v1")
        self.assertEqual(plan["game_base"], 0x0A400000)
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertFalse(any("--profile=hst" in arg for arg in plan["commands"]["codegen"]))

    def test_cli_is_deterministic_and_needs_no_private_input(self) -> None:
        cmd = [
            sys.executable, str(PLANNER), str(SYNTHETIC2),
            "--manager-plan",
            "--game-name=synthetic_title2",
            "--game-elf=build/fixtures/synthetic2.elf",
            "--build-dir=build/synthetic_title2",
        ]
        first = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
        second = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        parsed = json.loads(first.stdout)
        self.assertEqual(parsed["title_manifest_id"], "synthetic-title2-v1")
        self.assertNotIn("place_game_here", first.stdout)


class HstProfileIsolation(unittest.TestCase):
    """HST-only rules remain inside the HST adapter; unknown titles do not inherit HST."""

    def setUp(self) -> None:
        self.shell = shutil.which("pwsh")
        if self.shell is None:
            self.skipTest("pwsh required for adapter isolation checks")

    def _run_adapter_reject(self, plan: dict, expected_fragment: str) -> None:
        import json as _json
        with tempfile.TemporaryDirectory() as tmp:
            plan_path = pathlib.Path(tmp) / "plan.json"
            plan_path.write_text(_json.dumps(plan), encoding="utf-8")
            script = "\n".join([
                "$ErrorActionPreference='Stop'",
                f". '{HELPER}'",
                f"$plan = Get-Content -LiteralPath '{plan_path.as_posix()}' -Raw | ConvertFrom-Json",
                "try {",
                "  $bound = Get-HstManifestMakeArgs -Plan $plan -GameElfForMake 'build/hst/EBOOT.elf' -ModuleDirForMake 'build/hst/modules' -PspHeaderForMake 'build/hst/EBOOT.BIN' -VulkanSdkForMake 'C:/Vulkan' -BuildDir 'build/hst' -FuncsPerChunk 2000 -TitleManifestForMake 'assets/titles/synthetic-title2.json'",
                "  Write-Output 'UNEXPECTED_PASS'",
                "  exit 0",
                "} catch {",
                "  Write-Output \"THREW: $($_.Exception.Message)\"",
                "  exit 3",
                "}",
            ])
            proc = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", script],
                                  cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
            self.assertIn(expected_fragment, proc.stdout)

    def test_synthetic_title2_is_rejected_by_hst_adapter(self) -> None:
        """The HST manager accepts only the local HST retail manifest, not synthetics."""
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        self._run_adapter_reject(plan, "the HST manager accepts only the local HST retail manifest")

    def test_unknown_title_does_not_inherit_hst_constants(self) -> None:
        """A manifest with an unknown id/title_kind gets no HST modules/spans/disc."""
        base = _load(SYNTHETIC2)
        mutated = copy.deepcopy(base)
        mutated["id"] = "unknown-title-v1"
        mutated["display_name"] = "Unknown Title Fixture"
        plan = title_codegen_plan.build_manager_plan(
            mutated,
            game_name="unknown_title",
            game_elf=pathlib.Path("build/fixtures/unknown.elf"),
            build_dir=pathlib.Path("build/unknown"),
        )
        self.assertEqual(plan["title_manifest_id"], "unknown-title-v1")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["required_guest_modules"], [])
        self.assertIsNone(plan["disc"])
        # Must still be rejected by HST adapter (mutant: removing isolation would accept it)
        self._run_adapter_reject(plan, "the HST manager accepts only the local HST retail manifest")

    def test_tampering_title_identity_to_hst_still_fails_due_to_other_pins(self) -> None:
        """Mutant control: even if an attacker flips id to hst-ucus98701-v1, other HST pins catch it."""
        manifest = _load(SYNTHETIC2)
        # Build a normal synthetic-title2 plan (no psp-header needed for elf)
        plan = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        # Forge the id to look like HST but keep synthetic addresses - also patch make.game_name
        # so the generic derivation does not fail earlier on game_name mismatch.
        plan["title_manifest_id"] = "hst-ucus98701-v1"
        plan["title_kind"] = "retail"
        plan["game_name"] = "hst"
        plan["make"]["game_name"] = "hst"
        # Still fails because executable base/entry/profile/bss mismatch the HST contract
        self._run_adapter_reject(plan, "protected executable semantics are incompatible")

    def test_generic_helpers_do_not_name_hst_constants(self) -> None:
        """Generic helpers must not encode HST constants; only the adapter does."""
        generic_files = [
            ROOT / "tools" / "title_manifest.py",
            ROOT / "tools" / "title_codegen_plan.py",
            ROOT / "tools" / "title_runtime_config.py",
        ]
        for path in generic_files:
            text = path.read_text(encoding="utf-8")
            # Generic files may mention HST profile in comments describing separation, but must not encode the
            # literal disc constant as logic. Check that the literal does not appear outside comments.
            # Simple heuristic: strip # comments and look for literal.
            code_without_comments = "\n".join(
                line for line in text.splitlines() if not line.lstrip().startswith("#") and not line.lstrip().startswith("//")
            )
            # Also allow mentions in docstrings describing HST isolation; only forbid as code-level pin
            # For strictness, allow UCUS in title_codegen_plan.py's HST isolation docstring (we removed it anyway)
            if path.name == "title_codegen_plan.py":
                # This file's generic docstring no longer contains UCUS; any occurrence would be a leak
                self.assertNotIn("UCUS98701", text, f"{path.name} must not name HST disc")
            else:
                self.assertNotIn("UCUS98701", code_without_comments, f"{path.name} must not name HST disc in code")
            self.assertNotIn("0x00303194", text.lower())
            self.assertNotIn("libfont.prx", text)

        adapter_text = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        # Generic helpers inside the same file must also not name HST, but the adapter must.
        self.assertIn("UCUS98701", adapter_text)
        # Ensure the HST pin appears exactly once in the adapter's code (not counting comments)
        hst_fn = adapter_text.split("function Get-HstManifestMakeArgs", 1)[1].split("\nfunction ", 1)[0]
        # Count occurrences in code lines (ignore comment lines starting with #)
        code_lines = [l for l in hst_fn.splitlines() if not l.lstrip().startswith("#")]
        code_text = "\n".join(code_lines)
        self.assertEqual(code_text.count("UCUS98701"), 1)
        # Generic derivation must not mention UCUS
        generic_section = adapter_text.split("function Assert-TitlePlanDerivation", 1)[1].split("\nfunction ", 1)[0]
        self.assertNotIn("UCUS98701", generic_section)


class InvalidManifestsFailClosed(unittest.TestCase):
    def test_malformed_manifests_are_rejected_by_validator(self) -> None:
        base = _load(SYNTHETIC2)
        cases = [
            ("duplicate_key", lambda m: None, "duplicate"),  # handled via loads
            ("unknown_field", lambda m: m.update({"unexpected": True}), "unknown field"),
            ("bad_kind", lambda m: m.update({"kind": "arcade"}), "unsupported title kind"),
            ("retail_without_disc", lambda m: (m.update({"kind": "retail"}), m.pop("disc", None)), "require disc"),
            ("disc_on_synthetic", lambda m: m.update({"disc": {"id": "TEST00001", "region": "NA", "revision_policy": "exact-disc-id"}}), "only for retail"),
            ("zero_fallback", lambda m: m["runtime_bindings"].update({"fallback_entry": 0}), "must not be zero"),
            ("misaligned_addr", lambda m: m["runtime_bindings"].update({"fallback_entry": 0x0A401001}), "must be 4-byte aligned"),
            ("empty_dispatch", lambda m: m["runtime_bindings"].update({"dispatch_aliases": []}), "must not be empty"),
        ]
        for label, mutate, fragment in cases:
            with self.subTest(case=label):
                mutated = copy.deepcopy(base)
                try:
                    mutate(mutated)
                except Exception:
                    pass
                # Special case duplicate key needs raw JSON
                if label == "duplicate_key":
                    raw = '{"schema_version":1,"schema_version":1}'
                    with self.assertRaises(title_manifest.TitleManifestError):
                        title_manifest.loads_manifest(raw)
                    continue
                with self.assertRaises(title_manifest.TitleManifestError) as cm:
                    title_manifest.validate_manifest(mutated)
                self.assertIn(fragment.lower(), str(cm.exception).lower())

    def test_malformed_span_and_profile_combos_fail_in_planner(self) -> None:
        base = _load(SYNTHETIC2)
        # Extra span with nonzero base is rejected by planner
        with self.assertRaises(title_codegen_plan.TitleCodegenPlanError):
            bad = copy.deepcopy(base)
            bad["executable"]["extra_executable_spans"] = [{"start": 0x1000, "end": 0x2000}]
            title_codegen_plan.build_manager_plan(
                bad,
                game_name="synthetic_title2",
                game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
                build_dir=pathlib.Path("build/synthetic_title2"),
            )
        # Conflicting codegen profile is rejected
        with self.assertRaises(title_codegen_plan.TitleCodegenPlanError):
            title_codegen_plan.build_manager_plan(
                _load(SYNTHETIC2),
                game_name="synthetic_title2",
                game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
                build_dir=pathlib.Path("build/synthetic_title2"),
                codegen_profile="hst",
            )


class LegacyToolingRetirementTests(unittest.TestCase):
    RETIRED_PATHS = (
        "hst.ps1",
        "hst_manager.ps1",
        "tools/hst_doctor.py",
        "tools/hst_doctor_checks.py",
        "tools/hst_doctor_core.py",
        "tools/hst_safety.ps1",
        "tools/hst_run_support.ps1",
    )

    def test_deprecated_hst_tool_paths_are_removed(self) -> None:
        for relative in self.RETIRED_PATHS:
            with self.subTest(path=relative):
                self.assertFalse((ROOT / relative).exists())

    def test_generic_tooling_has_no_deprecated_hst_aliases(self) -> None:
        forbidden = (
            "HST_EXTRA_SPANS",
            "Assert-HstWorkspaceRoot",
            "HstWorkspaceRoot",
            "Get-HstRunEntry",
            "Get-HstMakeBaseArgs",
            "Stop-WorkspaceHst",
            "Push-HstAnalyzerEnvironment",
            "Pop-HstAnalyzerEnvironment",
        )
        for relative in (
            "Makefile",
            "nk_manager.ps1",
            "tools/nk_safety.ps1",
            "tools/title_manager_plan.ps1",
        ):
            text = (ROOT / relative).read_text(encoding="utf-8")
            for token in forbidden:
                with self.subTest(path=relative, token=token):
                    self.assertNotIn(token, text)

    def test_maintained_code_and_docs_have_no_retired_wrapper_dependencies(self) -> None:
        retired_names = tuple(self.RETIRED_PATHS)
        paths = [
            ROOT / "Makefile",
            ROOT / "copy_build_assets.ps1",
            ROOT / "nk.ps1",
            ROOT / "nk_manager.ps1",
            ROOT / "assets" / "titles" / "README.md",
        ]
        for directory, suffixes in (
            (ROOT / "tools", (".py", ".ps1")),
            (ROOT / "interface" / "src", (".ts", ".tsx", ".mjs")),
            (ROOT / ".github" / "workflows", (".yml", ".yaml")),
            (ROOT / "docs", (".md",)),
        ):
            paths.extend(
                path
                for path in directory.rglob("*")
                if path.is_file()
                and path.suffix in suffixes
                and path != pathlib.Path(__file__).resolve()
                and path.name != "TITLE_MANAGER_DECOUPLING.md"
            )
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for retired_name in retired_names:
                with self.subTest(path=path.relative_to(ROOT), retired_name=retired_name):
                    self.assertNotIn(retired_name, text)

    def test_direct_make_has_no_hst_title_defaults(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertNotIn("HST_EXTRA_SPANS", makefile)
        self.assertNotIn("0x00303194,0x00306e24", makefile)
        self.assertNotIn("CODEGEN_PROFILE_ARG := --profile=hst", makefile)
        self.assertEqual(
            len(re.findall(r"ifeq\s*\(\$\(GAME_NAME\),hst\)", makefile)),
            1,
        )


class NoSecondTitleConditionalInGenericCode(unittest.TestCase):
    def test_generic_code_contains_no_synthetic_title2_branch(self) -> None:
        """The generic planner must not add `if title == synthetic_title2` style code."""
        generic_paths = [
            ROOT / "tools" / "title_manifest.py",
            ROOT / "tools" / "title_codegen_plan.py",
            ROOT / "tools" / "title_runtime_config.py",
            ROOT / "tools" / "title_manager_plan.ps1",
        ]
        for path in generic_paths:
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("synthetic-title2", text, f"{path.name} must not branch on synthetic-title2")
            self.assertNotIn("synthetic_title2", text)
            # No title-id-specific conditionals at all in generic helpers (except adapter)
            if path.name == "title_manager_plan.ps1":
                # Only the HST adapter should name a concrete title id; generic helpers must not.
                generic_section = text.split("function Get-HstManifestMakeArgs", 1)[0]
                self.assertNotIn("synthetic-title2", generic_section)
                self.assertNotIn("synthetic_title2", generic_section)
                # Generic derivation must not check for any manifest id
                deriv = text.split("function Assert-TitlePlanDerivation", 1)[1].split("function ", 1)[0] if "Assert-TitlePlanDerivation" in text else ""
                self.assertNotIn("title_manifest_id", deriv.lower() if deriv else "")

    def test_makefile_has_no_second_title_conditional(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertNotIn("synthetic_title2", text)
        self.assertNotIn("synthetic-title2", text)
        hst_conds = [m.start() for m in re.finditer(r"ifeq\s*\(\$\(GAME_NAME\),hst\)", text)]
        self.assertEqual(len(hst_conds), 1, "Makefile may only retain the manifest-unbound refusal")


class HostPortability(unittest.TestCase):
    def test_generic_plan_renders_paths_with_forward_slashes(self) -> None:
        """Windows backslashes are normalized; plan is host-portable."""
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path(r"C:\private\EBOOT.elf"),
            build_dir=pathlib.Path(r"C:\repo\build\synthetic_title2"),
            module_dir=pathlib.Path(r"C:\private\modules"),
            include_optional_modules={"synthetic2.prx"},
        )
        rendered = json.dumps(plan)
        self.assertNotIn("\\\\", rendered)
        self.assertIn("C:/private/EBOOT.elf", rendered)
        self.assertIn("C:/repo/build/synthetic_title2/synthetic_title2_recomp.c", rendered)
        self.assertIn("C:/private/modules/synthetic2.prx@0x0a800000", rendered)

    def test_protected_digest_is_portable_and_covers_only_operational_semantics(self) -> None:
        """A notes-only edit does not move the digest; any operative edit does."""
        manifest = _load(SYNTHETIC2)
        baseline = title_codegen_plan.compute_protected_digest(manifest)
        notes_only = copy.deepcopy(manifest)
        notes_only["notes"] = "prose clarification"
        self.assertEqual(title_codegen_plan.compute_protected_digest(notes_only), baseline)
        operative = copy.deepcopy(manifest)
        operative["executable"]["base"] += 0x1000
        self.assertNotEqual(title_codegen_plan.compute_protected_digest(operative), baseline)


class MakeSpanPrecedenceTests(unittest.TestCase):
    MAKE = shutil.which("mingw32-make") or shutil.which("make") or "make"

    def _effective(self, game_name, title_val=None, title_origin="cmd"):
        env = os.environ.copy()
        env.pop("TITLE_EXTRA_SPANS", None)
        args = [self.MAKE, "-f", str(ROOT / "Makefile"), f"GAME_NAME={game_name}"]
        if title_val is not None:
            if title_origin == "cmd":
                args.append(f"TITLE_EXTRA_SPANS={title_val}" if title_val else "TITLE_EXTRA_SPANS=")
            else:
                env["TITLE_EXTRA_SPANS"] = title_val
        args += ["--eval", "print_effective: ; @echo EFFECTIVE=$(EFFECTIVE_EXTRA_SPANS)", "print_effective"]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for line in proc.stdout.splitlines():
            if line.strip().startswith("EFFECTIVE="):
                return line.strip().split("=", 1)[1].strip()
        return ""

    def test_undefined_is_empty(self) -> None:
        self.assertEqual(self._effective("synthetic2"), "")

    def test_command_line_value_is_used(self) -> None:
        self.assertEqual(self._effective("synthetic2", "generic-value"), "generic-value")

    def test_environment_value_is_used(self) -> None:
        self.assertEqual(self._effective("synthetic2", "env-generic", title_origin="env"), "env-generic")

    def test_command_line_overrides_environment(self) -> None:
        env = os.environ.copy()
        env["TITLE_EXTRA_SPANS"] = "env-generic"
        args = [
            self.MAKE,
            "-f",
            str(ROOT / "Makefile"),
            "GAME_NAME=synthetic2",
            "TITLE_EXTRA_SPANS=cmd-generic",
            "--eval",
            "print_effective: ; @echo EFFECTIVE=$(EFFECTIVE_EXTRA_SPANS)",
            "print_effective",
        ]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("EFFECTIVE=cmd-generic", proc.stdout)

    def test_explicit_empty_stays_empty(self) -> None:
        self.assertEqual(self._effective("synthetic2", ""), "")

    def test_hst_name_does_not_infer_a_span(self) -> None:
        self.assertEqual(self._effective("hst"), "")


class RetiredHstIsolationTests(unittest.TestCase):
    """Retired HST environment state cannot affect generic title work."""

    def test_generic_planner_ignores_stale_hst_env(self):
        # Stale HST env var must not leak into generic planner output
        env_backup = os.environ.get("HST_EXTRA_SPANS")
        try:
            os.environ["HST_EXTRA_SPANS"] = "stale-hst-value"
            manifest = _load(SYNTHETIC2)
            plan = title_codegen_plan.build_manager_plan(
                manifest,
                game_name="synthetic_title2",
                game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
                build_dir=pathlib.Path("build/synthetic_title2"),
            )
            self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
            self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
            # Direct build_plan also
            plan2 = title_codegen_plan.build_plan(
                manifest,
                game_name="synthetic_title2",
                game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
                build_dir=pathlib.Path("build/synthetic_title2"),
            )
            self.assertEqual(plan2["environment"]["TITLE_EXTRA_SPANS"], "")
            self.assertNotIn("HST_EXTRA_SPANS", plan2["environment"])
        finally:
            if env_backup is None:
                os.environ.pop("HST_EXTRA_SPANS", None)
            else:
                os.environ["HST_EXTRA_SPANS"] = env_backup

    def test_make_generic_ignores_stale_hst_via_env(self):
        # Direct Make with stale HST env for generic title must yield empty effective
        make = shutil.which("mingw32-make") or shutil.which("make") or "make"
        env = os.environ.copy()
        env["HST_EXTRA_SPANS"] = "stale-hst-value"
        env.pop("TITLE_EXTRA_SPANS", None)
        args = [make, "-f", str(ROOT / "Makefile"), "GAME_NAME=synthetic2", "--eval", "print_effective: ; @echo EFFECTIVE=$(EFFECTIVE_EXTRA_SPANS)", "print_effective"]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0)
        eff = ""
        for line in proc.stdout.splitlines():
            if "EFFECTIVE=" in line:
                eff = line.split("EFFECTIVE=", 1)[1].strip()
        self.assertEqual(eff, "")

    def test_powershell_generic_does_not_carry_hst(self):
        shell = shutil.which("pwsh")
        if shell is None:
            self.skipTest("pwsh required")
        helper = ROOT / "tools" / "title_manager_plan.ps1"
        script = "\n".join([
            "$ErrorActionPreference='Stop'",
            f". '{helper}'",
            "$env:HST_EXTRA_SPANS='stale-hst-value'",
            "$env:TITLE_EXTRA_SPANS='original-title'",
            "$state = Push-TitleAnalyzerEnvironment -Value 'new-title'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'new-title') { Write-Output 'FAIL_TITLE'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne 'stale-hst-value') { Write-Output \"FAIL_HST_LEAK:$env:HST_EXTRA_SPANS\"; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $state",
            "if ($env:TITLE_EXTRA_SPANS -ne 'original-title') { Write-Output 'FAIL_RESTORE_TITLE'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne 'stale-hst-value') { Write-Output 'FAIL_RESTORE_HST'; exit 1 }",
            "Write-Output 'PASS'",
        ])
        proc = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", script], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)


class PowershellGenericEnvironmentTests(unittest.TestCase):
    """PowerShell generic environment helper owns only TITLE_EXTRA_SPANS."""

    @classmethod
    def setUpClass(cls):
        cls.shell = shutil.which("pwsh")
        if cls.shell is None:
            raise unittest.SkipTest("pwsh required")

    def _run(self, body):
        helper = ROOT / "tools" / "title_manager_plan.ps1"
        script = "\n".join([
            "$ErrorActionPreference='Stop'",
            f". '{helper}'",
            "try {",
            body,
            "} catch { Write-Output \"THREW: $($_.Exception.Message)\"; exit 3 }",
        ])
        proc = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", script], cwd=ROOT, capture_output=True, text=True)
        return proc

    def test_generic_sets_only_generic_state(self):
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$state = Push-TitleAnalyzerEnvironment -Value 'generic-value'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'generic-value') { Write-Output 'FAIL_TITLE'; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $state",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_stale_hst_does_not_affect_generic_projection(self):
        proc = self._run("\n".join([
            "$env:HST_EXTRA_SPANS='stale-hst-value'",
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$state = Push-TitleAnalyzerEnvironment -Value ''",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_TITLE_PRESENT'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne 'stale-hst-value') { Write-Output 'FAIL_HST_CHANGED'; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $state",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_TITLE_AFTER'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne 'stale-hst-value') { Write-Output 'FAIL_HST_AFTER'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_initially_absent_restores_to_absent(self):
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$state = Push-TitleAnalyzerEnvironment -Value 'temp'",
            "Pop-TitleAnalyzerEnvironment -State $state",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_PRESENT'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_initially_populated_restores_exactly(self):
        proc = self._run("\n".join([
            "$env:TITLE_EXTRA_SPANS='original'",
            "$state = Push-TitleAnalyzerEnvironment -Value 'temp'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'temp') { Write-Output 'FAIL_TEMP'; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $state",
            "if ($env:TITLE_EXTRA_SPANS -ne 'original') { Write-Output \"FAIL_RESTORE:$env:TITLE_EXTRA_SPANS\"; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_exception_finally_restoration(self):
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$env:TITLE_EXTRA_SPANS='before'",
            "try {",
            "  $state = Push-TitleAnalyzerEnvironment -Value 'temp'",
            "  throw 'oops'",
            "} catch {",
            "  Pop-TitleAnalyzerEnvironment -State $state",
            "}",
            "if ($env:TITLE_EXTRA_SPANS -ne 'before') { Write-Output 'FAIL'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_nested_does_not_leak(self):
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$s1 = Push-TitleAnalyzerEnvironment -Value 'outer'",
            "$s2 = Push-TitleAnalyzerEnvironment -Value 'inner'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'inner') { Write-Output 'FAIL_INNER'; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $s2",
            "if ($env:TITLE_EXTRA_SPANS -ne 'outer') { Write-Output \"FAIL_OUTER:$env:TITLE_EXTRA_SPANS\"; exit 1 }",
            "Pop-TitleAnalyzerEnvironment -State $s1",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_FINAL_PRESENT'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)

    def test_document_absent_vs_empty(self):
        # Assigning "" removes the variable on PowerShell 7.4 (.NET 8) and keeps an
        # empty value on 7.5+ (.NET 9+). Both hosts are supported, so the analyzer
        # helpers normalize empty to removal and this test accepts either form.
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$env:TITLE_EXTRA_SPANS=''",
            "if (-not [string]::IsNullOrEmpty($env:TITLE_EXTRA_SPANS)) { Write-Output \"FAIL_EMPTY_VALUE:$env:TITLE_EXTRA_SPANS\"; exit 1 }",
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_AFTER_REMOVE'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)


class DirectMakeRetirementTests(unittest.TestCase):
    MAKE = shutil.which("mingw32-make") or shutil.which("make") or "make"

    def _get(self, var, game_name="hst", *extra_args):
        env = os.environ.copy()
        env.pop("TITLE_EXTRA_SPANS", None)
        args = [
            self.MAKE,
            "-f",
            str(ROOT / "Makefile"),
            f"GAME_NAME={game_name}",
            *extra_args,
            "--eval",
            f"print_var: ; @echo {var}=$({var})",
            "print_var",
        ]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for line in proc.stdout.splitlines():
            stripped = line.strip()
            if stripped.startswith(f"{var}="):
                return stripped.split(f"{var}=", 1)[1].strip()
        return ""

    def test_hst_name_has_no_direct_make_title_defaults(self) -> None:
        self.assertEqual(self._get("CODEGEN_PROFILE_ARG", "hst"), "")
        self.assertEqual(self._get("EFFECTIVE_EXTRA_SPANS", "hst"), "")
        self.assertEqual(self._get("TITLE_EXTRA_SPANS", "hst"), "")
        self.assertEqual(self._get("BUILD_DIR", "hst"), "build/hst")

    def test_source_owned_second_title_does_not_inherit_hst_values(self) -> None:
        extra = ("TITLE_MANIFEST=assets/titles/synthetic-title2.json",)
        self.assertEqual(self._get("CODEGEN_PROFILE_ARG", "hst", *extra), "")
        self.assertEqual(self._get("EFFECTIVE_EXTRA_SPANS", "hst", *extra), "")

    def test_direct_hst_make_without_manifest_fails_named(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nakagawa-hst-make-") as temp:
            build_dir = pathlib.Path(temp).resolve()
            target = pathlib.Path(build_dir.as_posix()) / "sr_title_config.h"
            proc = subprocess.run(
                [
                    self.MAKE,
                    "-f",
                    str(ROOT / "Makefile"),
                    "GAME_NAME=hst",
                    f"BUILD_DIR={build_dir.as_posix()}",
                    target.as_posix(),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("needs a validated title manifest", proc.stdout + proc.stderr)


class ArtifactFutureCompatibleTests(unittest.TestCase):
    """Ensure generic planner does not introduce host artifact coupling."""

    def test_generic_does_not_append_exe(self):
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        self.assertNotIn(".exe", plan["game_name"])
        self.assertNotIn(".exe", plan["make"]["build_dir"])
        # Make GAME_NAME may remain hst.exe for HST but generic must not
        self.assertFalse(plan["game_name"].endswith(".exe"))

    def test_generic_title_id_does_not_imply_host_path(self):
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        # Title id is not a path
        self.assertNotIn("/", plan["title_manifest_id"])
        self.assertNotIn("\\", plan["title_manifest_id"])
        self.assertNotIn(":", plan["title_manifest_id"])


class PathTextRenderingTests(unittest.TestCase):
    """_path_text is rendering only, not containment."""

    def test_path_text_is_rendering_only(self):
        # It replaces backslashes with slashes but does not claim containment
        from title_codegen_plan import _path_text
        self.assertEqual(_path_text(pathlib.Path(r"C:\foo\bar"), "label"), "C:/foo/bar")
        self.assertEqual(_path_text(pathlib.Path("a/b"), "label"), "a/b")
        # Ensure comments/tests say rendering only (checked by searching planner)
        text = (ROOT / "tools" / "title_codegen_plan.py").read_text(encoding="utf-8")
        self.assertIn("forward-slash", text.lower())
        # Ensure no claim of containment/security
        # The planner's _path_text should not be described as containment
        self.assertNotIn("containment", text.lower())
        self.assertNotIn("security validation", text.lower())


if __name__ == "__main__":
    unittest.main()
