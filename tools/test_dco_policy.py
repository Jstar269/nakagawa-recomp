# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent


class TestDCOPolicy(unittest.TestCase):
    def test_dco_policy_document_exists(self):
        doc_file = ROOT / "docs" / "DCO_POLICY.md"
        self.assertTrue(doc_file.is_file(), "docs/DCO_POLICY.md file missing")

        content = doc_file.read_text(encoding="utf-8")
        self.assertIn("Developer Certificate of Origin", content)
        self.assertIn("Version 1.1", content)
        self.assertIn("Signed-off-by:", content)
        self.assertIn("git commit -s", content)
        self.assertIn("Automated Bots", content)
        self.assertIn("Correction Workflow", content)

    def test_contributing_dco_reference(self):
        contrib_file = ROOT / "CONTRIBUTING.md"
        self.assertTrue(contrib_file.is_file(), "CONTRIBUTING.md file missing")

        content = contrib_file.read_text(encoding="utf-8")
        self.assertIn("docs/DCO_POLICY.md", content)
        self.assertIn("Signed-off-by:", content)

    def test_pr_template_dco_checkbox(self):
        pr_tmpl = ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md"
        self.assertTrue(pr_tmpl.is_file(), "PULL_REQUEST_TEMPLATE.md file missing")

        content = pr_tmpl.read_text(encoding="utf-8")
        self.assertIn("Developer Certificate of Origin (DCO 1.1)", content)
        self.assertIn("docs/DCO_POLICY.md", content)

    def test_maintainer_waiver_is_standing_and_survives_public_launch(self):
        # The waiver's whole purpose is that it does NOT lapse at public launch.
        # An edit that re-scopes it to "prior to public launch" would silently
        # reintroduce a merge blocker on a single-maintainer public repository.
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertIn("Maintainer Standing Waiver", content)
        self.assertRegex(
            content,
            r"until the maintainer explicitly revokes it",
            "waiver must be revocation-scoped, not time- or launch-scoped",
        )
        self.assertRegex(
            content,
            r"does \*\*not\*\* expire on public launch",
            "waiver must state explicitly that public launch does not end it",
        )
        self.assertNotRegex(
            content,
            r"Commits created prior to public launch operate under",
            "the superseded launch-scoped waiver wording must not return",
        )

    def test_waived_missing_signoff_is_not_a_merge_blocker(self):
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertRegex(content, r"is \*\*not\*\* a merge blocker")
        # No retroactive rewrite may be required to satisfy a checker.
        self.assertIn("history is not rewritten to satisfy a sign-off checker", content)

    def test_dco_not_required_status_check_while_waiver_active(self):
        content = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertIn(
            "DCO must not be configured as a required status check on the public repository",
            content,
        )
        self.assertIn("advisory (non-blocking) mode", content)
        # The superseded promise to activate an enforcing check at launch must
        # not reappear; promotion is gated on explicit revocation instead.
        self.assertNotIn(
            "An automated DCO check workflow will be activated on the public repository upon public launch",
            content,
        )
        self.assertIn("only after the standing waiver is explicitly revoked", content)

    def test_agents_may_never_fabricate_a_signoff(self):
        # The waiver must not read as agent authority to sign on someone's
        # behalf. This rule is stated in three places; all must hold.
        policy = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertIn("grants an agent **no** authority to add a sign-off", policy)
        for rel in ("AGENTS.md", "docs/AI_USAGE.md"):
            text = (ROOT / rel).read_text(encoding="utf-8")
            self.assertRegex(
                text,
                r"must n(ever|ot) invent",
                f"{rel} must keep the no-fabricated-sign-off rule",
            )

    def test_outside_contributor_requirements_are_not_weakened(self):
        # The waiver is personal to the maintainer. Outside contributors keep
        # DCO 1.1 plus third-party and AI disclosure.
        policy = (ROOT / "docs" / "DCO_POLICY.md").read_text(encoding="utf-8")
        self.assertIn("Not extended to outside contributors", policy)
        self.assertIn("Third-Party Source & Asset Disclosure", policy)
        self.assertIn("AI-Assisted Work Disclosure", policy)

        contrib = (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8")
        self.assertIn("Outside contributions to Nakagawa Recomp require", contrib)

        pr_tmpl = (ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
        # Disclosure checkboxes survive the waiver.
        self.assertIn("new third-party material is identified below", pr_tmpl)
        self.assertIn("material AI assistance is disclosed below", pr_tmpl)

    def test_dco_signoff_regex(self):
        pattern = re.compile(r"^Signed-off-by:\s+([^<]+)\s+<([^>]+)>$", re.MULTILINE)
        msg_valid = "feat(tooling): add feature\n\nSigned-off-by: Jane Doe <jane@example.com>"
        msg_invalid = "feat(tooling): add feature without signoff"

        self.assertTrue(pattern.search(msg_valid))
        self.assertFalse(pattern.search(msg_invalid))


if __name__ == "__main__":
    unittest.main()
