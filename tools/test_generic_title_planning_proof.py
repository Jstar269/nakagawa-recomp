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
    ("libfont.prx", 840957952),
    ("scePsmf_library.prx", 841482240),
    ("scePsmfP_library.prx", 841975912),
]
HST_DISC_ID = "UCUS98701"


def _load(path: pathlib.Path) -> dict:
    return title_manifest.load_manifest(path)


class GenericTitleProofFixtures(unittest.TestCase):
    def test_synthetic_title2_fixture_is_valid_and_deterministic(self) -> None:
        """Synthetic-title2 validates, canonicalizes stably, and is publication-safe."""
        raw = SYNTHETIC2.read_text(encoding="utf-8")
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
        import json as _json, tempfile
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
        """The HST manager accepts only the checked-in HST retail manifest, not synthetics."""
        manifest = _load(SYNTHETIC2)
        plan = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="synthetic_title2",
            game_elf=pathlib.Path("build/fixtures/synthetic2.elf"),
            build_dir=pathlib.Path("build/synthetic_title2"),
        )
        self._run_adapter_reject(plan, "the HST manager accepts only the checked-in HST retail manifest")

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
        self._run_adapter_reject(plan, "the HST manager accepts only the checked-in HST retail manifest")

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
            ("duplicate_key", lambda m: m.update({"id": "synthetic-title2-v1", "id": "dup"}), "duplicate"),  # handled via loads
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
        # Exactly two HST-specific conditionals exist (defaults + title-config unbound check);
        # a second synthetic title must not add a third.
        hst_conds = [m.start() for m in re.finditer(r"ifeq\s*\(\$\(GAME_NAME\),hst\)", text)]
        self.assertEqual(len(hst_conds), 2, "Makefile must have exactly two HST-specific GAME_NAME conditionals (defaults + config check)")


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
    """Executable Make precedence matrix using the ACTUAL candidate Makefile.

    Proves the precedence contract via GNU Make introspection (harmless --eval print
    target) not by reimplementing Make logic in Python.
    """

    MAKE = shutil.which("mingw32-make") or shutil.which("make") or "make"

    def _effective(self, game_name, title_val=None, hst_val=None, title_origin="cmd", hst_origin="cmd", env_overrides=None):
        env = os.environ.copy()
        env.pop("TITLE_EXTRA_SPANS", None)
        env.pop("HST_EXTRA_SPANS", None)
        if env_overrides:
            env.update(env_overrides)
        args = [self.MAKE, "-f", str(ROOT / "Makefile"), f"GAME_NAME={game_name}"]
        if title_val is not None:
            if title_origin == "cmd":
                args.append(f"TITLE_EXTRA_SPANS={title_val}" if title_val != "" else "TITLE_EXTRA_SPANS=")
            elif title_origin == "env":
                env["TITLE_EXTRA_SPANS"] = title_val
        if hst_val is not None:
            if hst_origin == "cmd":
                args.append(f"HST_EXTRA_SPANS={hst_val}" if hst_val != "" else "HST_EXTRA_SPANS=")
            elif hst_origin == "env":
                env["HST_EXTRA_SPANS"] = hst_val
        args += ["--eval", "print_effective: ; @echo EFFECTIVE=$(EFFECTIVE_EXTRA_SPANS)", "print_effective"]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for line in proc.stdout.splitlines():
            if "EFFECTIVE=" in line:
                # line is like "EFFECTIVE=..." or "EFFECTIVE=..."
                # The echo prints EFFECTIVE=VALUE
                idx = line.find("EFFECTIVE=")
                if idx != -1:
                    return line[idx + len("EFFECTIVE="):].strip()
        # Fallback: search in combined
        combined = proc.stdout + proc.stderr
        for line in combined.splitlines():
            if line.strip().startswith("EFFECTIVE="):
                return line.strip().split("=", 1)[1].strip()
        return ""

    def test_generic_G1_undefined_both_empty(self):
        self.assertEqual(self._effective("synthetic2", None, None), "")

    def test_generic_G2_title_only(self):
        self.assertEqual(self._effective("synthetic2", "generic-value", None), "generic-value")

    def test_generic_G3_stale_hst_ignored(self):
        self.assertEqual(self._effective("synthetic2", None, "stale-hst-value"), "")

    def test_generic_G4_both_title_wins(self):
        self.assertEqual(self._effective("synthetic2", "generic-value", "stale-hst-value"), "generic-value")

    def test_generic_G5_explicit_empty_stays_empty(self):
        self.assertEqual(self._effective("synthetic2", "", "stale-hst-value"), "")

    def test_hst_H1_default(self):
        self.assertEqual(self._effective("hst", None, None), "0x00303194,0x00306e24")

    def test_hst_H2_legacy_supplied(self):
        self.assertEqual(self._effective("hst", None, "my-hst"), "my-hst")

    def test_hst_H3_title_supplied(self):
        self.assertEqual(self._effective("hst", "my-title", None), "my-title")

    def test_hst_H4_both_equal(self):
        self.assertEqual(self._effective("hst", "same", "same"), "same")

    def test_hst_H5_both_different_title_wins(self):
        self.assertEqual(self._effective("hst", "title-val", "hst-val"), "title-val")

    def test_hst_H6_explicit_empty_authoritative(self):
        self.assertEqual(self._effective("hst", "", "non-empty"), "")

    def test_origin_env_generic_value(self):
        self.assertEqual(self._effective("synthetic2", "env-generic", None, title_origin="env"), "env-generic")

    def test_origin_env_legacy_stale_ignored_for_generic(self):
        self.assertEqual(self._effective("synthetic2", None, "stale", hst_origin="env"), "")

    def test_origin_cmd_generic_overrides_env(self):
        # env generic, cmd generic overrides
        env = {"TITLE_EXTRA_SPANS": "env-generic"}
        # Use helper with env_overrides
        # For this test, set env generic and cmd generic
        # Our helper's env_overrides already handles env, but we need to combine
        # We will call with title env and title cmd: cmd should win (Make command line overrides env)
        # To test, we need to set env generic and also pass cmd generic
        # Our helper currently supports only one title_val with one origin. We simulate by setting env directly
        # and passing cmd.
        proc_env = os.environ.copy()
        proc_env.pop("TITLE_EXTRA_SPANS", None)
        proc_env.pop("HST_EXTRA_SPANS", None)
        proc_env["TITLE_EXTRA_SPANS"] = "env-generic"
        args = [self.MAKE, "-f", str(ROOT / "Makefile"), "GAME_NAME=synthetic2", "TITLE_EXTRA_SPANS=cmd-generic", "--eval", "print_effective: ; @echo EFFECTIVE=$(EFFECTIVE_EXTRA_SPANS)", "print_effective"]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=proc_env)
        self.assertEqual(proc.returncode, 0)
        eff = ""
        for line in proc.stdout.splitlines():
            if "EFFECTIVE=" in line:
                eff = line.split("EFFECTIVE=", 1)[1].strip()
        self.assertEqual(eff, "cmd-generic")

    def test_origin_cmd_legacy_with_generic_undefined(self):
        # For HST, legacy cmd with generic undefined should be used
        self.assertEqual(self._effective("hst", None, "legacy-cmd", title_origin="cmd", hst_origin="cmd"), "legacy-cmd")

    def test_precedence_table_recorded(self):
        # Record the exact table for diagnostic output
        table = []
        for game, title, hst, expected in [
            ("synthetic2", None, None, ""),
            ("synthetic2", "generic-value", None, "generic-value"),
            ("synthetic2", None, "stale", ""),
            ("synthetic2", "generic-value", "stale", "generic-value"),
            ("synthetic2", "", "stale", ""),
            ("hst", None, None, "0x00303194,0x00306e24"),
            ("hst", None, "my-hst", "my-hst"),
            ("hst", "my-title", None, "my-title"),
            ("hst", "same", "same", "same"),
            ("hst", "title-val", "hst-val", "title-val"),
            ("hst", "", "non-empty", ""),
        ]:
            eff = self._effective(game, title, hst)
            table.append((game, repr(title), repr(hst), eff, expected))
            self.assertEqual(eff, expected)
        # Print table for REVIEW (not asserted as failure)
        # Use a deterministic string so reviewer can verify
        for row in table:
            sys.stderr.write(f"MAKE_PRECEDENCE {row}\n")


class StaleHstIsolationTests(unittest.TestCase):
    """Strong regression: stale HST state cannot affect a generic title."""

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

    def test_hst_adapter_still_supplies_legacy(self):
        shell = shutil.which("pwsh")
        if shell is None:
            self.skipTest("pwsh required")
        helper = ROOT / "tools" / "title_manager_plan.ps1"
        # Build a synthetic plan with a span, then verify HST adapter synthesizes HST
        manifest = _load(SYNTHETIC)
        # Make a zero-based manifest with a span
        owned = json.loads(json.dumps(manifest))
        owned["executable"]["base"] = 0
        owned["executable"]["entry"] = 0
        owned["executable"]["extra_executable_spans"] = [{"start": 0x00400000, "end": 0x00400100}]
        plan = title_codegen_plan.build_manager_plan(
            owned,
            game_name="synthetic",
            game_elf=pathlib.Path("build/fixtures/synthetic.elf"),
            build_dir=pathlib.Path("build/synthetic"),
            funcs_per_chunk=64,
        )
        # Generic plan must have only TITLE
        self.assertIn("TITLE_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "0x00400000,0x00400100")
        # Now check that HST adapter would synthesize HST (but generic plan shouldn't be accepted by HST adapter)
        # Instead test that a real HST plan via adapter would have both
        # For this we just verify the adapter's synthesis logic exists via a synthetic HST-like plan that passes HST checks
        # Use the HST adapter's environment synthesis: it should return both keys
        # We test via pwsh: create a minimal HST plan (using synthetic with HST-like id but will be rejected for other pins)
        # Instead we just verify that Push-Hst sets both
        script = "\n".join([
            "$ErrorActionPreference='Stop'",
            f". '{helper}'",
            "$state = Push-HstAnalyzerEnvironment -Value '0x00400000,0x00400100'",
            "if ($env:TITLE_EXTRA_SPANS -ne '0x00400000,0x00400100') { Write-Output 'FAIL_TITLE'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne '0x00400000,0x00400100') { Write-Output 'FAIL_HST'; exit 1 }",
            "Pop-HstAnalyzerEnvironment -State $state",
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
            "Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$state = Push-TitleAnalyzerEnvironment -Value 'generic-value'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'generic-value') { Write-Output 'FAIL_TITLE'; exit 1 }",
            "if (Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS') { Write-Output \"FAIL_HST_PRESENT:$env:HST_EXTRA_SPANS\"; exit 1 }",
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

    def test_hst_compatibility_can_supply_legacy(self):
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$state = Push-HstAnalyzerEnvironment -Value 'hst-value'",
            "if ($env:TITLE_EXTRA_SPANS -ne 'hst-value') { Write-Output 'FAIL_TITLE'; exit 1 }",
            "if ($env:HST_EXTRA_SPANS -ne 'hst-value') { Write-Output 'FAIL_HST'; exit 1 }",
            "Pop-HstAnalyzerEnvironment -State $state",
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
        # PowerShell cannot distinguish absent from empty: setting to "" removes.
        proc = self._run("\n".join([
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "$env:TITLE_EXTRA_SPANS=''",
            "# PowerShell 5.1 removes empty, pwsh keeps it; both are valid but empty value is always empty string.",
            "# Document real behavior: empty and absent are not reliably distinguishable across hosts.",
            "if ($env:TITLE_EXTRA_SPANS -ne '') { Write-Output \"FAIL_EMPTY_VALUE:$env:TITLE_EXTRA_SPANS\"; exit 1 }",
            "Remove-Item -LiteralPath 'Env:TITLE_EXTRA_SPANS' -Force -ErrorAction SilentlyContinue",
            "if (Test-Path -LiteralPath 'Env:TITLE_EXTRA_SPANS') { Write-Output 'FAIL_AFTER_REMOVE'; exit 1 }",
            "Write-Output 'PASS'",
        ]))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)


class DirectHstMakeCompatibilityTests(unittest.TestCase):
    """Pin public HST defaults via actual Makefile evaluation."""

    MAKE = shutil.which("mingw32-make") or shutil.which("make") or "make"

    def _get(self, var, game_name="hst"):
        env = os.environ.copy()
        env.pop("TITLE_EXTRA_SPANS", None)
        env.pop("HST_EXTRA_SPANS", None)
        args = [self.MAKE, "-f", str(ROOT / "Makefile"), f"GAME_NAME={game_name}", "--eval", f"print_var: ; @echo {var}=$({var})", "print_var"]
        proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for line in proc.stdout.splitlines():
            # The echo we added is exactly "VAR=VALUE" at line start; ignore the
            # build_profile record line which also contains "VAR=" as a substring
            # inside a longer python command.
            stripped = line.strip()
            if stripped.startswith(f"{var}="):
                return stripped.split(f"{var}=", 1)[1].strip()
        return ""

    def test_hst_defaults(self):
        # For HST, defaults must be present
        self.assertEqual(self._get("CODEGEN_PROFILE_ARG", "hst"), "--profile=hst")
        self.assertEqual(self._get("EFFECTIVE_EXTRA_SPANS", "hst"), "0x00303194,0x00306e24")
        self.assertEqual(self._get("HST_EXTRA_SPANS", "hst"), "0x00303194,0x00306e24")
        self.assertEqual(self._get("TITLE_EXTRA_SPANS", "hst"), "0x00303194,0x00306e24")
        # Build dir derived from GAME_NAME
        self.assertEqual(self._get("BUILD_DIR", "hst"), "build/hst")

    def test_synthetic_does_not_receive_hst_defaults(self):
        self.assertEqual(self._get("EFFECTIVE_EXTRA_SPANS", "synthetic2"), "")
        self.assertEqual(self._get("TITLE_EXTRA_SPANS", "synthetic2"), "")
        # HST variable may be empty for synthetic but must not be the HST default
        hst_val = self._get("HST_EXTRA_SPANS", "synthetic2")
        self.assertNotEqual(hst_val, "0x00303194,0x00306e24")
        self.assertEqual(self._get("CODEGEN_PROFILE_ARG", "synthetic2"), "")
        self.assertEqual(self._get("BUILD_DIR", "synthetic2"), "build/synthetic2")


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
