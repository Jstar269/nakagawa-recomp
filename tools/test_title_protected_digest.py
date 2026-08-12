# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the protected title-manifest digest.

The digest binds the *operational* semantics of a validated manifest so a manager
can refuse to build when the title contract it planned against has changed. It must
therefore depend on meaning alone: reordering keys, reindenting, or switching line
endings must not move it, an explicitly non-operative prose field must not move it,
and every operative mutation must.

Only source-owned public manifests are used here; the private HST manifest is never
required.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan  # noqa: E402
import title_manifest  # noqa: E402

TITLES = ROOT / "assets" / "titles"
PLANNER = ROOT / "tools" / "title_codegen_plan.py"
PUBLIC_MANIFESTS = ("synthetic.json", "pspdev-phase5.json")


def digest(manifest: dict) -> str:
    return title_codegen_plan.compute_protected_digest(manifest)


class ProtectedDigestCanonicalizationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.raw = (TITLES / "synthetic.json").read_text(encoding="utf-8")
        self.manifest = json.loads(self.raw)
        self.baseline = digest(self.manifest)

    def test_digest_is_a_lowercase_sha256(self) -> None:
        self.assertRegex(self.baseline, r"^[0-9a-f]{64}$")

    def test_digest_is_stable_across_repeated_computation(self) -> None:
        self.assertEqual(digest(json.loads(self.raw)), self.baseline)

    def test_key_order_does_not_change_the_digest(self) -> None:
        reordered = dict(reversed(list(self.manifest.items())))
        reordered["executable"] = dict(reversed(list(self.manifest["executable"].items())))
        self.assertNotEqual(list(reordered), list(self.manifest))
        self.assertEqual(digest(reordered), self.baseline)

    def test_whitespace_and_line_endings_do_not_change_the_digest(self) -> None:
        for label, text in (
            ("compact", json.dumps(self.manifest, separators=(",", ":"))),
            ("indented", json.dumps(self.manifest, indent=8)),
            ("crlf", json.dumps(self.manifest, indent=2).replace("\n", "\r\n")),
            ("trailing-newlines", json.dumps(self.manifest, indent=2) + "\n\n\n"),
        ):
            with self.subTest(form=label):
                self.assertEqual(digest(json.loads(text)), self.baseline)

    def test_notes_are_excluded_because_they_are_non_operative(self) -> None:
        notes_only = copy.deepcopy(self.manifest)
        notes_only["notes"] = "an unrelated prose edit that changes no behavior"
        self.assertEqual(digest(notes_only), self.baseline)
        dropped = copy.deepcopy(self.manifest)
        dropped.pop("notes", None)
        self.assertEqual(digest(dropped), self.baseline)
        self.assertEqual(title_codegen_plan.NON_OPERATIVE_FIELDS, frozenset({"notes"}))

    def test_every_operative_mutation_changes_the_digest(self) -> None:
        mutations = (
            ("id", lambda value: value.update(id="synthetic-allegrex-v2")),
            ("display_name", lambda value: value.update(display_name="Renamed Fixture")),
            ("kind", lambda value: value.update(kind="homebrew")),
            ("base", lambda value: value["executable"].update(base=0x08804000)),
            ("entry", lambda value: value["executable"].update(entry=0x08804000)),
            ("bss-source", lambda value: value["executable"].update(bss_metadata_source="none")),
            ("span", lambda value: value["executable"].update(
                extra_executable_spans=[{"start": 16, "end": 32}]
            )),
            ("module-name", lambda value: value["modules"][0].update(name="renamed.prx")),
            ("module-address", lambda value: value["modules"][0].update(load_address=0x09000000)),
            ("module-role", lambda value: value["modules"][0].update(role="guest-prx", required=True)),
            ("module-removed", lambda value: value.update(modules=[])),
            ("data-root", lambda value: value["filesystem"].update(data_root="fixtures/other")),
            ("device-prefix", lambda value: value["filesystem"]["device_prefixes"].append("flash0:")),
            ("hle-profile", lambda value: value.update(hle_profile="synthetic-extended")),
            ("codegen-profile", lambda value: value.update(codegen_profile="hst")),
            ("feature", lambda value: value["feature_requirements"].append("psp-hle")),
            ("verification-profile", lambda value: value.update(verification_profile="strict-public")),
        )
        seen = {self.baseline}
        for label, mutate in mutations:
            with self.subTest(mutation=label):
                mutated = copy.deepcopy(self.manifest)
                mutate(mutated)
                try:
                    moved = digest(mutated)
                except title_manifest.TitleManifestError:
                    # Rejected outright, which is at least as strict as a digest change.
                    continue
                self.assertNotEqual(moved, self.baseline)
                seen.add(moved)
        self.assertGreater(len(seen), 1)

    def test_unknown_operative_fields_are_rejected_rather_than_ignored(self) -> None:
        # A field the validator does not know cannot be silently dropped from the
        # digest; validation fails closed so it can never travel unprotected.
        for path, mutate in (
            ("$", lambda value: value.update(unexpected_setting=True)),
            ("$.executable", lambda value: value["executable"].update(rebase_policy="flat")),
            ("$.modules[0]", lambda value: value["modules"][0].update(priority=3)),
        ):
            with self.subTest(path=path):
                mutated = copy.deepcopy(self.manifest)
                mutate(mutated)
                with self.assertRaisesRegex(title_manifest.TitleManifestError, "unknown field"):
                    digest(mutated)

    def test_duplicate_keys_in_the_source_text_fail_closed(self) -> None:
        doubled = self.raw.replace('"schema_version": 1,', '"schema_version": 1,\n  "schema_version": 1,', 1)
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate"):
            digest(title_manifest.loads_manifest(doubled))


class ProtectedDigestOwnershipTests(unittest.TestCase):
    """One implementation owner: plan, CLI, and library must never disagree."""

    def test_checked_in_public_manifests_have_distinct_digests(self) -> None:
        digests = {
            name: digest(title_manifest.load_manifest(TITLES / name))
            for name in PUBLIC_MANIFESTS
        }
        self.assertEqual(len(set(digests.values())), len(digests), digests)

    def test_manager_plan_digest_matches_the_library_digest(self) -> None:
        for name in PUBLIC_MANIFESTS:
            with self.subTest(manifest=name):
                manifest = title_manifest.load_manifest(TITLES / name)
                plan = title_codegen_plan.build_manager_plan(
                    manifest,
                    game_name="fixture",
                    game_elf=Path("build/fixtures/fixture.elf"),
                    build_dir=Path("build/fixture"),
                    funcs_per_chunk=64,
                )
                self.assertEqual(plan["protected_digest"], digest(manifest))

    def test_manager_plan_digest_ignores_operational_inputs(self) -> None:
        # The digest protects the *title contract*, not the invocation: changing the
        # build directory or chunk size must not move it, or every operator override
        # would look like a contract breach.
        manifest = title_manifest.load_manifest(TITLES / "synthetic.json")
        first = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="fixture",
            game_elf=Path("build/fixtures/fixture.elf"),
            build_dir=Path("build/fixture"),
            funcs_per_chunk=64,
        )
        second = title_codegen_plan.build_manager_plan(
            manifest,
            game_name="other",
            game_elf=Path("elsewhere/other.elf"),
            build_dir=Path("build/other"),
            funcs_per_chunk=2000,
        )
        self.assertEqual(first["protected_digest"], second["protected_digest"])

    def test_cli_digest_is_deterministic_and_matches_the_library(self) -> None:
        for name in PUBLIC_MANIFESTS:
            with self.subTest(manifest=name):
                command = [
                    sys.executable, str(PLANNER),
                    str(TITLES / name), "--print-protected-digest",
                ]
                first = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
                second = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
                self.assertEqual(first.returncode, 0, first.stderr)
                self.assertEqual(first.stdout, second.stdout)
                self.assertEqual(
                    first.stdout.strip(),
                    digest(title_manifest.load_manifest(TITLES / name)),
                )

    def test_cli_digest_mode_needs_no_private_bindings(self) -> None:
        # Printing a digest must not require --game-elf/--build-dir; the plan-emitting
        # modes still must.
        proc = subprocess.run(
            [sys.executable, str(PLANNER), str(TITLES / "synthetic.json")],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("--game-name", proc.stderr)

    def test_cli_rejects_an_invalid_manifest_before_printing_anything(self) -> None:
        broken = ROOT / "build" / "test_protected_digest_invalid.json"
        broken.parent.mkdir(parents=True, exist_ok=True)
        broken.write_text('{"schema_version": 2}', encoding="utf-8")
        self.addCleanup(broken.unlink, True)
        proc = subprocess.run(
            [sys.executable, str(PLANNER), str(broken), "--print-protected-digest"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertIn("ERROR:", proc.stderr)


if __name__ == "__main__":
    unittest.main()
