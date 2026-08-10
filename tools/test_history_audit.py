# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
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
        f = history_audit.HistoryFinding(
            category="DEFINITE_SECRET",
            code="TEST_SECRET",
            commit="abc12345",
            path="config/keys.py",
            detail="Found key: 0123456789abcdef0123456789abcdef and token ghp_123456789012345678901234567890123456",
        )
        d = f.to_dict(redact=True)
        self.assertEqual(d["category"], "DEFINITE_SECRET")
        self.assertNotIn("0123456789abcdef0123456789abcdef", d["detail"])
        self.assertNotIn("ghp_123456789012345678901234567890123456", d["detail"])
        self.assertIn("[REDACTED_HEX_KEY]", d["detail"])
        self.assertIn("[REDACTED_API_TOKEN]", d["detail"])

    def test_full_history_audit_report_generation(self):
        report = history_audit.generate_full_history_audit_report(history_audit.ROOT)
        self.assertIn("status", report)
        self.assertIn("baseline", report)
        self.assertIn("summary", report)
        self.assertIn("large_blobs", report)
        self.assertIn("findings", report)


if __name__ == "__main__":
    unittest.main()
