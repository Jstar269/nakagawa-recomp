# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import subprocess
import tempfile
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import history_audit


class TestHistoryAudit(unittest.TestCase):
    def test_repository_baseline(self):
        baseline = history_audit.get_repository_baseline(history_audit.ROOT)
        self.assertIn("git_commit_main", baseline)
        self.assertGreater(baseline["total_commits"], 0)
        self.assertGreater(baseline["total_objects"], 0)
        self.assertGreater(baseline["total_refs"], 0)

    def test_redaction_and_finding_classification(self):
        token = "ghp_" + "123456789012345678901234567890123456"
        token_literal = "ghp_" + "123456789012345678901234567890123456"
        f = history_audit.HistoryFinding(
            category="DEFINITE_SECRET",
            code="TEST_SECRET",
            commit="abc12345",
            path="config/keys.py",
            detail="Found key: 0123456789abcdef0123456789abcdef and token " + token,
        )
        d = f.to_dict(redact=True)
        self.assertEqual(d["category"], "DEFINITE_SECRET")
        self.assertNotIn("0123456789abcdef0123456789abcdef", d["detail"])
        self.assertNotIn(token_literal, d["detail"])
        self.assertIn("[REDACTED_HEX_KEY]", d["detail"])
        self.assertIn("[REDACTED_API_TOKEN]", d["detail"])

    def test_full_history_audit_report_generation(self):
        report = history_audit.generate_full_history_audit_report(history_audit.ROOT)
        self.assertIn("status", report)
        self.assertIn("baseline", report)
        self.assertIn("summary", report)
        self.assertIn("large_blobs", report)
        self.assertIn("findings", report)

    def test_ancestor_only_sensitive_blob_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for argv in (("init", "-q"), ("config", "user.email", "t@example.invalid"),
                         ("config", "user.name", "test")):
                subprocess.run(["git", *argv], cwd=root, check=True, capture_output=True)
            (root / "safe.txt").write_text("safe\n", encoding="utf-8")
            subprocess.run(["git", "add", "safe.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "safe"], cwd=root, check=True)
            sensitive_fixture = " ".join(("private", "save", "baseline", "capture")) + "\n"
            (root / "private.txt").write_text(sensitive_fixture, encoding="utf-8")
            subprocess.run(["git", "add", "private.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "temporary"], cwd=root, check=True)
            subprocess.run(["git", "rm", "-q", "private.txt"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "remove"], cwd=root, check=True)

            findings = history_audit.audit_history_blob_contents(root)
            self.assertTrue(
                any(f.code == "HISTORICAL_BLOB_PRIVATE_VOCABULARY" for f in findings),
                "sensitive content existing only in an ancestor must still fail",
            )


if __name__ == "__main__":
    unittest.main()
