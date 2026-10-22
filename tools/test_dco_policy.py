# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import re
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


def _section(content: str, number: int) -> str:
    """Return AGENTS.md section ``number`` (heading to the next ``## `` heading)."""
    start = re.search(rf"(?m)^## {number}\.\s", content)
    if start is None:
        raise AssertionError(f"AGENTS.md has no section {number}")
    end = re.search(r"(?m)^## ", content[start.end():])
    return content[start.start(): start.end() + end.start()] if end else content[start.start():]


def _blocks(section: str) -> list[str]:
    """Split a section into bullets and paragraphs, each with its wrapping collapsed.

    The checks below look for a rule's key terms inside one block, so rewording,
    reflowing or reordering a rule never fails them; dropping a term does.
    """
    blocks: list[str] = []
    for line in section.splitlines():
        stripped = line.strip()
        if not stripped:
            blocks.append("")
        elif stripped.startswith(("- ", "* ", "|", "```", "#")) or not blocks or not blocks[-1]:
            blocks.append(stripped)
        else:
            blocks[-1] += " " + stripped
    return [block for block in blocks if block]


class TestGovernanceSurfaces(unittest.TestCase):
    """Small fail-closed checks for the public agent/configuration contract.

    These pin the rules AGENTS.md must state, as the key terms each rule needs,
    not its sentences: a maintainer can reword or reflow the contract, and the
    test fails only when a rule loses a term that carries its meaning.
    """

    def assertRuleStated(self, blocks, terms, rule):
        matches = [block for block in blocks if all(term.lower() in block.lower() for term in terms)]
        self.assertTrue(matches, f"AGENTS.md no longer states {rule}: no bullet or paragraph has all of {terms}")

    def test_agents_contract_pins_required_content(self):
        content = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        self.assertRegex(content, r"(?m)^## Where to start\s*$")
        for section in range(1, 12):
            with self.subTest(section=section):
                self.assertRegex(content, rf"(?m)^## {section}\.\s")

        self.assertRuleStated(
            _blocks(_section(content, 4)),
            ("Assisted-by:", "never", "Co-Authored-By:", "docs/AI_USAGE.md"),
            "the agent commit trailer rule",
        )

        section_3 = _section(content, 3)
        section_3_blocks = _blocks(section_3)
        self.assertRuleStated(
            section_3_blocks,
            ("explicit maintainer authorization", "current turn"),
            "that the hard stops need explicit maintainer authorization in the current turn",
        )
        hard_stop_bullets = [block for block in section_3_blocks if block.startswith("- ")]
        hard_stops = {
            "attestation on a maintainer's behalf": ("provenance attestation", "legal clearance", "behalf"),
            "no invented sign-off or lineage": ("contributor identity", "Signed-off-by:", "DCO", "source lineage"),
            "no history rewriting": ("history rewriting", "force-push", "another agent's work"),
            "no settings changes": ("security", "settings", "exact setting"),
            "no device writes": ("firmware", "flash", "NAND", "idStorage", "damage"),
            "no private disclosure": ("disclosing private inputs", "keys", "derived bytes", "Section 5"),
        }
        for rule, terms in hard_stops.items():
            with self.subTest(hard_stop=rule):
                self.assertRuleStated(hard_stop_bullets, terms, f"the hard stop: {rule}")

        standing = [block for block in section_3_blocks if block.startswith("**Standing hardware authorization.**")]
        self.assertEqual(len(standing), 1, "section 3 needs one standing hardware authorization paragraph")
        for terms in (
            ("non-destructive", "per-turn authorization", "hardware-lock protocol", "one holder at a time"),
            ("revoke", "return to per-turn authorization"),
            ("never covers", "destructive writes", "tags", "releases", "credentials", "disclosure of private inputs"),
            ("requested from the maintainer", "never simulated"),
        ):
            with self.subTest(standing_authorization=terms):
                self.assertRuleStated(standing, terms, "the bounds of the standing hardware authorization")

        execution_contracts = (
            (118, ("fail-closed", "analyzer-owned", "fail closed", "not an all-Allegrex interpreter")),
            (126, ("`jr`/`jalr`", "latched at transfer", "before link writes", "delay slot")),
            (127, ("`target`", "`resume_pc`", "`dispatch_call`", "CALL and TAIL", "not a resume descriptor")),
            (128, ("cosimulation", "CALL/TAIL", "negative corpus", "build-only mutant", "not a valid semantic kill")),
        )
        section_7_blocks = _blocks(_section(content, 7))
        last_position = -1
        for pr, terms in execution_contracts:
            with self.subTest(contract=pr):
                bullets = [i for i, block in enumerate(section_7_blocks) if block.startswith(f"- **PR #{pr}:**")]
                self.assertEqual(len(bullets), 1, f"section 7 needs one PR #{pr} contract bullet")
                self.assertRuleStated([section_7_blocks[bullets[0]]], terms, f"the PR #{pr} execution contract")
                self.assertGreater(bullets[0], last_position, "the execution contracts stay in PR order")
                last_position = bullets[0]

        report = re.search(r"```text\n(.*?)```", _section(content, 11), re.S)
        self.assertIsNotNone(report, "section 11 needs its report template block")
        fields = [line.split(":", 1)[0] for line in report.group(1).splitlines() if line.strip()]
        self.assertEqual(
            fields,
            [
                "LANE", "MISSION", "BASE_SHA", "HEAD_SHA", "PR", "STATUS", "RESULT", "ROOT_CAUSE",
                "CHANGED_FILES", "NEW_PATH_PROVENANCE", "FAILING_BEFORE", "TESTS", "PRIVATE_ACCEPTANCE",
                "HARDWARE", "UNRESOLVED", "DISCOVERED_FOLLOWUPS", "READY_QUALITY",
            ],
            "the section 11 report template must keep its fields",
        )

    def test_release_safety_rule_is_present_and_not_contradicted(self):
        policy_text = "\n".join(
            (ROOT / rel).read_text(encoding="utf-8")
            for rel in (
                "AGENTS.md",
                "CLAUDE.md",
                ".github/copilot-instructions.md",
                ".github/PULL_REQUEST_TEMPLATE.md",
            )
        )
        self.assertRuleStated(
            _blocks(_section((ROOT / "AGENTS.md").read_text(encoding="utf-8"), 3)),
            (
                "must not", "Git tags", "GitHub Releases", "release assets", "published version",
                "explicit maintainer authorization in the current turn", "do not authorize",
            ),
            "the tag and release rule",
        )
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
