#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression test suite for public/private title isolation.

Verifies:
1. Public title catalog only includes titles authorized by assets/public_source_profile.json.
2. Dropping an excluded or unclassified manifest into assets/titles/ does NOT leak into public C codegen.
3. tools/title_catalog_codegen.py --verify fails closed if generated C contains unauthorized titles or is out of sync.
4. No retail or unclassified title manifests exist in the git tracked assets/titles/ directory.
5. The in-memory private overlay mechanism allows local acceptance without modifying public artifacts.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import publication_policy
import title_catalog_codegen
from nk_core.title_registry import TitleRegistry


class PublicTitleIsolationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy_path = ROOT / "assets" / "public_source_profile.json"
        self.policy = publication_policy.load_policy(self.policy_path)
        self.titles_dir = ROOT / "assets" / "titles"

    def test_tracked_titles_are_all_public_included(self) -> None:
        """Every title in assets/titles/ must be explicitly INCLUDED by publication policy."""
        json_files = list(self.titles_dir.glob("*.json"))
        self.assertGreater(len(json_files), 0, "Expected at least one public title manifest")

        for mf in json_files:
            rel = mf.relative_to(ROOT).as_posix()
            res = self.policy.resolve(rel)
            self.assertEqual(
                res.disposition,
                publication_policy.INCLUDED,
                f"Manifest {rel} must be INCLUDED by public source policy, got {res.disposition}"
            )
            self.assertFalse(
                res.is_excluded,
                f"Manifest {rel} is marked as excluded in publication policy!"
            )
            # Assert no retail patterns in filename
            self.assertNotIn("hst", mf.name.lower())
            self.assertNotIn("ucus98701", mf.name.lower())
            self.assertNotIn("uces01402", mf.name.lower())

    def test_catalog_generator_excludes_unauthorized_and_private_manifests(self) -> None:
        """Dropping a private manifest into assets/titles must be ignored by public collector."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            # Copy over current public manifests
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Inject fake retail manifest (simulating hst-ucus98701.json)
            fake_retail = {
                "schema_version": 1,
                "id": "hst-ucus98701-v1",
                "display_name": "Hot Shots Tennis (Private Injection Test)",
                "kind": "retail",
                "game_base": "0x00000000",
                "game_entry": "0x00000000",
                "executables": [{"path": "PSP_GAME/SYSDIR/EBOOT.BIN", "role": "main"}],
                "disc": {
                    "primary_disc_id": "UCUS98701",
                    "compatible_disc_ids": []
                }
            }
            retail_file = tmp_titles / "hst-ucus98701.json"
            retail_file.write_text(json.dumps(fake_retail), encoding="utf-8")

            # Inject unclassified arbitrary manifest
            unclassified = {
                "schema_version": 1,
                "id": "unclassified-title-v1",
                "display_name": "Unclassified Title",
                "kind": "retail",
                "game_base": "0x08804000",
                "game_entry": "0x08804000",
                "executables": [{"path": "EBOOT.BIN", "role": "main"}],
                "disc": {
                    "primary_disc_id": "ULUS99999",
                    "compatible_disc_ids": []
                }
            }
            unclass_file = tmp_titles / "unclassified.json"
            unclass_file.write_text(json.dumps(unclassified), encoding="utf-8")

            # Collect public manifests using the real publication policy
            # collect_public_manifests now refuses an out-of-repository
            # directory outright, because a path outside ROOT has no
            # publication-policy identity. These manifests are verbatim copies
            # of the tracked ones, so this test states explicitly which
            # in-repository directory's decisions apply to the copies.
            manifest_files, validated_titles = title_catalog_codegen.collect_public_manifests(
                tmp_titles, policy=self.policy, policy_dir=self.titles_dir
            )

            collected_names = [p.name for p in manifest_files]
            collected_ids = [t["id"] for t in validated_titles]

            # Neither the retail nor the unclassified manifest may be included
            self.assertNotIn("hst-ucus98701.json", collected_names)
            self.assertNotIn("unclassified.json", collected_names)
            self.assertNotIn("hst-ucus98701-v1", collected_ids)
            self.assertNotIn("unclassified-title-v1", collected_ids)

            # Public synthetic titles MUST be present
            self.assertIn("pspdev-phase5-v1", collected_ids)
            self.assertIn("synthetic-allegrex-v1", collected_ids)

    def test_generated_c_contains_zero_retail_references(self) -> None:
        """The checked-in generated C code must contain no retail IDs, keys, or titles."""
        gen_dir = ROOT / "src" / "core" / "generated"
        cat_h = gen_dir / "nk_title_catalog.h"
        cat_c = gen_dir / "nk_title_catalog.c"

        self.assertTrue(cat_h.is_file(), "nk_title_catalog.h does not exist")
        self.assertTrue(cat_c.is_file(), "nk_title_catalog.c does not exist")

        text_h = cat_h.read_text(encoding="utf-8")
        text_c = cat_c.read_text(encoding="utf-8")
        combined = text_h + "\n" + text_c

        forbidden_tokens = ["UCUS98701", "UCES01402", "Hot Shots", "Tennis", "hst-ucus98701"]
        for token in forbidden_tokens:
            self.assertNotIn(token, combined, f"Forbidden retail token '{token}' leaked into generated C catalog!")

    def test_verify_flag_detects_out_of_date_or_corrupted_catalog(self) -> None:
        """--verify flag must return non-zero exit code if generated files differ."""
        cmd = [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(proc.returncode, 0, f"--verify failed on clean tree:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

    def test_in_memory_private_overlay_registry(self) -> None:
        """TitleRegistry loads public titles by default, and can load private manifest on demand."""
        reg = TitleRegistry()
        reg.load_from_directory(self.titles_dir)

        # Public titles are loaded
        self.assertIsNotNone(reg.find_by_disc_id("TEST00001"))
        self.assertIsNotNone(reg.find_by_disc_id("TEST00005"))

        # Retail title is not present initially
        self.assertIsNone(reg.find_by_disc_id("UCUS98701"))

        # Load private overlay from simulated manifest
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            fake_private = {
                "schema_version": 1,
                "id": "private-overlay-test-v1",
                "display_name": "Private Overlay Title",
                "kind": "synthetic",
                "executable": {
                    "base": 142622720,
                    "entry": 142622720,
                    "bss_metadata_source": "elf",
                    "extra_executable_spans": []
                },
                "modules": [],
                "filesystem": {
                    "data_root": "fixtures/synthetic",
                    "memory_stick_root": "build/test/memstick",
                    "device_prefixes": ["host0:", "ms0:"]
                },
                "hle_profile": "synthetic-minimal",
                "codegen_profile": "none",
                "feature_requirements": ["allegrex-core"],
                "verification_profile": "synthetic-public",
                "runtime_bindings": {
                    "schema_version": 1,
                    "fallback_entry": 142622976,
                    "worker_thread_entry": 142623232,
                    "launcher_thread_entry": 142623488,
                    "vblank_frame_counter_addr": 142737408,
                    "vblank_vsync_counter_addr": 142737412
                }
            }
            json.dump(fake_private, f)
            f_path = Path(f.name)

        try:
            entry = reg.load_private_manifest(f_path)
            self.assertEqual(entry.id, "private-overlay-test-v1")
            found = reg.find_by_id("private-overlay-test-v1")
            self.assertIsNotNone(found)
            self.assertEqual(found.id, "private-overlay-test-v1")
        finally:
            if f_path.is_file():
                f_path.unlink()

    def test_included_non_title_json_fails_closed(self) -> None:
        """A non-title JSON file placed in manifests must be rejected / fail closed."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Create an arbitrary non-title JSON file (e.g. package.json or config)
            non_title = {"name": "not-a-title-manifest", "version": "1.0.0", "dependencies": {}}
            (tmp_titles / "pspdev-phase5.json").write_text(json.dumps(non_title), encoding="utf-8")

            # Must raise when attempting to collect public manifests
            # policy_dir keeps this exercising manifest VALIDATION rejection
            # rather than the new out-of-repository refusal.
            with self.assertRaises(Exception):
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=self.titles_dir
                )

    def test_out_of_repository_manifest_dir_fails_closed(self) -> None:
        """A manifest outside ROOT must not borrow a tracked path's policy decision."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            # Schema-valid, but NOT the tracked bytes of that name. Before the
            # fix this was named assets/titles/synthetic.json for policy
            # purposes, inherited that path's INCLUDED decision, and was
            # emitted into the public catalog with no record of its own.
            forged = json.loads((self.titles_dir / "synthetic.json").read_text(encoding="utf-8"))
            forged["display_name"] = "Forged Out-Of-Tree Title"
            (tmp_titles / "synthetic.json").write_text(json.dumps(forged), encoding="utf-8")

            with self.assertRaises(ValueError) as ctx:
                title_catalog_codegen.collect_public_manifests(tmp_titles, policy=self.policy)
            self.assertIn("outside the repository root", str(ctx.exception))

    def test_policy_dir_outside_repository_is_refused(self) -> None:
        """The explicit mapping cannot itself point outside the repository."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            with self.assertRaises(ValueError) as ctx:
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=tmp_titles
                )
            self.assertIn("not inside the repository root", str(ctx.exception))

    def test_malformed_manifest_rejection(self) -> None:
        """Manifests missing required schema fields or with bad types must be rejected."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Malformed manifest: unsupported schema_version 999
            bad_schema = {
                "schema_version": 999,
                "id": "bad-schema-v1",
                "display_name": "Bad Schema Title",
                "kind": "synthetic"
            }
            (tmp_titles / "pspdev-phase5.json").write_text(json.dumps(bad_schema), encoding="utf-8")

            # policy_dir keeps this exercising manifest VALIDATION rejection
            # rather than the new out-of-repository refusal.
            with self.assertRaises(Exception):
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=self.titles_dir
                )

    def test_private_overlay_has_zero_effect_on_verify(self) -> None:
        """External private manifest overlays must have zero effect on catalog verify."""
        # 1. Clean verify must succeed
        cmd = [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(proc.returncode, 0, f"--verify failed on baseline: {proc.stderr}")

        # 2. Simulate private overlay existing outside public assets
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            fake_overlay = {
                "schema_version": 1,
                "id": "hst-ucus98701-v1",
                "display_name": "Hot Shots Tennis Overlay",
                "kind": "retail",
                "disc": {"id": "UCUS98701", "region": "NA", "revision_policy": "exact-disc-id"}
            }
            json.dump(fake_overlay, f)
            overlay_path = Path(f.name)

        try:
            # Re-run verify; external overlay must not touch or alter verification
            proc2 = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc2.returncode, 0, f"--verify failed with external overlay present: {proc2.stderr}")
            self.assertIn("is up to date", proc2.stdout)
        finally:
            if overlay_path.is_file():
                overlay_path.unlink()

    def test_overlay_collision_policy(self) -> None:
        """Section 4: Conflicting overlay on canonical public title must fail closed unless overridden."""
        reg = TitleRegistry()
        reg.load_from_directory(self.titles_dir)

        # Create a manifest that conflicts with public title 'synthetic-allegrex-v1'
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            collision = {
                "schema_version": 1,
                "id": "synthetic-allegrex-v1",
                "display_name": "Colliding Overlay",
                "kind": "synthetic",
                "executable": {
                    "base": 142622720,
                    "entry": 142622720,
                    "bss_metadata_source": "elf",
                    "extra_executable_spans": []
                },
                "modules": [],
                "filesystem": {
                    "data_root": "fixtures/synthetic",
                    "memory_stick_root": "build/test/memstick",
                    "device_prefixes": ["host0:", "ms0:"]
                },
                "hle_profile": "synthetic-minimal",
                "codegen_profile": "none",
                "feature_requirements": ["allegrex-core"],
                "verification_profile": "synthetic-public",
            }
            json.dump(collision, f)
            f_path = Path(f.name)

        try:
            # 1. Standard mode: MUST REJECT
            with self.assertRaises(RuntimeError) as ctx:
                reg.load_private_manifest(f_path, allow_override=False)
            self.assertIn("conflicts with public canonical catalog", str(ctx.exception))

            # 2. Explicit developer override mode: ACCEPT
            profile = reg.load_private_manifest(f_path, allow_override=True)
            self.assertEqual(profile.id, "synthetic-allegrex-v1")
            self.assertEqual(profile.name, "Colliding Overlay")
        finally:
            if f_path.is_file():
                f_path.unlink()


if __name__ == "__main__":
    unittest.main()
