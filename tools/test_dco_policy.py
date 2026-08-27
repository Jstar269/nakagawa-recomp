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


class TestGovernanceSurfaces(unittest.TestCase):
    """Small fail-closed checks for the public agent/configuration contract."""

    def test_agents_contract_stays_concise(self):
        lines = (ROOT / "AGENTS.md").read_text(encoding="utf-8").splitlines()
        self.assertGreaterEqual(len(lines), 180)
        self.assertLessEqual(len(lines), 250)

    def test_release_safety_rule_is_present_and_not_contradicted(self):
        required = (
            "Agents must not create, move, push, or delete Git tags; create, edit, delete, "
            "publish, or unpublish GitHub Releases; upload release assets; or change a published "
            "version without explicit maintainer authorization in the current turn. Generic "
            "instructions such as 'finish', 'ship', 'publish', 'integrate', or 'do everything' "
            "do not authorize a version/tag/release operation."
        )
        policy_text = "\n".join(
            (ROOT / rel).read_text(encoding="utf-8")
            for rel in (
                "AGENTS.md",
                "CLAUDE.md",
                ".github/copilot-instructions.md",
                ".github/PULL_REQUEST_TEMPLATE.md",
            )
        )
        self.assertIn(required, policy_text)
        self.assertNotRegex(
            policy_text,
            r"(?im)^[ \t-]*agents?\s+(?:may|can)\s+(?:create|edit|publish|delete).*\b(?:tag|release)",
        )

    def test_documented_gate_paths_exist(self):
        for rel in (
            "tools/codegen.py",
            "tools/policy_sync.py",
            "tools/publish_audit.py",
            "tools/modified_file_notice_audit.py",
            "tools/lint_docs.py",
            "tools/ci_paths.py",
            "tools/test_ci_paths.py",
            "tools/test_codegen_transfer_target_timing.py",
            "tools/test_codegen_continuations.py",
            "tools/test_dispatch_c.py",
            "tools/test_dispatch_call_boundary.py",
            "tools/test_cosim_fixture.py",
            "tools/test_sched_invariants.py",
            "src/rt/guest_interp.c",
            "src/rt/recomp.c",
            "src/rt/sched.c",
        ):
            with self.subTest(path=rel):
                self.assertTrue((ROOT / rel).is_file(), f"documented gate path missing: {rel}")

    def test_single_pull_request_template_is_canonical(self):
        template = ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md"
        template_dir = ROOT / ".github" / "PULL_REQUEST_TEMPLATE"
        self.assertTrue(template.is_file())
        self.assertFalse((template_dir / "default.md").exists())
        self.assertEqual(list(template_dir.glob("*.md")), [])

    def test_tracked_claude_guidance_is_public_and_small(self):
        content = (ROOT / "CLAUDE.md").read_text(encoding="utf-8")
        self.assertLessEqual(len(content.splitlines()), 50)
        windows_path = r"[A-Z]" + re.escape(":") + r"[\\/]"
        unix_user_path = "/" + "Users" + "/"
        self.assertNotRegex(
            content,
            rf"(?i)(place_game_here|game\.iso|EBOOT|SaveBase|memstick/|{unix_user_path}|{windows_path}|session[-_ ]?[0-9a-f]{{8,}})",
        )
        self.assertNotRegex(content, r"(?i)\b(?:issue|pr)\s*#\d+")

    def test_claude_is_no_longer_ignored(self):
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertNotIn("/CLAUDE.md", ignore.splitlines())


if __name__ == "__main__":
    unittest.main()
