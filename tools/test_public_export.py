# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import json
from pathlib import Path
import tempfile
import unittest

from tools.build_public_export import (
    check_unresolved_legal_blockers,
    export_sanitized_public_tree,
    is_public_safe_export_tree,
    run_all_publication_gates,
    run_history_audit,
    run_publish_audit,
    run_sbom_verification,
)

# Several checks below re-export from, or assert the on-disk presence of, the
# unreviewed #98/#99/#104 components. Those are absent by design once this tree
# *is* a public-safe export, so the checks describe the private source tree
# only. Skip rather than weaken them: in the private tree they still run in
# full, and in a public clone a stranger gets an explained skip instead of an
# unexplained failure.
_PUBLIC_SAFE_TREE = is_public_safe_export_tree()
_PRIVATE_TREE_ONLY = "private source tree only: the public-safe profile excludes #98/#99/#104 components"


class TestPublicExport(unittest.TestCase):
    def test_legal_blockers_fail_closed_without_profile(self):
        res = check_unresolved_legal_blockers(public_safe_profile=False)
        self.assertFalse(res.passed)
        self.assertIn("OPEN PUBLICATION BLOCKERS", res.detail)

    def test_legal_blockers_pass_with_public_safe_profile(self):
        res = check_unresolved_legal_blockers(public_safe_profile=True)
        self.assertTrue(res.passed)
        self.assertIn("PUBLIC-SAFE PROFILE ACTIVE", res.detail)

    @unittest.skipIf(_PUBLIC_SAFE_TREE, _PRIVATE_TREE_ONLY)
    def test_publish_audit_gate(self):
        res = run_publish_audit()
        self.assertTrue(res.passed, f"publish_audit gate failed: {res.detail}")

    def test_history_audit_gate(self):
        res = run_history_audit()
        self.assertTrue(res.passed, f"history_audit gate failed: {res.detail}")

    def test_sbom_verification_gate(self):
        res = run_sbom_verification()
        self.assertTrue(res.passed, f"sbom_verification gate failed: {res.detail}")

    @unittest.skipIf(_PUBLIC_SAFE_TREE, _PRIVATE_TREE_ONLY)
    def test_run_all_publication_gates(self):
        results = run_all_publication_gates(public_safe_profile=True)
        self.assertEqual(len(results), 4)
        for gate in results:
            self.assertTrue(gate.passed, f"Gate '{gate.name}' failed: {gate.detail}")

    def test_dry_run_export(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            target_path = Path(tmpdir) / "export_repo"
            success = export_sanitized_public_tree(target_path, public_safe_profile=True, dry_run=True)
            self.assertTrue(success)

    @unittest.skipIf(_PUBLIC_SAFE_TREE, _PRIVATE_TREE_ONLY)
    def test_public_safe_export_excludes_unreviewed_components(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            target_path = Path(tmpdir) / "public_export"
            success = export_sanitized_public_tree(target_path, public_safe_profile=True)
            self.assertTrue(success, "public-safe export should succeed at the tree level")
            # Core toolkit and required publication files are present.
            self.assertTrue((target_path / "src" / "rt" / "recomp.c").is_file())
            self.assertTrue((target_path / "LICENSE").is_file())
            self.assertTrue((target_path / "NOTICE.md").is_file())
            # Unreviewed #98/#99/#104 components are excluded from the tree.
            for rel in (
                "font/jpn0.pgf",
                "font/kr0.pgf",
                "font/ltn0.pgf",
                "font/ltn8.pgf",
                "src/rt/pgf.c",
                "src/rt/pgf.h",
                "src/rt/pgd.c",
                "src/rt/pgd.h",
                "tools/pgd_decrypt.py",
                "tools/pgd_e2e_harness.c",
                "tools/test_pgd_c.py",
                "tools/test_pgd_decrypt.py",
            ):
                self.assertFalse((target_path / rel).exists(), f"{rel} must be excluded")
            # Provenance metadata records the profile, source commit, and exclusions.
            metadata_path = target_path / "PUBLIC_EXPORT.json"
            self.assertTrue(metadata_path.is_file())
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            self.assertEqual(metadata["profile"], "public-safe-v1")
            self.assertGreater(metadata["excluded_file_count"], 0)
            self.assertIn("font/jpn0.pgf", metadata["excluded_paths"])
            self.assertIn("src/rt/pgd.c", metadata["excluded_paths"])
            self.assertTrue(metadata["source_commit"])

    def test_public_safe_export_audits_in_public_scope_in_its_own_pre_commit(self):
        # The exported tree lacks the manifest-declared excluded components by
        # design; the export's own publication-safety pre-commit hook must run
        # in public scope so contributor commits do not fail on their absence.
        with tempfile.TemporaryDirectory() as tmpdir:
            target_path = Path(tmpdir) / "public_export"
            success = export_sanitized_public_tree(target_path, public_safe_profile=True)
            self.assertTrue(success, "public-safe export should succeed at the tree level")
            pre_commit_text = (target_path / ".pre-commit-config.yaml").read_text(encoding="utf-8")
            self.assertIn(
                "entry: python tools/publish_audit.py --tracked-only --public-scope",
                pre_commit_text,
            )
            self.assertNotIn(
                "entry: python tools/publish_audit.py --tracked-only\n",
                pre_commit_text.replace("--public-scope", ""),
            )

    @unittest.skipIf(_PUBLIC_SAFE_TREE, _PRIVATE_TREE_ONLY)
    def test_standard_export_keeps_profile_excluded_files(self):
        # Without --public-safe-profile the export is a plain tree snapshot and
        # keeps the unresolved fonts/sources; main()'s candidate audit then
        # fails closed on them. This documents that exclusion requires the flag.
        with tempfile.TemporaryDirectory() as tmpdir:
            target_path = Path(tmpdir) / "standard_export"
            success = export_sanitized_public_tree(target_path, public_safe_profile=False)
            self.assertTrue(success)
            self.assertTrue((target_path / "font" / "jpn0.pgf").is_file())
            self.assertTrue((target_path / "src" / "rt" / "pgf.c").is_file())
            self.assertFalse((target_path / "PUBLIC_EXPORT.json").exists())


if __name__ == "__main__":
    unittest.main()
