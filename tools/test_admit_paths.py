#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for ``tools/admit_paths.py``, single and batched public-path admissions.

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


def git_bytes(cwd: Path, *args: str) -> bytes:
    env = {**os.environ, **GIT_ENV}
    return subprocess.run(
        ["git", *args], cwd=cwd, env=env, check=True, capture_output=True,
    ).stdout


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

    def new_pr(self, slug: str, relative: str, content: str) -> tuple[Path, str]:
        """Create an independent PR worktree with its path and canonical profile committed."""
        branch = f"docs/{slug}-2026-10-22"
        if not hasattr(self, "_first_pr_created"):
            root = self.pr
            git(root, "checkout", "-q", "-b", branch, self.base)
            self._first_pr_created = True
        else:
            root = self.tmp / f"pr-{slug}"
            git(self.pr, "worktree", "add", "-q", "-b", branch, str(root), self.base)
        write(root, relative, content)
        git(root, "add", relative)
        git(root, "commit", "-qm", f"add {relative}")
        base_policy = (git(root, "show", f"{self.base}:assets/public_source_profile.json") + "\n").encode("utf-8")
        profile, _ = admit_paths._candidate_policy(base_policy, {relative})
        (root / "assets/public_source_profile.json").write_bytes(profile)
        git(root, "add", "assets/public_source_profile.json")
        git(root, "commit", "-qm", f"include {relative}")
        return root, branch

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

    def worktree(self, slug: str, *, base: str | None = None) -> Path:
        path = self.tmp / f"pr-{slug}"
        branch = f"docs/{slug}-20261022"
        git(self.pr, "worktree", "add", "-q", "-b", branch, str(path), base or self.base)
        return path

    def add_pr_path(self, worktree: Path, relative: str, content: str) -> str:
        write(worktree, relative, content)
        policy = json.loads((worktree / admit_paths.POLICY).read_text(encoding="utf-8"))
        policy["include_paths"] = sorted(set(policy["include_paths"]) | {relative})
        write(worktree, admit_paths.POLICY, admit_paths._canonical_profile_bytes(policy).decode("utf-8"))
        git(worktree, "add", relative, admit_paths.POLICY)
        git(worktree, "commit", "-qm", f"add {relative} and publication entry")
        return hashlib.sha256(content.encode("utf-8")).hexdigest()


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
        self.assertEqual(delta_dir.parent.name, hashlib.sha256(candidate_policy).hexdigest())
        self.assertEqual(delta_dir.name, hashlib.sha256(base_policy.encode()).hexdigest())
        delta = fixture.record(f"{delta_dir.as_posix()}/policy-delta-authority.json")
        self.assertEqual(delta["baseline_policy_sha256"], hashlib.sha256(base_policy.encode()).hexdigest())
        self.assertEqual(delta["candidate_policy_sha256"], delta_dir.parent.name)
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


class BatchAdmitPathsTest(unittest.TestCase):
    DATE = "2026-10-22"

    @staticmethod
    def profile_union(base_policy: bytes, paths: set[str]) -> bytes:
        document = json.loads(base_policy.decode("utf-8"))
        document["include_paths"] = sorted(set(document["include_paths"]) | paths)
        document["exclude_paths"] = sorted(set(document["exclude_paths"]))
        return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")

    def entry(self, worktree: Path, path: str, slug: str, *, approval: bool = True) -> dict:
        entry = {"pr_worktree": str(worktree), "paths": [path], "slug": slug}
        if approval:
            entry["approval_basis"] = f"Owner decision: {slug}."
        return entry

    def verify_delta(
        self, fixture: _Fixture, *, baseline: bytes, candidate: bytes, paths: list[str], record_dir: str,
    ) -> None:
        authority_path = fixture.authority / Path(*record_dir.split("/")) / "policy-delta-authority.json"
        allowed = admit_paths.provenance_ledger._read_policy_delta_authority(
            authority_path,
            candidate_root=fixture.pr,
            baseline_policy_bytes=baseline,
            candidate_policy_bytes=candidate,
        )
        expected = admit_paths.provenance_ledger._classify_policy_delta(
            json.loads(baseline.decode("utf-8")), json.loads(candidate.decode("utf-8")),
        )
        self.assertEqual(allowed, expected)
        self.assertEqual(allowed["include_added"], sorted(paths))
        self.assertEqual(allowed["include_removed"], [])
        self.assertEqual(allowed["exclude_added"], [])
        self.assertEqual(allowed["exclude_removed"], [])
        self.assertEqual(allowed["rule_changes"], [])

    def test_two_and_three_entry_batches_cover_every_subset_transition(self) -> None:
        for count in (2, 3):
            with self.subTest(count=count):
                fixture = _Fixture(self)
                entries = []
                entry_paths = []
                for index in range(count):
                    slug = f"batch-{count}-{index}"
                    relative = f"docs/{slug}.md"
                    pr = fixture.worktree(slug)
                    fixture.add_pr_path(pr, relative, f"# {slug}\n")
                    entries.append(self.entry(pr, relative, slug))
                    entry_paths.append(relative)

                result = admit_paths.admit_batch(
                    batch=entries, private_repo=fixture.authority, base=fixture.base,
                    date=self.DATE, scratch_root=fixture.tmp,
                )
                expected_count = count * (2 ** (count - 1))
                self.assertEqual(result["delta_count"], expected_count)
                self.assertEqual(len(result["deltas"]), expected_count)
                admissions = sorted((fixture.authority / admit_paths.ADMISSIONS).glob("*.json"))
                self.assertEqual(len(admissions), count)
                committed_paths = git(fixture.authority, "show", "--format=", "--name-only", "HEAD").splitlines()
                self.assertEqual(set(committed_paths), set(result["records"]))
                self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

                base_policy = git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")
                for delta in result["deltas"]:
                    selected = set(delta["subset_slugs"])
                    candidate_slug = delta["entry_slug"]
                    candidate_index = next(i for i in range(count) if entries[i]["slug"] == candidate_slug)
                    selected_indices = {
                        i for i, entry in enumerate(entries) if entry["slug"] in selected
                    }
                    selected_paths = {entry_paths[i] for i in selected_indices}
                    baseline = base_policy if not selected_paths else self.profile_union(base_policy, selected_paths)
                    candidate_paths = selected_paths | {entry_paths[candidate_index]}
                    candidate = self.profile_union(base_policy, candidate_paths)
                    baseline_sha = hashlib.sha256(baseline).hexdigest()
                    candidate_sha = hashlib.sha256(candidate).hexdigest()
                    self.assertEqual(delta["baseline_policy_sha256"], baseline_sha)
                    self.assertEqual(delta["candidate_policy_sha256"], candidate_sha)
                    record_dir = f"{admit_paths.POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
                    self.assertEqual(delta["records"], [
                        f"{record_dir}/candidate-policy.json",
                        f"{record_dir}/policy-delta-authority.json",
                    ])
                    self.assertEqual(
                        (fixture.authority / record_dir / "candidate-policy.json").read_bytes(), candidate,
                    )
                    self.verify_delta(
                        fixture, baseline=baseline, candidate=candidate,
                        paths=[entry_paths[candidate_index]], record_dir=record_dir,
                    )

                for entry_result in result["entries"]:
                    admission = json.loads((fixture.authority / entry_result["records"][0]).read_text())
                    self.assertEqual(
                        [statement["path"] for statement in admission["reviewed_new_paths"]],
                        entry_result["paths"],
                    )

    def test_two_entry_batches_merge_in_both_orders_with_canonical_profiles(self) -> None:
        for order in ((0, 1), (1, 0)):
            with self.subTest(order=order):
                fixture = _Fixture(self)
                git(fixture.pr, "checkout", "-q", "main")
                base_document = json.loads(git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}"))
                base_document["exclude_paths"] = ["docs/private-z.md", "docs/private-a.md"]
                write(
                    fixture.pr, admit_paths.POLICY,
                    json.dumps(base_document, indent=2, ensure_ascii=False) + "\n",
                )
                git(fixture.pr, "add", admit_paths.POLICY)
                git(fixture.pr, "commit", "-qm", "make base exclusion order non-canonical")
                fixture.base = git(fixture.pr, "rev-parse", "HEAD")
                git(fixture.pr, "update-ref", "refs/remotes/origin/main", fixture.base)
                paths = ["docs/order-a.md", "docs/order-b.md"]
                slugs = ["order-a", "order-b"]
                prs = [fixture.worktree(slug) for slug in slugs]
                for index in range(2):
                    fixture.add_pr_path(prs[index], paths[index], f"# {slugs[index]}\n")
                result = admit_paths.admit_batch(
                    batch=[self.entry(prs[i], paths[i], slugs[i]) for i in range(2)],
                    private_repo=fixture.authority, base=fixture.base, date=self.DATE,
                    scratch_root=fixture.tmp,
                )

                landing = fixture.worktree(f"landing-{order[0]}-{order[1]}")
                first_commit = git(prs[order[0]], "rev-parse", "HEAD")
                second_commit = git(prs[order[1]], "rev-parse", "HEAD")
                git(landing, "merge", "--ff-only", first_commit)
                first_policy = git_bytes(landing, "show", f"HEAD:{admit_paths.POLICY}")
                merge = subprocess.run(
                    ["git", "merge", "--no-commit", "--no-ff", second_commit],
                    cwd=landing, env={**os.environ, **GIT_ENV}, capture_output=True, check=False,
                )
                self.assertTrue(merge.returncode in (0, 1), merge.stderr.decode("utf-8", errors="replace"))
                git(landing, "rev-parse", "--verify", "MERGE_HEAD")
                admit_paths.merge_profile(landing)
                git(landing, "add", admit_paths.POLICY)
                git(landing, "commit", "-qm", "merge path-adding PR")

                base_policy = git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")
                expected_profile = self.profile_union(base_policy, set(paths))
                actual_profile = git_bytes(landing, "show", f"HEAD:{admit_paths.POLICY}")
                self.assertEqual(actual_profile, expected_profile)
                candidate_sha = hashlib.sha256(expected_profile).hexdigest()
                baseline_sha = hashlib.sha256(first_policy).hexdigest()
                record_dir = f"{admit_paths.POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
                self.assertTrue((fixture.authority / record_dir / "candidate-policy.json").is_file())
                self.assertTrue((fixture.authority / record_dir / "policy-delta-authority.json").is_file())
                self.assertEqual(
                    (fixture.authority / record_dir / "candidate-policy.json").read_bytes(), expected_profile,
                )
                self.verify_delta(
                    fixture, baseline=first_policy, candidate=expected_profile,
                    paths=[paths[order[1]]], record_dir=record_dir,
                )

    def test_batch_has_no_entry_count_cap(self) -> None:
        five = [
            {"pr_worktree": "C:/temporary/pr", "paths": [f"docs/path-{index}.md"],
             "slug": f"path-{index}", "approval_basis": "Owner approved this synthetic path."}
            for index in range(5)
        ]
        self.assertEqual(len(admit_paths._batch_entries(five)), 5)

    def test_seven_entry_chain_and_batch_resume_after_three_land(self) -> None:
        fixture = _Fixture(self)
        paths = [f"docs/chain-{index}.md" for index in range(7)]
        slugs = [f"chain-{index}" for index in range(7)]
        prs = [fixture.worktree(slug) for slug in slugs]
        entries = []
        for index, pr in enumerate(prs):
            fixture.add_pr_path(pr, paths[index], f"# {slugs[index]}\n")
            entries.append(self.entry(pr, paths[index], slugs[index]))
        first = admit_paths.admit_batch(
            batch=entries, private_repo=fixture.authority, base=fixture.base, date=self.DATE,
            scratch_root=fixture.tmp,
        )
        self.assertEqual(first["delta_count"], 7)
        self.assertEqual([delta["subset_slugs"] for delta in first["deltas"]], [slugs[:i] for i in range(7)])

        base_policy = git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")
        prefix: set[str] = set()
        for index in range(7):
            baseline = base_policy if not prefix else self.profile_union(base_policy, prefix)
            candidate = self.profile_union(base_policy, prefix | {paths[index]})
            candidate_sha = hashlib.sha256(candidate).hexdigest()
            baseline_sha = hashlib.sha256(baseline).hexdigest()
            record_dir = f"{admit_paths.POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
            self.verify_delta(
                fixture, baseline=baseline, candidate=candidate,
                paths=[paths[index]], record_dir=record_dir,
            )
            prefix.add(paths[index])

        landing = fixture.worktree("landed-chain")
        landed = set(paths[:3])
        for index in range(3):
            git(landing, "checkout", first["entries"][index]["admit_branch"], "--", paths[index])
        write(landing, admit_paths.POLICY, self.profile_union(base_policy, landed).decode("utf-8"))
        git(landing, "add", *paths[:3], admit_paths.POLICY)
        git(landing, "commit", "-qm", "land three admitted paths")
        new_base = git(landing, "rev-parse", "HEAD")
        git(landing, "update-ref", "refs/remotes/origin/main", new_base)
        new_base_policy = git_bytes(landing, "show", f"{new_base}:{admit_paths.POLICY}")

        remaining_order = list(reversed(range(3, 7)))
        resume_entries = []
        for index in remaining_order:
            slug = slugs[index]
            pr = fixture.worktree(f"resume-{slug}", base=new_base)
            git(pr, "checkout", first["entries"][index]["admit_branch"], "--", paths[index])
            write(pr, admit_paths.POLICY, self.profile_union(new_base_policy, {paths[index]}).decode("utf-8"))
            git(pr, "add", paths[index], admit_paths.POLICY)
            git(pr, "commit", "-qm", f"merge updated base for {slug}")
            resume_entries.append(self.entry(pr, paths[index], slug, approval=False))

        batch_file = fixture.tmp / "remaining.json"
        batch_file.write_text(json.dumps(resume_entries, indent=2) + "\n", encoding="utf-8")
        output = io.StringIO()
        with mock.patch.object(admit_paths.tempfile, "tempdir", str(fixture.tmp)), contextlib.redirect_stdout(output):
            code = admit_paths.main([
                "--batch-resume", str(batch_file), "--private-repo", str(fixture.authority),
                "--base", new_base, "--date", self.DATE,
            ])
        self.assertEqual(code, 0)
        self.assertIn("pre-issued 4 remaining listed-order chain", output.getvalue())
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).glob("*.json"))), 7)
        resume_paths = git(fixture.authority, "show", "--format=", "--name-only", "HEAD").splitlines()
        self.assertFalse(any(path.startswith(f"{admit_paths.ADMISSIONS}/") for path in resume_paths))
        self.assertEqual(len([path for path in resume_paths if path.endswith("policy-delta-authority.json")]), 4)

        prefix = set()
        for index in remaining_order:
            baseline = new_base_policy if not prefix else self.profile_union(new_base_policy, prefix)
            candidate = self.profile_union(new_base_policy, prefix | {paths[index]})
            candidate_sha = hashlib.sha256(candidate).hexdigest()
            baseline_sha = hashlib.sha256(baseline).hexdigest()
            record_dir = f"{admit_paths.POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
            self.verify_delta(
                fixture, baseline=baseline, candidate=candidate,
                paths=[paths[index]], record_dir=record_dir,
            )
            prefix.add(paths[index])

    def test_overlapping_batch_paths_are_rejected_before_mutation(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/shared.md", "# Shared\n")
        before = git(fixture.authority, "rev-parse", "HEAD")
        entries = [
            self.entry(fixture.pr, "docs/shared.md", "shared-a"),
            self.entry(fixture.pr, "docs/shared.md", "shared-b"),
        ]
        with self.assertRaises(AdmitError) as caught:
            admit_paths.admit_batch(batch=entries, private_repo=fixture.authority, base=fixture.base)
        self.assertEqual(caught.exception.code, "BATCH_PATH_OVERLAP")
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), before)
        self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

    def test_merge_profile_refuses_non_path_field_changes(self) -> None:
        fixture = _Fixture(self)
        ours = fixture.worktree("profile-ours")
        fixture.add_pr_path(ours, "docs/ours.md", "# Ours\n")
        theirs = fixture.worktree("profile-theirs")
        fixture.add_pr_path(theirs, "docs/theirs.md", "# Theirs\n")
        theirs_policy = json.loads((theirs / admit_paths.POLICY).read_text(encoding="utf-8"))
        theirs_policy["build_mode"] = "PUBLIC_SAFE=0"
        write(theirs, admit_paths.POLICY, admit_paths._canonical_profile_bytes(theirs_policy).decode("utf-8"))
        git(theirs, "add", admit_paths.POLICY)
        git(theirs, "commit", "-qm", "change a non-path profile field")
        merge = subprocess.run(
            ["git", "merge", "--no-commit", "--no-ff", git(theirs, "rev-parse", "HEAD")],
            cwd=ours, env={**os.environ, **GIT_ENV}, capture_output=True, check=False,
        )
        self.assertIn(merge.returncode, (0, 1))
        with self.assertRaises(AdmitError) as caught:
            admit_paths.merge_profile(ours)
        self.assertEqual(caught.exception.code, "MERGE_PROFILE_NON_PATH_FIELD_CHANGED")
        git(ours, "merge", "--abort")

    def test_nested_delta_verifies_and_legacy_flat_delta_remains_readable(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        result = fixture.admit("docs/new-page.md")
        nested = Path(result["records"][1]).parent
        candidate_policy = (fixture.authority / nested / "candidate-policy.json").read_bytes()
        baseline_policy = (git(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}") + "\n").encode()
        nested_authority = fixture.authority / nested / "policy-delta-authority.json"
        verified = admit_paths.provenance_ledger._read_policy_delta_authority(
            nested_authority, candidate_root=fixture.pr,
            baseline_policy_bytes=baseline_policy, candidate_policy_bytes=candidate_policy,
        )
        self.assertEqual(verified["include_added"], ["docs/new-page.md"])

        flat = fixture.authority / admit_paths.POLICY_DELTAS / nested.parent.name
        flat.mkdir(parents=True, exist_ok=True)
        (flat / "candidate-policy.json").write_bytes(candidate_policy)
        (flat / "policy-delta-authority.json").write_bytes(nested_authority.read_bytes())
        legacy_verified = admit_paths.provenance_ledger._read_policy_delta_authority(
            flat / "policy-delta-authority.json", candidate_root=fixture.pr,
            baseline_policy_bytes=baseline_policy, candidate_policy_bytes=candidate_policy,
        )
        self.assertEqual(legacy_verified, verified)


class BatchAdmissionTest(unittest.TestCase):
    def _entries(self, fixture: _Fixture, count: int) -> tuple[list[dict], list[tuple[Path, str, str]]]:
        entries = []
        prs = []
        for index in range(count):
            slug = f"batch-{chr(ord('a') + index)}"
            path = f"docs/{chr(ord('a') + index)}-batch.md"
            root, branch = fixture.new_pr(slug, path, f"# {slug}\n")
            entries.append({
                "pr_worktree": str(root),
                "paths": [path],
                "slug": slug,
                "approval_basis": f"Owner decision for {slug}.",
            })
            prs.append((root, branch, path))
        return entries, prs

    def _assert_every_transition(self, fixture: _Fixture, result: dict) -> None:
        base_policy = (git(fixture.pr, "show", f"{result['base']}:{admit_paths.POLICY}") + "\n").encode("utf-8")
        path_sets = [set(entry["paths"]) for entry in result["entries"]]
        for mask in range(1 << len(path_sets)):
            selected = [i for i in range(len(path_sets)) if mask & (1 << i)]
            subset_paths = set().union(*(path_sets[i] for i in selected)) if selected else set()
            baseline_policy = base_policy if not selected else admit_paths._candidate_policy(base_policy, subset_paths)[0]
            baseline_document = json.loads(baseline_policy)
            baseline_sha = hashlib.sha256(baseline_policy).hexdigest()
            for candidate_index, paths in enumerate(path_sets):
                if mask & (1 << candidate_index):
                    continue
                candidate_policy = admit_paths._candidate_policy(base_policy, subset_paths | paths)[0]
                candidate_sha = hashlib.sha256(candidate_policy).hexdigest()
                delta_dir = Path(admit_paths.POLICY_DELTAS) / candidate_sha / baseline_sha
                candidate_path = fixture.authority / delta_dir / "candidate-policy.json"
                authority_path = fixture.authority / delta_dir / "policy-delta-authority.json"
                with self.subTest(subset=selected, candidate=candidate_index):
                    self.assertEqual(candidate_path.read_bytes(), candidate_policy)
                    authority = admit_paths.provenance_ledger._read_policy_delta_authority(
                        authority_path, candidate_root=fixture.pr,
                        baseline_policy_bytes=baseline_policy, candidate_policy_bytes=candidate_policy,
                    )
                    candidate_document = json.loads(candidate_policy)
                    classified = admit_paths.provenance_ledger._classify_policy_delta(
                        baseline_document, candidate_document,
                    )
                    expected = {key: sorted(set(value)) for key, value in classified.items()}
                    self.assertEqual(authority, expected)
                    self.assertEqual(authority["include_added"], sorted(paths))
                    self.assertEqual(authority["include_removed"], [])
                    self.assertEqual(authority["exclude_added"], [])
                    self.assertEqual(authority["exclude_removed"], [])
                    self.assertEqual(authority["rule_changes"], [])

    def _assert_one_private_commit(self, fixture: _Fixture, result: dict, before: str) -> None:
        self.assertEqual(result["private_commit"], git(fixture.authority, "rev-parse", "HEAD"))
        self.assertEqual(git(fixture.authority, "rev-parse", f"{result['private_commit']}^"), before)
        committed = sorted(git(fixture.authority, "show", "--name-only", "--format=", "HEAD").splitlines())
        self.assertEqual(committed, result["records"])
        self.assertEqual(git(fixture.authority, "status", "--porcelain", "--", "docs/provenance"), "")
        self.assertFalse(result["pushed"])

    def test_two_pr_batch_preissues_every_transition_in_one_private_commit(self) -> None:
        fixture = _Fixture(self)
        batch, prs = self._entries(fixture, 2)
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        result = admit_paths.admit_batch(
            batch=batch, private_repo=fixture.authority, base=fixture.base,
            date="2026-10-22", scratch_root=fixture.tmp,
        )
        self.assertEqual(result["base"], fixture.base)
        self.assertEqual(result["delta_count"], 4)
        self._assert_every_transition(fixture, result)
        self._assert_one_private_commit(fixture, result, private_before)
        self.assertEqual(len({entry["records"][0] for entry in result["entries"]}), 2)
        for entry, (root, _branch, path) in zip(result["entries"], prs, strict=True):
            self.assertEqual(entry["base"], fixture.base)
            self.assertEqual(entry["paths"], [path])
            self.assertEqual(git(root, "merge-base", entry["admit_branch"], fixture.base), fixture.base)

    def test_three_pr_batch_preissues_every_transition_in_one_private_commit(self) -> None:
        fixture = _Fixture(self)
        batch, _prs = self._entries(fixture, 3)
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        result = admit_paths.admit_batch(
            batch=batch, private_repo=fixture.authority, base=fixture.base,
            date="2026-10-22", scratch_root=fixture.tmp,
        )
        self.assertEqual(result["delta_count"], 12)
        self._assert_every_transition(fixture, result)
        self._assert_one_private_commit(fixture, result, private_before)

    def test_batch_cli_reads_json(self) -> None:
        fixture = _Fixture(self)
        batch, _prs = self._entries(fixture, 2)
        batch_file = fixture.tmp / "batch.json"
        batch_file.write_text(json.dumps(batch), encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = admit_paths.main([
                "--batch", str(batch_file), "--base", fixture.base,
                "--private-repo", str(fixture.authority), "--date", "2026-10-22",
                "--any-order-limit", "1",
            ])
        self.assertEqual(code, 0)
        self.assertIn("admitted 2 pull requests on shared base", out.getvalue())
        self.assertIn("pre-issued 2 subset/next-landing policy deltas", out.getvalue())

    def test_batch_refuses_overlapping_paths_and_has_no_size_cap(self) -> None:
        fixture = _Fixture(self)
        first, _ = fixture.new_pr("overlap-a", "docs/shared.md", "# Shared A\n")
        second, _ = fixture.new_pr("overlap-b", "docs/shared.md", "# Shared B\n")
        overlapping = [
            {"pr_worktree": str(root), "paths": ["docs/shared.md"], "slug": slug,
             "approval_basis": f"Owner decision for {slug}."}
            for root, slug in ((first, "overlap-a"), (second, "overlap-b"))
        ]
        with self.assertRaises(AdmitError) as caught:
            admit_paths._batch_entries(overlapping)
        self.assertEqual(caught.exception.code, "BATCH_PATH_OVERLAP")

        five = [
            {"pr_worktree": str(fixture.pr), "paths": [f"docs/path-{index}.md"], "slug": f"path-{index}",
             "approval_basis": "Owner approved this synthetic path."}
            for index in range(5)
        ]
        self.assertEqual(len(admit_paths._batch_entries(five)), 5)

    def _landing_fixture(self, first: int) -> None:
        fixture = _Fixture(self)
        batch, prs = self._entries(fixture, 2)
        result = admit_paths.admit_batch(
            batch=batch, private_repo=fixture.authority, base=fixture.base,
            date="2026-10-22", scratch_root=fixture.tmp,
        )
        first_root, first_branch, _first_path = prs[first]
        waiting_root, _waiting_branch, waiting_path = prs[1 - first]
        waiting_entry = result["entries"][1 - first]

        git(first_root, "checkout", "-q", "main")
        git(first_root, "merge", "-q", "--ff-only", first_branch)
        baseline_policy = subprocess.run(
            ["git", "show", f"main:{admit_paths.POLICY}"], cwd=first_root, env={**os.environ, **GIT_ENV},
            check=True, capture_output=True,
        ).stdout
        merge = subprocess.run(
            ["git", "merge", "--no-commit", "--no-ff", "main"], cwd=waiting_root,
            env={**os.environ, **GIT_ENV}, check=False, capture_output=True,
        )
        self.assertIn(merge.returncode, (0, 1), merge.stderr.decode("utf-8", errors="replace"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = admit_paths.main(["--merge-profile", str(waiting_root)])
        self.assertEqual(code, 0, out.getvalue())
        profile = (waiting_root / admit_paths.POLICY).read_bytes()
        expected_paths = set(prs[first][2:3]) | {waiting_path}
        expected_policy = admit_paths._candidate_policy(
            (git(waiting_root, "show", f"{fixture.base}:{admit_paths.POLICY}") + "\n").encode("utf-8"),
            expected_paths,
        )[0]
        self.assertEqual(profile, expected_policy)
        git(waiting_root, "add", admit_paths.POLICY)
        git(waiting_root, "commit", "-qm", "merge the other admitted path")

        candidate_sha = hashlib.sha256(profile).hexdigest()
        baseline_sha = hashlib.sha256(baseline_policy).hexdigest()
        delta_dir = Path(admit_paths.POLICY_DELTAS) / candidate_sha / baseline_sha
        candidate_path = fixture.authority / delta_dir / "candidate-policy.json"
        authority_path = fixture.authority / delta_dir / "policy-delta-authority.json"
        self.assertEqual(candidate_path.read_bytes(), profile)
        verified = admit_paths.provenance_ledger._read_policy_delta_authority(
            authority_path, candidate_root=waiting_root,
            baseline_policy_bytes=baseline_policy, candidate_policy_bytes=profile,
        )
        self.assertEqual(verified["include_added"], [waiting_path])
        self.assertEqual(verified["include_removed"], [])
        self.assertEqual(verified["rule_changes"], [])

    def test_both_two_pr_landing_orders_merge_to_preissued_profile(self) -> None:
        with self.subTest(order="a then b"):
            self._landing_fixture(0)
        with self.subTest(order="b then a"):
            self._landing_fixture(1)

    def test_merge_profile_refuses_non_path_field_changes(self) -> None:
        fixture = _Fixture(self)
        ours = fixture.pr
        git(ours, "checkout", "-q", "-b", "docs/profile-ours", fixture.base)
        baseline_policy = (git(ours, "show", f"{fixture.base}:{admit_paths.POLICY}") + "\n").encode()
        ours_document = json.loads(baseline_policy)
        ours_document["description"] = "ours changed the policy"
        (ours / admit_paths.POLICY).write_bytes(admit_paths._canonical_profile_bytes(ours_document))
        git(ours, "add", admit_paths.POLICY)
        git(ours, "commit", "-qm", "change a non-path field on ours")

        theirs = fixture.tmp / "profile-theirs"
        git(ours, "worktree", "add", "-q", "-b", "docs/profile-theirs", str(theirs), fixture.base)
        theirs_document = json.loads(baseline_policy)
        theirs_document["description"] = "theirs changed the policy"
        (theirs / admit_paths.POLICY).write_bytes(admit_paths._canonical_profile_bytes(theirs_document))
        git(theirs, "add", admit_paths.POLICY)
        git(theirs, "commit", "-qm", "change a non-path field on theirs")
        merge = subprocess.run(
            ["git", "merge", "--no-commit", "--no-ff", "docs/profile-theirs"], cwd=ours,
            env={**os.environ, **GIT_ENV}, check=False, capture_output=True,
        )
        self.assertEqual(merge.returncode, 1)
        conflicted = (ours / admit_paths.POLICY).read_bytes()
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = admit_paths.main(["--merge-profile", str(ours)])
        self.assertEqual(code, 1)
        self.assertIn("MERGE_PROFILE_NON_PATH_FIELD_CHANGED", err.getvalue())
        self.assertEqual((ours / admit_paths.POLICY).read_bytes(), conflicted)


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
        baseline_policy = git(ROOT, "show", f"{self.BASE}:assets/public_source_profile.json") + "\n"
        baseline_sha = hashlib.sha256(baseline_policy.encode()).hexdigest()
        records = [
            self.ADMISSION,
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/{baseline_sha}/candidate-policy.json",
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/{baseline_sha}/policy-delta-authority.json",
        ]
        # The private commit that added the records, and the latest form of each.
        added = git(self.authority, "log", "--diff-filter=A", "--format=%H", "--", self.ADMISSION).splitlines()[-1]
        historical_records = [
            self.ADMISSION,
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/candidate-policy.json",
            f"docs/provenance/policy-deltas/{self.PROFILE_SHA}/policy-delta-authority.json",
        ]
        expected = {path: git(self.authority, "log", "-1", "--format=%H", "--", path)
                    for path in historical_records}
        expected = dict(zip(records, expected.values(), strict=True))
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
