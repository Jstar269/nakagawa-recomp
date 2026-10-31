#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for ``tools/admit_paths.py``, single and batched public-path admissions.

The synthetic cases build a real public repository and a real stand-in for the
private authority repository, then run the whole admission, including the
real ``admit-new-reviewed``.  ``RecompJsonRegressionTest`` replays the
2026-10-22 ``.recomp.json`` admission and requires the same admission record
and the same tree, apart from the canonical profile and the claims ledger; it
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
import provenance_attest_verify as verifier  # noqa: E402
import provenance_ledger  # noqa: E402

#: Commits run by the tool need an identity, and a host's own Git configuration
#: must not change what the tests observe.
#:
#: Every git command a fixture or the tool runs must also be finished when it
#: returns. `git commit`, `merge` and `fetch` otherwise start
#: `git maintenance run --auto --detach`, and Git 2.55's default geometric
#: strategy repacks in that detached process whenever two or more loose objects
#: share the objects/17 directory, which depends on the commit hashes. The repack
#: then writes .git/objects while TemporaryDirectory removes the tree, and the
#: cleanup fails with ENOTEMPTY. The same settings as
#: nk_core/git_isolation.isolated_git_env, in the environment so that the
#: repositories the tests create keep their default configuration.
GIT_ENV = {
    "GIT_AUTHOR_NAME": "Nakagawa Recomp Tests",
    "GIT_AUTHOR_EMAIL": "tests@nakagawa-recomp.invalid",
    "GIT_COMMITTER_NAME": "Nakagawa Recomp Tests",
    "GIT_COMMITTER_EMAIL": "tests@nakagawa-recomp.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_COUNT": "3",
    "GIT_CONFIG_KEY_0": "gc.auto",
    "GIT_CONFIG_VALUE_0": "0",
    "GIT_CONFIG_KEY_1": "maintenance.auto",
    "GIT_CONFIG_VALUE_1": "false",
    "GIT_CONFIG_KEY_2": "gc.autoDetach",
    "GIT_CONFIG_VALUE_2": "false",
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


def assert_v3_delta(
    testcase: unittest.TestCase, fixture: "_Fixture", record: str, *,
    baseline: bytes, candidate: bytes, files: dict[str, bytes],
) -> dict:
    """The stored V3 delta is named after its bytes and approves exactly ``baseline`` -> ``candidate``.

    ``files`` are the candidate's new files; the delta binds each one's SHA-256.
    """
    testcase.assertTrue(record.startswith(f"{admit_paths.POLICY_DELTAS_V3}/"), record)
    raw = (fixture.authority / record).read_bytes()
    testcase.assertEqual(Path(record).stem, hashlib.sha256(raw).hexdigest())
    with tempfile.TemporaryDirectory() as scratch:
        policy_file = Path(scratch) / "candidate-policy.json"
        policy_file.write_bytes(candidate)
        candidate_policy = admit_paths.load_policy(policy_file)
    return provenance_ledger.verify_policy_delta_v3(
        raw, baseline_policy_bytes=baseline, candidate_policy_bytes=candidate,
        base_blobs={}, candidate_blobs=files, candidate_policy=candidate_policy,
    )


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
        candidate_policy, _ = admit_paths._candidate_policy(base_policy.encode(), {"docs/new-page.md"})
        # One V3 delta and no nested V1/V2 record (#1673): it binds the new file's bytes.
        record = result["records"][1]
        self.assertEqual(result["policy_delta_record"], record)
        self.assertEqual(len(result["records"]), 2)
        self.assertEqual(fixture.record(record), {
            "exclude_added": [], "exclude_removed": [],
            "include_added": [{"path": "docs/new-page.md", "sha256": digest}], "include_removed": [],
            "kind": "policy-delta-authority", "rule_changes": [], "schema_version": 3,
        })
        allowed = assert_v3_delta(self, fixture, record, baseline=base_policy.encode(), candidate=candidate_policy,
                                  files={"docs/new-page.md": b"# New page\n"})
        self.assertEqual(allowed["include_added"], ["docs/new-page.md"])
        # The name the attestation derives from the admission commit is the stored file's name.
        self.assertEqual(
            verifier.policy_delta_key(repo=fixture.pr, base_rev=fixture.base, candidate_rev=result["admission_commit"]),
            Path(record).stem,
        )

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
        # The branch carries the committed claims form (#1673): the merge adds the new entry
        # and rewrites nothing else. The bytes stay bound by the private admission record.
        ledger = json.loads(git(fixture.pr, "show", f"{branch}:assets/public_provenance_ledger.json"))
        self.assertTrue(provenance_ledger.is_claims_ledger(ledger))
        self.assertNotIn("admission", ledger)
        entry = next(entry for entry in ledger["entries"] if entry["path"] == "docs/new-page.md")
        self.assertEqual(entry["classification"], "reviewed_documentation")
        self.assertNotIn("sha256", entry)
        admission_digest = fixture.record(result["records"][0])["reviewed_new_paths"][0]["sha256"]
        self.assertEqual(admission_digest, digest)
        self.assertIn("Assisted-by: Test Tool (test-model)", git(fixture.pr, "log", "-1", "--format=%B", branch))
        self.assertNotIn("Signed-off-by", git(fixture.pr, "log", "-2", "--format=%B", branch))

        # Only the two records were committed in the authority checkout; the
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
        """A lands first. B's V3 delta still holds on the new main; a pre-V3 admission re-keys once."""
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
        private_before = git(fixture.authority, "rev-parse", "HEAD")

        # B's V3 delta from before A landed already approves B's change on the new main.
        main_policy = (git(fixture.pr, "show", f"{new_main}:assets/public_source_profile.json") + "\n").encode()
        union_policy, _ = admit_paths._candidate_policy(main_policy, {"docs/b.md"})
        assert_v3_delta(self, fixture, first["policy_delta_record"], baseline=main_policy, candidate=union_policy,
                        files={"docs/b.md": b"# B\n"})

        result = fixture.admit("docs/b.md", rekey=True, approval_basis=None)

        self.assertEqual(result["base"], new_main)
        self.assertEqual(result["records"], first["records"], "the same decision and the same V3 delta")
        self.assertEqual(admission.read_bytes(), recorded)
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), private_before, "nothing new to record")
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).iterdir())), 2)
        self.assertNotEqual(result["candidate_policy_sha256"], first["candidate_policy_sha256"])
        self.assertEqual(result["admit_branch"], f"admit/new-pages-2026-10-22-{new_main[:8]}")
        git(fixture.pr, "merge", "-q", "--no-edit", result["admit_branch"])

        # An admission recorded before V3 deltas existed has no V3 file: the re-key writes it, once.
        git(fixture.authority, "rm", "-q", first["policy_delta_record"])
        git(fixture.authority, "commit", "-qm", "an admission from before V3 deltas")
        git(fixture.pr, "branch", "-q", "-D", result["admit_branch"])
        rekeyed = fixture.admit("docs/b.md", rekey=True, approval_basis=None)
        self.assertEqual(rekeyed["policy_delta_record"], first["policy_delta_record"])
        self.assertTrue((fixture.authority / rekeyed["policy_delta_record"]).is_file())
        self.assertIn("re-key", git(fixture.authority, "log", "-1", "--format=%s"))

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
        self, fixture: _Fixture, *, baseline: bytes, candidate: bytes, path: str, record: str,
    ) -> None:
        """The entry's V3 delta approves exactly its own path, on the given base."""
        allowed = assert_v3_delta(self, fixture, record, baseline=baseline, candidate=candidate,
                                  files={path: f"# {Path(path).stem}\n".encode()})
        self.assertEqual(allowed, {"exclude_added": [], "exclude_removed": [], "include_added": [path],
                                   "include_removed": [], "rule_changes": []})

    def test_two_and_three_entry_batches_write_one_v3_delta_each_valid_in_any_order(self) -> None:
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
                self.assertEqual(result["delta_count"], count)
                self.assertEqual([delta["paths"] for delta in result["deltas"]], [[path] for path in entry_paths])
                admissions = sorted((fixture.authority / admit_paths.ADMISSIONS).glob("*.json"))
                self.assertEqual(len(admissions), count)
                committed_paths = git(fixture.authority, "show", "--format=", "--name-only", "HEAD").splitlines()
                self.assertEqual(set(committed_paths), set(result["records"]))
                self.assertEqual(len(result["records"]), 2 * count, "one admission and one V3 delta per entry")
                self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

                # Each delta holds on every base the others' landings can produce.
                base_policy = git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")
                for index, delta in enumerate(result["deltas"]):
                    others = [path for other, path in enumerate(entry_paths) if other != index]
                    for mask in range(1 << len(others)):
                        landed = {others[bit] for bit in range(len(others)) if mask & (1 << bit)}
                        baseline = base_policy if not landed else self.profile_union(base_policy, landed)
                        with self.subTest(entry=index, landed=sorted(landed)):
                            self.verify_delta(
                                fixture, baseline=baseline,
                                candidate=self.profile_union(base_policy, landed | {entry_paths[index]}),
                                path=entry_paths[index], record=delta["record"],
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
                batch = admit_paths.admit_batch(
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
                # The second PR's own V3 delta, issued against the old base, approves the merge.
                second = next(delta for delta in batch["deltas"] if delta["paths"] == [paths[order[1]]])
                self.verify_delta(
                    fixture, baseline=first_policy, candidate=expected_profile,
                    path=paths[order[1]], record=second["record"],
                )

    def test_batch_has_no_entry_count_cap(self) -> None:
        five = [
            {"pr_worktree": "C:/temporary/pr", "paths": [f"docs/path-{index}.md"],
             "slug": f"path-{index}", "approval_basis": "Owner approved this synthetic path."}
            for index in range(5)
        ]
        self.assertEqual(len(admit_paths._batch_entries(five)), 5)

    def test_seven_entry_batch_lands_in_any_order_and_resume_records_nothing_new(self) -> None:
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

        base_policy = git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")
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

        # The four waiting deltas hold on the new base in any order: no re-key.
        remaining_order = list(reversed(range(3, 7)))
        prefix: set[str] = set()
        for index in remaining_order:
            baseline = new_base_policy if not prefix else self.profile_union(new_base_policy, prefix)
            self.verify_delta(
                fixture, baseline=baseline, candidate=self.profile_union(new_base_policy, prefix | {paths[index]}),
                path=paths[index], record=first["deltas"][index]["record"],
            )
            prefix.add(paths[index])

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
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        output = io.StringIO()
        with mock.patch.object(admit_paths.tempfile, "tempdir", str(fixture.tmp)), contextlib.redirect_stdout(output):
            code = admit_paths.main([
                "--batch-resume", str(batch_file), "--private-repo", str(fixture.authority),
                "--base", new_base, "--date", self.DATE,
            ])
        self.assertEqual(code, 0)
        self.assertIn("re-issued V3 deltas for 4 pull requests", output.getvalue())
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).glob("*.json"))), 7)
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), private_before,
                         "the V3 deltas re-derived on the new base are the ones already recorded")

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

    def test_merge_profile_keeps_a_path_the_other_side_moved_or_removed(self) -> None:
        # main publishes an excluded path and drops an included one while the pull
        # request adds its own: a union of each list would keep the path in both lists
        # and bring the dropped one back.
        fixture = _Fixture(self)
        ours = fixture.worktree("profile-ours")
        fixture.add_pr_path(ours, "docs/ours.md", "# Ours\n")
        theirs = fixture.worktree("profile-theirs")
        write(theirs, "docs/private-notes.md", "# Now public\n")
        policy = json.loads((theirs / admit_paths.POLICY).read_text(encoding="utf-8"))
        policy["exclude_paths"] = []
        policy["include_paths"] = sorted(
            (set(policy["include_paths"]) | {"docs/private-notes.md"}) - {"docs/guide.md"}
        )
        write(theirs, admit_paths.POLICY, admit_paths._canonical_profile_bytes(policy).decode("utf-8"))
        git(theirs, "rm", "-q", "docs/guide.md")
        git(theirs, "add", "docs/private-notes.md", admit_paths.POLICY)
        git(theirs, "commit", "-qm", "publish the notes and drop the guide")
        merge = subprocess.run(
            ["git", "merge", "--no-commit", "--no-ff", git(theirs, "rev-parse", "HEAD")],
            cwd=ours, env={**os.environ, **GIT_ENV}, capture_output=True, check=False,
        )
        self.assertIn(merge.returncode, (0, 1))
        admit_paths.merge_profile(ours)
        merged = json.loads((ours / admit_paths.POLICY).read_text(encoding="utf-8"))
        self.assertEqual(merged["exclude_paths"], [])
        self.assertIn("docs/private-notes.md", merged["include_paths"])
        self.assertIn("docs/ours.md", merged["include_paths"])
        self.assertNotIn("docs/guide.md", merged["include_paths"])
        git(ours, "merge", "--abort")

    def test_v3_delta_verifies_and_legacy_nested_and_flat_deltas_remain_readable(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        result = fixture.admit("docs/new-page.md")
        baseline_policy = (git(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}") + "\n").encode()
        candidate_policy, _ = admit_paths._candidate_policy(baseline_policy, {"docs/new-page.md"})
        verified = assert_v3_delta(self, fixture, result["policy_delta_record"], baseline=baseline_policy,
                                   candidate=candidate_policy, files={"docs/new-page.md": b"# New page\n"})
        self.assertEqual(verified["include_added"], ["docs/new-page.md"])

        # Admissions from before #1673 left V1/V2 records in both layouts; the gate still reads them.
        v1 = admit_paths._canonical({
            "allowed_delta": {"exclude_added": [], "exclude_removed": [], "include_added": ["docs/new-page.md"],
                              "include_removed": [], "rule_changes": []},
            "baseline_policy_sha256": hashlib.sha256(baseline_policy).hexdigest(),
            "candidate_policy_sha256": hashlib.sha256(candidate_policy).hexdigest(),
            "kind": provenance_ledger.POLICY_DELTA_KIND,
            "schema_version": provenance_ledger.POLICY_DELTA_SCHEMA_VERSION,
        })
        candidate_sha = hashlib.sha256(candidate_policy).hexdigest()
        nested = fixture.authority / admit_paths.POLICY_DELTAS / candidate_sha / hashlib.sha256(baseline_policy).hexdigest()
        flat = fixture.authority / admit_paths.POLICY_DELTAS / candidate_sha
        for directory in (nested, flat):
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "candidate-policy.json").write_bytes(candidate_policy)
            (directory / "policy-delta-authority.json").write_bytes(v1)
            with self.subTest(layout=directory.relative_to(fixture.authority).as_posix()):
                self.assertEqual(admit_paths.provenance_ledger._read_policy_delta_authority(
                    directory / "policy-delta-authority.json", candidate_root=fixture.pr,
                    baseline_policy_bytes=baseline_policy, candidate_policy_bytes=candidate_policy,
                ), verified)


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
        """Every entry's V3 delta approves its own paths on every base the other entries can produce."""
        base_policy = (git(fixture.pr, "show", f"{result['base']}:{admit_paths.POLICY}") + "\n").encode("utf-8")
        path_sets = [set(entry["paths"]) for entry in result["entries"]]
        for mask in range(1 << len(path_sets)):
            selected = [i for i in range(len(path_sets)) if mask & (1 << i)]
            subset_paths = set().union(*(path_sets[i] for i in selected)) if selected else set()
            baseline_policy = base_policy if not selected else admit_paths._candidate_policy(base_policy, subset_paths)[0]
            for candidate_index, paths in enumerate(path_sets):
                if mask & (1 << candidate_index):
                    continue
                candidate_policy = admit_paths._candidate_policy(base_policy, subset_paths | paths)[0]
                record = result["entries"][candidate_index]["policy_delta_record"]
                with self.subTest(subset=selected, candidate=candidate_index):
                    files = {path: (Path(result["entries"][candidate_index]["pr_worktree"]) / path).read_bytes()
                             for path in paths}
                    authority = assert_v3_delta(self, fixture, record, baseline=baseline_policy,
                                                candidate=candidate_policy, files=files)
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

    def test_two_pr_batch_covers_every_transition_in_one_private_commit(self) -> None:
        fixture = _Fixture(self)
        batch, prs = self._entries(fixture, 2)
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        result = admit_paths.admit_batch(
            batch=batch, private_repo=fixture.authority, base=fixture.base,
            date="2026-10-22", scratch_root=fixture.tmp,
        )
        self.assertEqual(result["base"], fixture.base)
        self.assertEqual(result["delta_count"], 2)
        self._assert_every_transition(fixture, result)
        self._assert_one_private_commit(fixture, result, private_before)
        self.assertEqual(len({entry["records"][0] for entry in result["entries"]}), 2)
        for entry, (root, _branch, path) in zip(result["entries"], prs, strict=True):
            self.assertEqual(entry["base"], fixture.base)
            self.assertEqual(entry["paths"], [path])
            self.assertEqual(git(root, "merge-base", entry["admit_branch"], fixture.base), fixture.base)

    def test_three_pr_batch_covers_every_transition_in_one_private_commit(self) -> None:
        fixture = _Fixture(self)
        batch, _prs = self._entries(fixture, 3)
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        result = admit_paths.admit_batch(
            batch=batch, private_repo=fixture.authority, base=fixture.base,
            date="2026-10-22", scratch_root=fixture.tmp,
        )
        self.assertEqual(result["delta_count"], 3)
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
            ])
        self.assertEqual(code, 0)
        self.assertIn("admitted 2 pull requests on shared base", out.getvalue())
        self.assertIn("wrote 2 V3 policy deltas, valid in any landing order", out.getvalue())

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

        record = result["entries"][1 - first]["policy_delta_record"]
        verified = assert_v3_delta(self, fixture, record, baseline=baseline_policy, candidate=profile,
                                   files={waiting_path: (waiting_root / waiting_path).read_bytes()})
        self.assertEqual(verified["include_added"], [waiting_path])
        self.assertEqual(verified["include_removed"], [])
        self.assertEqual(verified["rule_changes"], [])

    def test_both_two_pr_landing_orders_merge_and_verify_without_a_rekey(self) -> None:
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


class PolicyDeltaIssueTest(unittest.TestCase):
    """``--policy-delta``: deltas that add no path, including V2 rule changes."""

    BASIS = "Owner decision: synthetic profile cleanup."

    def _commit_profile(self, fixture: _Fixture, change) -> tuple[bytes, bytes]:
        baseline = (fixture.pr / admit_paths.POLICY).read_bytes()
        policy = json.loads(baseline.decode("utf-8"))
        change(policy)
        candidate = admit_paths._canonical_profile_bytes(policy)
        (fixture.pr / admit_paths.POLICY).write_bytes(candidate)
        git(fixture.pr, "add", admit_paths.POLICY)
        git(fixture.pr, "commit", "-qm", "profile change")
        return baseline, candidate

    def _issue(self, fixture: _Fixture, **kwargs) -> dict:
        kwargs.setdefault("approval_basis", self.BASIS)
        return admit_paths.issue_policy_delta(pr_worktree=fixture.pr, private_repo=fixture.authority, **kwargs)

    def _authority(self, fixture: _Fixture, result: dict) -> tuple[Path, dict]:
        (record,) = result["records"]
        self.assertTrue(record.startswith(f"{admit_paths.POLICY_DELTAS_V3}/"))
        path = fixture.authority / record
        self.assertEqual(path.stem, hashlib.sha256(path.read_bytes()).hexdigest())
        return path, json.loads(path.read_text(encoding="utf-8"))

    def test_rule_change_issues_a_v3_delta_the_gate_reads(self) -> None:
        fixture = _Fixture(self)

        def change(policy: dict) -> None:
            policy["include_paths"].remove("docs/guide.md")
            policy["build_mode"] = "PUBLIC_SAFE=2"

        baseline, candidate = self._commit_profile(fixture, change)
        result = self._issue(fixture, push=True)

        self.assertEqual(result["schema_version"], 3)
        self.assertEqual(result["base"], fixture.base)
        self.assertEqual(fixture.remote_main(), result["private_commit"])
        path, authority = self._authority(fixture, result)
        self.assertEqual(authority["include_removed"], ["docs/guide.md"])
        self.assertEqual(authority["rule_changes"], [{
            "key": "build_mode",
            "baseline_sha256": hashlib.sha256(b'"PUBLIC_SAFE=1"').hexdigest(),
            "candidate_sha256": hashlib.sha256(b'"PUBLIC_SAFE=2"').hexdigest(),
        }])
        self.assertIn(self.BASIS, git(fixture.authority, "log", "-1", "--format=%B"))
        allowed = assert_v3_delta(self, fixture, path.relative_to(fixture.authority).as_posix(),
                                  baseline=baseline, candidate=candidate, files={})
        self.assertEqual(allowed, provenance_ledger._classify_policy_delta(
            json.loads(baseline), json.loads(candidate)))

    def test_a_removed_rule_entry_binds_the_whole_field(self) -> None:
        fixture = _Fixture(self)

        def seed(policy: dict) -> None:
            policy["exclude_rationale"] = {"docs/private-notes.md": "Private.", "docs/gone.md": "Orphan."}

        self._commit_profile(fixture, seed)
        git(fixture.pr, "update-ref", "refs/remotes/origin/main", "HEAD")
        _, candidate = self._commit_profile(fixture, lambda policy: policy["exclude_rationale"].pop("docs/gone.md"))
        result = self._issue(fixture)

        _, authority = self._authority(fixture, result)
        self.assertEqual(authority["schema_version"], 3)
        self.assertEqual(authority["rule_changes"][0]["candidate_sha256"],
                         hashlib.sha256(json.dumps({"docs/private-notes.md": "Private."}).encode()).hexdigest())
        self.assertFalse(result["pushed"])
        self.assertNotEqual(fixture.remote_main(), result["private_commit"], "nothing is pushed without --push")

    def test_path_only_removal_is_a_v3_delta_with_no_rule_change(self) -> None:
        fixture = _Fixture(self)
        self._commit_profile(fixture, lambda policy: policy["exclude_paths"].remove("docs/private-notes.md"))
        result = self._issue(fixture)
        _, authority = self._authority(fixture, result)
        self.assertEqual(authority["schema_version"], 3)
        self.assertEqual(authority["exclude_removed"], ["docs/private-notes.md"])
        self.assertEqual(authority["rule_changes"], [])

    def test_refusals(self) -> None:
        fixture = _Fixture(self)
        with self.assertRaises(AdmitError) as caught:
            self._issue(fixture)
        self.assertEqual(caught.exception.code, "POLICY_DELTA_EMPTY")
        with self.assertRaises(AdmitError) as caught:
            self._issue(fixture, approval_basis=" ")
        self.assertEqual(caught.exception.code, "APPROVAL_BASIS_REQUIRED")
        fixture.add_pr_path(fixture.pr, "docs/new-page.md", "# New page\n")
        with self.assertRaises(AdmitError) as caught:
            self._issue(fixture)
        self.assertEqual(caught.exception.code, "POLICY_DELTA_ADDS_PATHS")
        self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

    def test_cli_mode(self) -> None:
        fixture = _Fixture(self)
        self._commit_profile(fixture, lambda policy: policy.update(build_mode="PUBLIC_SAFE=2"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = admit_paths.main([
                "--policy-delta", str(fixture.pr), "--private-repo", str(fixture.authority),
                "--approval-basis", self.BASIS,
            ])
        self.assertEqual(code, 0)
        self.assertIn("issued a V3 policy delta", out.getvalue())
        self.assertIn("rule_changes: build_mode", out.getvalue())


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
    #: The historical admitted profile, written before the canonical path-list order existed.
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
        # The private commit that added the admission, and its latest form. Since #1673 the
        # policy delta is the V3 record, not the historical nested candidate/baseline pair.
        added = git(self.authority, "log", "--diff-filter=A", "--format=%H", "--", self.ADMISSION).splitlines()[-1]
        latest = git(self.authority, "log", "-1", "--format=%H", "--", self.ADMISSION)
        expected = {self.ADMISSION: subprocess.run(["git", "show", f"{latest}:{self.ADMISSION}"], cwd=self.authority,
                                                   capture_output=True, check=True).stdout}
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

        self.assertEqual(result["records"], [self.ADMISSION, result["policy_delta_record"]])
        self.assertEqual((private / self.ADMISSION).read_bytes(), expected[self.ADMISSION])
        self.assertEqual(
            verifier.policy_delta_key(repo=pr, base_rev=self.BASE, candidate_rev=result["admission_commit"]),
            Path(result["policy_delta_record"]).stem,
        )
        self.assertEqual(result["statements"], [{"path": ".recomp.json", "classification": "reviewed_configuration"}])
        # The historical profile (sha256 PROFILE_SHA) predates the canonical path-list order; the
        # admission now writes its canonical form, so compare the profile in that form.
        profile = "assets/public_source_profile.json"
        historical_profile = json.loads(git(pr, "show", f"{self.ADMITTED}:{profile}"))
        self.assertEqual(git_bytes(pr, "show", f"{result['admission_commit']}:{profile}"),
                         provenance_ledger.canonical_profile_bytes(historical_profile))
        # The admission commit equals the historical one except for the profile above and its
        # ledger, which is now the claims form of the historical (hashed) ledger.
        ledger = "assets/public_provenance_ledger.json"
        listing = {
            commit: dict(line.split("\t", 1)[::-1] for line in git(pr, "ls-tree", "-r", commit).splitlines())
            for commit in (result["admission_commit"], self.ADMITTED)
        }
        ours, theirs = (listing[commit] for commit in (result["admission_commit"], self.ADMITTED))
        self.assertEqual(set(ours), set(theirs))
        # PUBLIC_EXPORT.json records the profile digest, so it follows the profile.
        self.assertEqual(sorted(path for path in ours if ours[path] != theirs[path]),
                         sorted([ledger, profile, "PUBLIC_EXPORT.json"]))
        historical = json.loads(git(pr, "show", f"{self.ADMITTED}:{ledger}"))
        self.assertEqual(git_bytes(pr, "show", f"{result['admission_commit']}:{ledger}"),
                         provenance_ledger.claims_ledger_bytes(historical))


if __name__ == "__main__":
    unittest.main()
