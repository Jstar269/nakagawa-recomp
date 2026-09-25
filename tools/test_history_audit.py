# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import tempfile
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import history_audit
from nk_core.git_isolation import run_git


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

    def _blob_findings(self, content: str) -> list:
        """Commit ``content`` into a throwaway repository and scan it."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for argv in (("init", "-q"),):
                run_git(argv, cwd=root, check=True, capture_output=True)
            (root / "note.md").write_text(content, encoding="utf-8")
            run_git(["add", "note.md"], cwd=root, check=True)
            run_git(["commit", "-qm", "fixture"], cwd=root, check=True)
            return history_audit.audit_history_blob_contents(root)

    def test_quoted_armour_delimiter_without_a_body_is_not_a_secret(self):
        """Prose naming the PEM format is not a key.

        docs/PROVENANCE_MERGE_GATE.md told a maintainer which value to paste into
        a CI secret store and quoted both delimiters on one line to identify the
        format. That failed this gate as a DEFINITE_SECRET while containing no
        key material at all, and a gate that is red over prose is a gate people
        learn to skip past.
        """
        begin = "-----BE" + "GIN RSA PRIVATE KEY-----"
        end = "-----E" + "ND RSA PRIVATE KEY-----"
        prose = (
            "* `PROVENANCE_APP_PRIVATE_KEY` -- the full PEM text "
            "(`" + begin + "` ... `" + end + "`), newlines preserved.\n"
        )
        secrets = [f for f in self._blob_findings(prose) if f.category == "DEFINITE_SECRET"]
        self.assertEqual(
            secrets, [],
            "an armour delimiter with no key body between it and the footer is "
            "documentation, not a secret",
        )

    def test_armour_delimiter_with_a_body_is_still_a_secret(self):
        """The narrowing must not cost a single real key."""
        bodies = {
            "RSA": "MIIEowIBAAKCAQEAx7Vq9kZ3mQ8Jf2pL0sYtWn4cB6dHgR1uEvXaTzKmNpQrSuVw",
            "OPENSSH": "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAABlwAAAAdzc2gtcn",
            "ENCRYPTED": "MIIFDjBABgkqhkiG9w0BBQ0wMzAbBgkqhkiG9w0BBQwwDgQI5yNCu9T5SnsCAggA",
        }
        for label, body in bodies.items():
            with self.subTest(key=label):
                begin = "-----BE" + "GIN " + label + " PRIVATE KEY-----"
                end = "-----E" + "ND " + label + " PRIVATE KEY-----"
                pem = begin + "\n" + body + "\n" + end + "\n"
                self.assertTrue(
                    any(f.code == "HISTORICAL_BLOB_SECRET" for f in self._blob_findings(pem)),
                    "a " + label + " key body must still be caught",
                )

    def test_ancestor_only_sensitive_blob_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for argv in (("init", "-q"),):
                run_git(argv, cwd=root, check=True, capture_output=True)
            (root / "safe.txt").write_text("safe\n", encoding="utf-8")
            run_git(["add", "safe.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "safe"], cwd=root, check=True)
            sensitive_fixture = " ".join(("private", "save", "baseline", "capture")) + "\n"
            (root / "private.txt").write_text(sensitive_fixture, encoding="utf-8")
            run_git(["add", "private.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "temporary"], cwd=root, check=True)
            run_git(["rm", "-q", "private.txt"], cwd=root, check=True)
            run_git(["commit", "-qm", "remove"], cwd=root, check=True)

            findings = history_audit.audit_history_blob_contents(root)
            self.assertTrue(
                any(f.code == "HISTORICAL_BLOB_PRIVATE_VOCABULARY" for f in findings),
                "sensitive content existing only in an ancestor must still fail",
            )


if __name__ == "__main__":
    unittest.main()
