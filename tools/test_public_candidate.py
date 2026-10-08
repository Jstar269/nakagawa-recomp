# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import sys
import json
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))

import public_candidate
import publish_audit


class TestPublicCandidate(unittest.TestCase):
    def setUp(self):
        self.profile = public_candidate.load_profile(public_candidate.DEFAULT_PROFILE)

    def test_profile_excludes_disputed_implementations_and_all_fonts(self):
        for path in (
            "src/rt/pgf.c",
            "src/rt/pgf.h",
            "src/rt/pgd.c",
            "src/rt/pgd.h",
            "tools/pgd_decrypt.py",
            "font/jpn0.pgf",
            "font/future.pgf",
        ):
            self.assertTrue(public_candidate.is_excluded(path, self.profile), path)
        self.assertFalse(public_candidate.is_excluded("src/rt/pgf_unavailable.c", self.profile))
        self.assertFalse(public_candidate.is_excluded("src/rt/pgd_unavailable.c", self.profile))
        self.assertFalse(public_candidate.is_excluded("font/README.md", self.profile))

    def test_filesystem_candidate_audit_uses_candidate_contents(self):
        with tempfile.TemporaryDirectory() as temp_raw:
            root = Path(temp_raw)
            for required in publish_audit.REQUIRED_PATHS:
                # Final newline and explicit LF: this fixture is asserted to
                # produce a clean audit, and text hygiene is part of clean.
                (root / required).write_text(
                    required + "\n", encoding="utf-8", newline="\n")
            manifest = root / "assets" / "release_manifest.json"
            manifest.parent.mkdir()
            manifest.write_text('{"components": []}\n', encoding="utf-8", newline="\n")
            # The gate fails closed without a canonical policy and rejects any path
            # the policy does not classify, so this fixture declares its own.
            import publication_policy

            document = {
                "name": "hermetic-test-profile",
                "profile_version": "2.0.0",
                "min_tool_version": "0.4.0",
                "build_mode": "PUBLIC_SAFE=1",
                "default_disposition": "REJECT",
                "exclude_prefixes": [],
                "exclude_globs": [],
                "exclude_paths": [],
                "include_paths": sorted(
                    [*publish_audit.REQUIRED_PATHS, "assets/release_manifest.json",
                     "_policy.json", "PUBLIC_EXPORT.json"]
                ),
            }
            policy_path = root / "_policy.json"
            policy_path.write_text(
                json.dumps(document) + "\n", encoding="utf-8", newline="\n")
            export_path = root / "PUBLIC_EXPORT.json"
            export_path.write_text(
                json.dumps({"profile": document["name"],
                            "policy_sha256": publication_policy.canonical_digest(document)}) + "\n",
                encoding="utf-8",
                newline="\n",
            )

            entries = publish_audit._get_filesystem_entries(root)
            findings = publish_audit.audit_entries(
                entries, manifest_path=manifest, public_scope=True, repo_root=root,
                policy_path=policy_path, export_path=export_path,
            )
            self.assertEqual(findings, [])

    def test_manifest_lands_beside_the_tree_with_lf_endings(self):
        """Workspace hygiene (#368): the export manifest must not live inside the audited tree.

        Before the fix, ``materialize()`` wrote ``PUBLIC_CANDIDATE.json`` into the
        destination, so every fresh export failed its own audit
        (POLICY_UNCLASSIFIED / UNRESOLVED_PUBLIC on that path, plus CRLF from the
        host-default newline). The manifest now sits beside the destination, and
        its bytes are pinned to LF.
        """
        def fake_git(*args):
            if args[0] == "rev-parse":
                return "0" * 40
            output = next(
                arg.split("=", 1)[1] for arg in args if arg.startswith("--output=")
            )
            with zipfile.ZipFile(output, "w") as archive:
                archive.writestr("docs/example.md", "# example\n")
                archive.writestr("nested/marker.txt", "marker\n")
            return ""

        with tempfile.TemporaryDirectory() as temp_raw:
            destination = Path(temp_raw) / "candidate"
            with mock.patch.object(public_candidate, "_git", fake_git):
                metadata = public_candidate.materialize(
                    "HEAD", destination, public_candidate.DEFAULT_PROFILE
                )

            manifest = destination.parent / (
                destination.name + ".PUBLIC_CANDIDATE.json"
            )
            self.assertEqual(sorted(p.name for p in destination.rglob("*")), [
                "docs", "example.md", "marker.txt", "nested",
            ])
            self.assertFalse((destination / "PUBLIC_CANDIDATE.json").exists())
            self.assertTrue(manifest.is_file())
            raw = manifest.read_bytes()
            self.assertNotIn(b"\r", raw)
            payload = json.loads(raw)
            self.assertEqual(payload["source_commit"], "0" * 40)
            self.assertEqual(payload["file_count"], 2)
            self.assertEqual(payload["profile"], self.profile["name"])
            self.assertEqual(metadata, payload)


if __name__ == "__main__":
    unittest.main()
