#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for ``tools/admit_paths.py``, the one-command admission of new public paths.

The synthetic cases build a real public repository and a real stand-in for the
private authority repository, then run the whole admission, including the
real ``admit-new-reviewed``.  ``RecompJsonRegressionTest`` replays the
2026-10-22 ``.recomp.json`` admission and requires byte-identical records; it
needs the private authority checkout (``NK_PRIVATE_AUTHORITY``) and the public
commits of that admission, and is skipped (NOT_RUN) without them.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import admit_paths  # noqa: E402
from admit_paths import AdmitError  # noqa: E402

#: Commits run by the tool need an identity, and a host's own Git configuration
#: must not change what the tests observe.
GIT_ENV = {
    "GIT_AUTHOR_NAME": "Nakagawa Recomp Tests",
    "GIT_AUTHOR_EMAIL": "tests@nakagawa-recomp.invalid",
    "GIT_COMMITTER_NAME": "Nakagawa Recomp Tests",
    "GIT_COMMITTER_EMAIL": "tests@nakagawa-recomp.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
}


def git(cwd: Path, *args: str) -> str:
    env = {**os.environ, **GIT_ENV}
    return subprocess.run(
        ["git", *args], cwd=cwd, env=env, check=True, capture_output=True, text=True,
    ).stdout.strip()


def write(root: Path, relative: str, content: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


class _Fixture:
    """A synthetic public repository, its pull request branch, and an authority repository."""

    POLICY = {
        "name": "public-safe-v1",
        "profile_version": "2.0.0",
        "min_tool_version": "0.4.0",
        "build_mode": "PUBLIC_SAFE=1",
        "default_disposition": "REJECT",
        "exclude_prefixes": [],
        "exclude_globs": [],
        "exclude_paths": ["docs/private-notes.md"],
        "include_paths": sorted([
            "README.md", "docs/guide.md", "src/rt/existing.c", "assets/release_manifest.json",
            "assets/public_source_profile.json", "assets/public_provenance_ledger.json",
            "PUBLIC_EXPORT.json",
        ]),
    }

    def __init__(self, testcase: unittest.TestCase, records: list[dict] | None = None) -> None:
        self.tmp = Path(testcase.enterContext(tempfile.TemporaryDirectory()))
        testcase.enterContext(mock.patch.dict(os.environ, GIT_ENV))

        self.pr = self.tmp / "public"
        self.pr.mkdir()
        git(self.pr, "init", "-q", "-b", "main")
        write(self.pr, "README.md", "# Synthetic admission fixture\n")
        write(self.pr, "docs/guide.md", "# Guide\n\nSynthetic fixture.\n")
        write(self.pr, "src/rt/existing.c", "// SPDX-License-Identifier: GPL-3.0-or-later\nint existing(void);\n")
        write(self.pr, "assets/release_manifest.json", '{"name": "synthetic", "components": []}\n')
        write(self.pr, "assets/public_source_profile.json", json.dumps(self.POLICY, indent=2) + "\n")
        write(self.pr, "assets/public_provenance_ledger.json", "{}\n")
        write(self.pr, "PUBLIC_EXPORT.json", "{}\n")
        git(self.pr, "add", "-A")
        git(self.pr, "commit", "-qm", "synthetic base")
        self.base = git(self.pr, "rev-parse", "HEAD")
        git(self.pr, "update-ref", "refs/remotes/origin/main", self.base)
        git(self.pr, "checkout", "-q", "-b", "docs/new-pages-20261022")

        self.remote = self.tmp / "authority.git"
        git(self.tmp, "init", "-q", "--bare", "-b", "main", str(self.remote))
        self.authority = self.tmp / "authority"
        self.authority.mkdir()
        git(self.authority, "init", "-q", "-b", "main")
        ledger = {"records": [
            {"id": "PROV-EXISTING", "classification": "project-authored-independent",
             "evidence_tier": "H", "paths": ["src/rt/existing.c"]},
            *(records or []),
        ]}
        write(self.authority, admit_paths.TRUSTED_LEDGER, json.dumps(ledger, indent=2) + "\n")
        write(self.authority, "src/unrelated.c", "int unrelated;\n")
        git(self.authority, "add", "-A")
        git(self.authority, "commit", "-qm", "synthetic authority")
        git(self.authority, "remote", "add", "origin", str(self.remote))
        git(self.authority, "push", "-q", "origin", "main")

    def add(self, relative: str, content: str) -> str:
        write(self.pr, relative, content)
        git(self.pr, "add", relative)
        git(self.pr, "commit", "-qm", f"add {relative}")
        return hashlib.sha256(content.encode("utf-8")).hexdigest()

    def admit(self, *paths: str, **kwargs) -> dict:
        kwargs.setdefault("approval_basis", "Owner request: synthetic fixture pages.")
        kwargs.setdefault("date", "2026-10-22")
        return admit_paths.admit(
            pr_worktree=self.pr, paths=list(paths), private_repo=self.authority,
            scratch_root=self.tmp, **kwargs,
        )

    def record(self, relative: str) -> dict:
        return json.loads((self.authority / relative).read_text(encoding="utf-8"))

    def remote_main(self) -> str:
        return git(self.remote, "rev-parse", "main")


class AdmitPathsTest(unittest.TestCase):
    def test_documentation_path_end_to_end(self) -> None:
        fixture = _Fixture(self)
        digest = fixture.add("docs/new-page.md", "# New page\n")
        write(fixture.authority, "src/unrelated.c", "int unrelated = 1; /* another session's edit */\n")
        remote_before = fixture.remote_main()

        result = fixture.admit("docs/new-page.md", assisted_by="Test Tool (test-model)")

        self.assertEqual(result["base"], fixture.base, "default base is merge-base(HEAD, origin/main)")
        admission_path = "docs/provenance/admissions/admission-new-pages-2026-10-22.json"
        self.assertEqual(result["records"][0], admission_path)
        admission = fixture.record(admission_path)
        self.assertEqual(admission, {
            "approval_basis": "Owner request: synthetic fixture pages.",
            "kind": "admission-authority",
            "reviewed_new_paths": [
                {"classification": "reviewed_documentation", "path": "docs/new-page.md", "sha256": digest},
            ],
            "schema_version": 1,
        })
        raw = (fixture.authority / admission_path).read_bytes()
        self.assertEqual(raw, admit_paths._canonical(admission), "records are sorted-key, LF-terminated JSON")

        base_policy = git(fixture.pr, "show", f"{fixture.base}:assets/public_source_profile.json") + "\n"
        delta_dir = Path(result["records"][1]).parent
        candidate_policy = (fixture.authority / delta_dir / "candidate-policy.json").read_bytes()
        self.assertEqual(delta_dir.name, hashlib.sha256(candidate_policy).hexdigest())
        delta = fixture.record(f"{delta_dir.as_posix()}/policy-delta-authority.json")
        self.assertEqual(delta["baseline_policy_sha256"], hashlib.sha256(base_policy.encode()).hexdigest())
        self.assertEqual(delta["candidate_policy_sha256"], delta_dir.name)
        self.assertEqual(delta["allowed_delta"]["include_added"], ["docs/new-page.md"])
        self.assertEqual(delta["allowed_delta"]["rule_changes"], [])

        # The admission commit is the base plus the path, its profile entry and
        # regenerated controls -- nothing else from the pull request.
        branch = result["admit_branch"]
        self.assertEqual(branch, "admit/new-pages-2026-10-22")
        changed = git(fixture.pr, "diff", "--name-only", fixture.base, branch).splitlines()
        self.assertEqual(sorted(changed), sorted([
            "docs/new-page.md", "assets/public_source_profile.json",
            "assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json",
        ]))
        self.assertEqual(git(fixture.pr, "show", f"{branch}:assets/public_source_profile.json").encode() + b"\n",
                         candidate_policy)
        ledger = json.loads(git(fixture.pr, "show", f"{branch}:assets/public_provenance_ledger.json"))
        entry = next(entry for entry in ledger["entries"] if entry["path"] == "docs/new-page.md")
        self.assertEqual((entry["classification"], entry["sha256"]), ("reviewed_documentation", digest))
        self.assertEqual(ledger["admission"]["candidate_tree"], result["candidate_tree"])
        self.assertIn("Assisted-by: Test Tool (test-model)", git(fixture.pr, "log", "-1", "--format=%B", branch))
        self.assertNotIn("Signed-off-by", git(fixture.pr, "log", "-2", "--format=%B", branch))

        # Only the three records were committed in the authority checkout; the
        # unrelated edit stays uncommitted and nothing was pushed.
        self.assertEqual(sorted(git(fixture.authority, "show", "--name-only", "--format=", "HEAD").splitlines()),
                         sorted(result["records"]))
        self.assertEqual(git(fixture.authority, "status", "--porcelain"), "M src/unrelated.c")
        self.assertEqual(fixture.remote_main(), remote_before)
        self.assertFalse(result["pushed"])

        steps = admit_paths.remaining_steps(result)
        self.assertIn("push origin main", steps[0])
        self.assertIn(f"merge --no-edit {branch}", steps[1])
        self.assertIn(f"provenance_refresh.py --base {result['admission_commit']} --trusted-ledger", steps[2])
        self.assertNotIn(str(fixture.tmp / "nakagawa-admit-"), " ".join(steps))
        self.assertEqual([p for p in fixture.tmp.iterdir() if p.name.startswith("nakagawa-admit-")], [],
                         "the scratch candidate is removed")

    def test_admission_commit_merges_into_the_pull_request(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        fixture.add("docs/guide.md", "# Guide\n\nEdited in the same pull request.\n")
        result = fixture.admit("docs/new-page.md")
        git(fixture.pr, "merge", "-q", "--no-edit", result["admit_branch"])
        self.assertEqual(git(fixture.pr, "merge-base", "HEAD", result["admission_commit"]),
                         result["admission_commit"], "the refresh base is an ancestor of the merged head")

    def test_push_only_when_asked(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        result = fixture.admit("docs/new-page.md", push=True)
        self.assertTrue(result["pushed"])
        self.assertEqual(fixture.remote_main(), result["private_commit"])
        self.assertNotIn("push origin main", " ".join(admit_paths.remaining_steps(result)))

    def test_implementation_path_needs_an_existing_record(self) -> None:
        fixture = _Fixture(self)
        fixture.add("src/rt/new.c", "// SPDX-License-Identifier: GPL-3.0-or-later\nint fresh(void);\n")
        before = git(fixture.authority, "rev-parse", "HEAD")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("src/rt/new.c")
        self.assertEqual(caught.exception.code, "PROVENANCE_UNRESOLVED")
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), before)
        self.assertEqual(git(fixture.authority, "status", "--porcelain", "--", "docs"), "")
        self.assertEqual(git(fixture.pr, "branch", "--list", "admit/*"), "")

    def test_implementation_path_with_record_cites_it(self) -> None:
        record = {"id": "PROV-NEW", "classification": "project-authored-independent",
                  "evidence_tier": "H", "paths": ["src/rt/new.c"]}
        fixture = _Fixture(self, records=[record])
        fixture.add("src/rt/new.c", "// SPDX-License-Identifier: GPL-3.0-or-later\nint fresh(void);\n")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("src/rt/new.c")
        self.assertEqual(caught.exception.code, "ADMISSION_AUTHORITY_INCOMPLETE")

        origin = {"origin_kind": "authored_from_scratch", "origin": "Project-authored synthetic file.",
                  "license": "GPL-3.0-or-later"}
        result = fixture.admit("src/rt/new.c", origins={"src/rt/new.c": origin})
        statement = fixture.record(result["records"][0])["reviewed_new_paths"][0]
        self.assertEqual(statement["classification"], "project_authored_attested")
        self.assertEqual(statement["record_id"], "PROV-NEW")
        self.assertEqual({key: statement[key] for key in origin}, origin)

    def test_refusals_leave_nothing_behind(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        cases = {
            "ADMISSION_PATH_EXISTING": dict(paths=["docs/guide.md"]),
            "ADMISSION_PATH_EXCLUDED": dict(paths=["docs/private-notes.md"]),
            "ADMIT_PATH_MISSING": dict(paths=["docs/absent.md"]),
            "ADMISSION_PATH_FORBIDDEN": dict(paths=["PUBLIC_EXPORT.json"]),
            "ADMIT_PATH_NOT_EXACT": dict(paths=["docs/*.md"]),
            "ADMISSION_BASIS_REQUIRED": dict(paths=["docs/new-page.md"], approval_basis=" "),
        }
        write(fixture.pr, "docs/private-notes.md", "private\n")
        git(fixture.pr, "add", "docs/private-notes.md")
        git(fixture.pr, "commit", "-qm", "excluded file")
        for code, case in cases.items():
            with self.subTest(code=code):
                paths = case.pop("paths")
                with self.assertRaises(AdmitError) as caught:
                    fixture.admit(*paths, **case)
                self.assertEqual(caught.exception.code, code)
        self.assertEqual(git(fixture.authority, "status", "--porcelain", "--", "docs"), "")

        write(fixture.pr, "docs/new-page.md", "# Edited but not committed\n")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/new-page.md")
        self.assertEqual(caught.exception.code, "ADMIT_PATH_UNCOMMITTED")

    def test_base_must_be_an_ancestor(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        git(fixture.pr, "checkout", "-q", "main")
        fixture.add("docs/other.md", "# Elsewhere\n")
        unrelated = git(fixture.pr, "rev-parse", "HEAD")
        git(fixture.pr, "checkout", "-q", "docs/new-pages-20261022")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/new-page.md", base=unrelated)
        self.assertEqual(caught.exception.code, "TRUSTED_BASE_NOT_ANCESTOR")

    def test_rerun_with_identical_records_is_refused_only_by_the_branch(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        fixture.admit("docs/new-page.md")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/new-page.md")
        self.assertEqual(caught.exception.code, "ADMIT_BRANCH_EXISTS")
        git(fixture.pr, "branch", "-q", "-D", "admit/new-pages-2026-10-22")
        before = git(fixture.authority, "rev-parse", "HEAD")
        result = fixture.admit("docs/new-page.md")
        self.assertEqual(result["private_commit"], before, "identical records are reused, not recommitted")

        with self.assertRaises(AdmitError) as caught:
            git(fixture.pr, "branch", "-q", "-D", "admit/new-pages-2026-10-22")
            fixture.admit("docs/new-page.md", approval_basis="A different decision.")
        self.assertEqual(caught.exception.code, "ADMISSION_RECORD_CONFLICT")

    def test_rekey_after_another_admission_lands(self) -> None:
        """The serialisation case: pull request A lands first, B re-keys without a new decision."""
        fixture = _Fixture(self)
        fixture.add("docs/b.md", "# B\n")
        first = fixture.admit("docs/b.md")

        git(fixture.pr, "checkout", "-q", "-b", "docs/a-page-20261022", fixture.base)
        fixture.add("docs/a.md", "# A\n")
        landed = fixture.admit("docs/a.md", approval_basis="Owner request: page A.")
        git(fixture.pr, "checkout", "-q", "main")
        git(fixture.pr, "merge", "-q", "--ff-only", landed["admit_branch"])
        new_main = git(fixture.pr, "rev-parse", "HEAD")
        git(fixture.pr, "update-ref", "refs/remotes/origin/main", new_main)
        git(fixture.pr, "checkout", "-q", "docs/new-pages-20261022")
        git(fixture.pr, "merge", "-q", "--no-edit", "main")

        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/b.md", rekey=True)
        self.assertEqual(caught.exception.code, "REKEY_BASIS_REJECTED")
        admission = fixture.authority / first["records"][0]
        recorded = admission.read_bytes()

        result = fixture.admit("docs/b.md", rekey=True, approval_basis=None)

        self.assertEqual(result["base"], new_main)
        self.assertEqual(result["records"][0], first["records"][0], "the recorded decision is reused")
        self.assertEqual(admission.read_bytes(), recorded)
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).iterdir())), 2)
        self.assertNotEqual(result["candidate_policy_sha256"], first["candidate_policy_sha256"])
        delta = fixture.record(result["records"][2])
        main_policy = git(fixture.pr, "show", f"{new_main}:assets/public_source_profile.json") + "\n"
        self.assertEqual(delta["baseline_policy_sha256"], hashlib.sha256(main_policy.encode()).hexdigest())
        self.assertEqual(delta["allowed_delta"]["include_added"], ["docs/b.md"])
        self.assertEqual(result["admit_branch"], f"admit/new-pages-2026-10-22-{new_main[:8]}")
        self.assertIn("re-key", git(fixture.authority, "log", "-1", "--format=%s"))
        git(fixture.pr, "merge", "-q", "--no-edit", result["admit_branch"])

        git(fixture.pr, "checkout", "-q", "-b", "docs/c-page-20261022", new_main)
        fixture.add("docs/c.md", "# C\n")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/c.md", rekey=True, approval_basis=None)
        self.assertEqual(caught.exception.code, "REKEY_ADMISSION_MISSING")

    def test_rekey_reuses_a_recorded_class_stricter_than_the_derived_one(self) -> None:
        """A reviewer's recorded class and record id are the decision; a re-key matches on paths and bytes only."""
        record = {"id": "b-upstream-import", "classification": "upstream-third-party", "evidence_tier": "H",
                  "paths": ["docs/b.md"], "upstream": "a synthetic upstream"}
        fixture = _Fixture(self, records=[record])
        fixture.add("docs/b.md", "# B\n")
        first = fixture.admit("docs/b.md")
        admission = fixture.authority / first["records"][0]
        document = json.loads(admission.read_text(encoding="utf-8"))
        document["reviewed_new_paths"][0].update(
            classification="upstream_derived", record_id="b-upstream-import", origin_kind="third_party",
            origin="Byte-identical page from a synthetic upstream.", license="MIT")
        admission.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        git(fixture.authority, "commit", "-qam", "reviewer: stricter class for docs/b.md")

        git(fixture.pr, "checkout", "-q", "-b", "docs/a-page-20261022", fixture.base)
        fixture.add("docs/a.md", "# A\n")
        landed = fixture.admit("docs/a.md", approval_basis="Owner request: page A.")
        git(fixture.pr, "checkout", "-q", "main")
        git(fixture.pr, "merge", "-q", "--ff-only", landed["admit_branch"])
        new_main = git(fixture.pr, "rev-parse", "HEAD")
        git(fixture.pr, "update-ref", "refs/remotes/origin/main", new_main)
        git(fixture.pr, "checkout", "-q", "docs/new-pages-20261022")
        git(fixture.pr, "merge", "-q", "--no-edit", "main")

        result = fixture.admit("docs/b.md", rekey=True, approval_basis=None)

        self.assertEqual(result["records"][0], first["records"][0], "the recorded decision is reused")
        self.assertEqual(json.loads(admission.read_text(encoding="utf-8")), document, "and left as recorded")
        self.assertEqual(result["base"], new_main)

    def test_cli_prints_the_remaining_steps(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        out = io.StringIO()
        with mock.patch.object(admit_paths.tempfile, "tempdir", str(fixture.tmp)), contextlib.redirect_stdout(out):
            code = admit_paths.main([
                str(fixture.pr), "docs/new-page.md", "--approval-basis", "Owner request.",
                "--private-repo", str(fixture.authority), "--date", "2026-10-22",
            ])
        self.assertEqual(code, 0)
        text = out.getvalue()
        self.assertIn("docs/new-page.md: reviewed_documentation", text)
        self.assertIn("Left to do, in order:", text)
        self.assertIn("1. git -C", text)

    def test_default_slug(self) -> None:
        self.assertEqual(admit_paths.default_slug("docs/recomp-fyi-listing-20261022", [".recomp.json"]),
                         "recomp-fyi-listing")
        self.assertEqual(admit_paths.default_slug("feat/brand-mark-2026-10-21", ["x"]), "brand-mark")
        self.assertEqual(admit_paths.default_slug("", [".recomp.json"]), "recomp-json")


class RecompJsonRegressionTest(unittest.TestCase):
    """Replay the 2026-10-22 ``.recomp.json`` admission and require byte-identical output.

    The manual run took a pull request head that also changed
    ``tools/tree_lint_allow.json`` (``CANDIDATE_TREE_STALE``) and first wrote the
    wrong class (``ADMISSION_CLASS_MISMATCH``).  The tool starts from that same
    head and must produce the final records, the same candidate tree and the
    same generated controls without either correction.
    """

    BASE = "eccf84caa1820bbd0e01e2ccb1f7ac18e5484ae1"
    PR_HEAD = "727ee24aae195a7e33f85cc6c2b05fb6468e5612"
    ADMITTED = "cd82b3b109124bd63a4c6a842b48eae109c348ed"
    CANDIDATE_TREE = "0558207c855a8a5ec78cf8f9827f8ba9300de132"
    PROFILE_SHA = "427ea6ad58dfc629110c7aa3652dee5a167043f3d5b8b29850962c9e596ee6c7"
    ADMISSION = "docs/provenance/admissions/admission-recomp-fyi-listing-2026-10-22.json"

    def setUp(self) -> None:
        authority = os.environ.get("NK_PRIVATE_AUTHORITY")
        if not authority or not (Path(authority) / admit_paths.TRUSTED_LEDGER).is_file():
            self.skipTest("NOT_RUN: set NK_PRIVATE_AUTHORITY to the private authority checkout")
        for commit in (self.BASE, self.PR_HEAD, self.ADMITTED):
            if subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=ROOT,
                              capture_output=True).returncode:
                self.skipTest(f"NOT_RUN: public commit {commit[:8]} of the 2026-10-22 admission is not present")
        self.authority = Path(authority)

    def test_records_and_controls_are_byte_identical(self) -> None:
        records = [
            self.ADMISSION,
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/candidate-policy.json",
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/policy-delta-authority.json",
        ]
        # The private commit that added the records, and the latest form of each.
        added = git(self.authority, "log", "--diff-filter=A", "--format=%H", "--", self.ADMISSION).splitlines()[-1]
        expected = {path: git(self.authority, "log", "-1", "--format=%H", "--", path) for path in records}
        expected = {path: subprocess.run(["git", "show", f"{commit}:{path}"], cwd=self.authority,
                                         capture_output=True, check=True).stdout
                    for path, commit in expected.items()}
        basis = json.loads(expected[self.ADMISSION])["approval_basis"]

        fixture_root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.enterContext(mock.patch.dict(os.environ, GIT_ENV))
        pr = fixture_root / "pr"
        git(fixture_root, "clone", "-q", "--shared", "--no-checkout", str(ROOT), str(pr))
        git(pr, "checkout", "-q", "-b", "docs/recomp-fyi-listing-20261022", self.PR_HEAD)
        private = fixture_root / "private"
        git(fixture_root, "clone", "-q", "--shared", "--no-checkout", str(self.authority), str(private))
        git(private, "checkout", "-q", "-B", "main", f"{added}^")

        result = admit_paths.admit(
            pr_worktree=pr, paths=[".recomp.json"], approval_basis=basis, private_repo=private,
            base=self.BASE, date="2026-10-22", scratch_root=fixture_root,
        )

        self.assertEqual(result["records"], records)
        for path in records:
            with self.subTest(record=path):
                self.assertEqual((private / path).read_bytes(), expected[path])
        self.assertEqual(result["statements"], [{"path": ".recomp.json", "classification": "reviewed_configuration"}])
        self.assertEqual(result["candidate_tree"], self.CANDIDATE_TREE)
        self.assertEqual(git(pr, "rev-parse", f"{result['admission_commit']}^{{tree}}"),
                         git(pr, "rev-parse", f"{self.ADMITTED}^{{tree}}"))


if __name__ == "__main__":
    unittest.main()
