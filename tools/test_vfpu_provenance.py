# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import unittest.mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_vfpu_provenance as provenance


class TestVfpuProvenance(unittest.TestCase):
    def test_checked_in_assets_match_manifest(self):
        manifest = provenance.load_manifest()
        self.assertEqual(provenance.verify_local(manifest), [])

    def test_git_blob_id_matches_definition(self):
        data = b"vfpu provenance test"
        expected = hashlib.sha1(
            f"blob {len(data)}\0".encode() + data,
            usedforsecurity=False,
        ).hexdigest()
        self.assertEqual(provenance.git_blob_id(data), expected)

    def test_modified_asset_is_rejected(self):
        manifest = provenance.load_manifest()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for entry in manifest["files"]:
                source = provenance.ASSET_DIR / entry["path"]
                (root / entry["path"]).write_bytes(source.read_bytes())
            first = root / manifest["files"][0]["path"]
            first.write_bytes(first.read_bytes() + b"\0")
            errors = provenance.verify_local(manifest, root)
        self.assertTrue(any("mismatch" in error for error in errors))

    def test_upstream_repo_parsed_from_manifest_url(self):
        manifest = provenance.load_manifest()
        self.assertEqual(provenance._upstream_repo(manifest), "hrydgard/ppsspp")
        with self.assertRaises(ValueError):
            provenance._upstream_repo({"source_repository": "notaurl"})

    def test_upstream_api_reports_blob_drift(self):
        # A manifest blob that disagrees with upstream must surface as a mismatch,
        # proving the API check is load-bearing rather than vacuously passing. The
        # network call is stubbed so this stays hermetic.
        manifest = provenance.load_manifest()
        fake_tree = json.dumps({
            "tree": [
                {"path": Path(e["upstream_path"]).name, "type": "blob", "sha": e["git_blob"]}
                for e in manifest["files"]
            ]
        })
        # Flip one recorded blob so it no longer matches the (correct) upstream tree.
        drifted = copy.deepcopy(manifest)
        drifted["files"][0]["git_blob"] = "0" * 40

        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout=fake_tree, stderr="")
        with unittest.mock.patch.object(subprocess, "run", return_value=completed):
            errors = provenance.verify_upstream_api(drifted)
        self.assertTrue(any("mismatch" in error for error in errors))

    def test_upstream_api_skips_without_gh(self):
        # Missing `gh` must yield a single explicit "skipped" note, never a silent
        # pass -- a provenance check that quietly no-ops is worse than none.
        manifest = provenance.load_manifest()

        def raise_missing(*args, **kwargs):
            raise FileNotFoundError("gh")

        with unittest.mock.patch.object(subprocess, "run", side_effect=raise_missing):
            errors = provenance.verify_upstream_api(manifest)
        self.assertEqual(len(errors), 1)
        self.assertIn("skipped", errors[0])


if __name__ == "__main__":
    unittest.main()
