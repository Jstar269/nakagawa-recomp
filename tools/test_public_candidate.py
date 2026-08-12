# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import sys
import json
import tempfile
import unittest

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
                (root / required).write_text(required, encoding="utf-8")
            manifest = root / "assets" / "release_manifest.json"
            manifest.parent.mkdir()
            manifest.write_text('{"components": []}\n', encoding="utf-8")
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
            policy_path.write_text(json.dumps(document), encoding="utf-8")
            export_path = root / "PUBLIC_EXPORT.json"
            export_path.write_text(
                json.dumps({"profile": document["name"],
                            "policy_sha256": publication_policy.canonical_digest(document)}),
                encoding="utf-8",
            )

            entries = publish_audit._get_filesystem_entries(root)
            findings = publish_audit.audit_entries(
                entries, manifest_path=manifest, public_scope=True, repo_root=root,
                policy_path=policy_path, export_path=export_path,
            )
            self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
