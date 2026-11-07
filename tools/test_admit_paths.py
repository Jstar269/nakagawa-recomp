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
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from typing import Any

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


_REAL_HOOK_CHECK = admit_paths.check_rewriting_hooks


def setUpModule() -> None:
    # Every admission first applies the byte-rewriting hooks' rules; HookCheckTest covers that
    # step, so the synthetic content elsewhere need not be shaped for it.
    patcher = mock.patch.object(admit_paths, "check_rewriting_hooks", return_value=[])
    patcher.start()
    unittest.addModuleCleanup(patcher.stop)


def authority_claims(fixture: "_Fixture"):
    """``claim_of`` over the fixture authority's committed ledger, as the attestation derives it."""
    records = provenance_ledger._detailed_records(json.loads(
        (fixture.authority / admit_paths.TRUSTED_LEDGER).read_text(encoding="utf-8")))
    return lambda path: provenance_ledger.admission_claim(path, records.get(path))


def assert_policy_delta(
    testcase: unittest.TestCase, fixture: "_Fixture", record: str, *,
    baseline: bytes, candidate: bytes, files: dict[str, bytes],
) -> dict:
    """The stored V4 delta is named after its selector and approves exactly ``baseline`` -> ``candidate``.

    ``files`` are the candidate's new files; the delta binds each one's origin claim.
    """
    testcase.assertTrue(record.startswith(f"{admit_paths.POLICY_DELTAS_V4}/"), record)
    raw = (fixture.authority / record).read_bytes()
    testcase.assertEqual(Path(record).stem, provenance_ledger.policy_delta_v4_key(
        json.loads(baseline.decode("utf-8")), json.loads(candidate.decode("utf-8"))))
    with tempfile.TemporaryDirectory() as scratch:
        policy_file = Path(scratch) / "candidate-policy.json"
        policy_file.write_bytes(candidate)
        candidate_policy = admit_paths.load_policy(policy_file)
    return provenance_ledger.verify_policy_delta_v4(
        raw, baseline_policy_bytes=baseline, candidate_policy_bytes=candidate,
        base_blobs={}, candidate_blobs=files, candidate_policy=candidate_policy,
        claim_of=authority_claims(fixture),
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
        returned: dict[str, Any] = json.loads((self.authority / relative).read_text(encoding="utf-8"))
        return returned

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
        # One V4 delta and no nested V1/V2 record (#1673): it binds the new file's origin claim.
        record = result["records"][1]
        self.assertEqual(result["policy_delta_record"], record)
        self.assertEqual(len(result["records"]), 2)
        self.assertEqual(fixture.record(record), {
            "exclude_added": [], "exclude_removed": [],
            "include_added": [{"path": "docs/new-page.md", "binding": "claim", "claim": {
                "classification": "reviewed_documentation", "record_id": None, "record_classification": None,
                "upstream": None, "upstream_revision": None, "upstream_license": None,
            }}],
            "include_removed": [],
            "kind": "policy-delta-authority", "rule_changes": [], "schema_version": 4,
        })
        allowed = assert_policy_delta(self, fixture, record, baseline=base_policy.encode(), candidate=candidate_policy,
                                  files={"docs/new-page.md": b"# New page\n"})
        self.assertEqual(allowed["include_added"], ["docs/new-page.md"])
        # The name the attestation derives from the admission commit is the stored file's name.
        self.assertEqual(
            verifier.policy_delta_key_v4(repo=fixture.pr, base_rev=fixture.base,
                                         candidate_rev=result["admission_commit"]),
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
        self.assertEqual(git(fixture.pr, "branch", "--list", "admit/*"), "", "a refused admission leaves no branch")

    def test_rekey_after_another_admission_lands(self) -> None:
        """A lands first. B's V4 delta still holds on the new main; a pre-V4 admission re-keys once."""
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

        # B's V4 delta from before A landed already approves B's change on the new main.
        main_policy = (git(fixture.pr, "show", f"{new_main}:assets/public_source_profile.json") + "\n").encode()
        union_policy, _ = admit_paths._candidate_policy(main_policy, {"docs/b.md"})
        assert_policy_delta(self, fixture, first["policy_delta_record"], baseline=main_policy, candidate=union_policy,
                        files={"docs/b.md": b"# B\n"})

        result = fixture.admit("docs/b.md", rekey=True, approval_basis=None)

        self.assertEqual(result["base"], new_main)
        self.assertEqual(result["records"], first["records"], "the same decision and the same V4 delta")
        self.assertEqual(admission.read_bytes(), recorded)
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), private_before, "nothing new to record")
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).iterdir())), 2)
        self.assertNotEqual(result["candidate_policy_sha256"], first["candidate_policy_sha256"])
        self.assertEqual(result["admit_branch"], f"admit/new-pages-2026-10-22-{new_main[:8]}")
        git(fixture.pr, "merge", "-q", "--no-edit", result["admit_branch"])

        # An admission recorded before V4 deltas existed has no V4 file: the re-key writes it, once.
        git(fixture.authority, "rm", "-q", first["policy_delta_record"])
        git(fixture.authority, "commit", "-qm", "an admission from before V4 deltas")
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

        # A re-key never rewrites a V4 decision already recorded under the same name.
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/b.md", rekey=True, approval_basis=None)
        self.assertEqual(caught.exception.code, "ADMISSION_RECORD_CONFLICT")
        # As an admission from before V4 (no V4 file), it re-keys from the recorded decision.
        git(fixture.authority, "rm", "-q", first["policy_delta_record"])
        git(fixture.authority, "commit", "-qm", "an admission from before V4 deltas")
        result = fixture.admit("docs/b.md", rekey=True, approval_basis=None)

        self.assertEqual(result["records"][0], first["records"][0], "the recorded decision is reused")
        self.assertEqual(json.loads(admission.read_text(encoding="utf-8")), document, "and left as recorded")
        self.assertEqual(result["base"], new_main)
        # The recorded third_party origin binds the import to its bytes; the claim names the record.
        (entry,) = fixture.record(result["policy_delta_record"])["include_added"]
        self.assertEqual((entry["binding"], entry["sha256"]), ("bytes", hashlib.sha256(b"# B\n").hexdigest()))
        self.assertEqual(entry["claim"]["record_id"], "b-upstream-import")

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
        """The entry's V4 delta approves exactly its own path, on the given base."""
        allowed = assert_policy_delta(self, fixture, record, baseline=baseline, candidate=candidate,
                                  files={path: f"# {Path(path).stem}\n".encode()})
        self.assertEqual(allowed, {"exclude_added": [], "exclude_removed": [], "include_added": [path],
                                   "include_removed": [], "rule_changes": []})

    def test_two_and_three_entry_batches_write_one_v4_delta_each_valid_in_any_order(self) -> None:
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
                self.assertEqual(len(result["records"]), 2 * count, "one admission and one V4 delta per entry")
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
                # The second PR's own V4 delta, issued against the old base, approves the merge.
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
        self.assertIn("re-issued V4 deltas for 4 pull requests", output.getvalue())
        self.assertEqual(len(list((fixture.authority / admit_paths.ADMISSIONS).glob("*.json"))), 7)
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), private_before,
                         "the V4 deltas re-derived on the new base are the ones already recorded")

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

    def test_v4_delta_verifies_and_legacy_nested_and_flat_deltas_remain_readable(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        result = fixture.admit("docs/new-page.md")
        baseline_policy = (git(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}") + "\n").encode()
        candidate_policy, _ = admit_paths._candidate_policy(baseline_policy, {"docs/new-page.md"})
        verified = assert_policy_delta(self, fixture, result["policy_delta_record"], baseline=baseline_policy,
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
        """Every entry's V4 delta approves its own paths on every base the other entries can produce."""
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
                    authority = assert_policy_delta(self, fixture, record, baseline=baseline_policy,
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
        self.assertIn("wrote 2 V4 policy deltas, valid in any landing order", out.getvalue())

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
        verified = assert_policy_delta(self, fixture, record, baseline=baseline_policy, candidate=profile,
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
        returned: dict[str, Any] = admit_paths.issue_policy_delta(pr_worktree=fixture.pr, private_repo=fixture.authority, **kwargs)
        return returned

    def _authority(self, fixture: _Fixture, result: dict) -> tuple[Path, dict]:
        (record,) = result["records"]
        self.assertTrue(record.startswith(f"{admit_paths.POLICY_DELTAS_V4}/"))
        path = fixture.authority / record
        document = json.loads(path.read_text(encoding="utf-8"))
        selector = {**document, "include_added": [item["path"] for item in document["include_added"]]}
        self.assertEqual(path.stem, hashlib.sha256(provenance_ledger.policy_delta_v3_bytes(selector)).hexdigest(),
                         "a V4 delta is named after its selector")
        return path, document

    def test_rule_change_issues_a_v4_delta_the_gate_reads(self) -> None:
        fixture = _Fixture(self)

        def change(policy: dict) -> None:
            policy["include_paths"].remove("docs/guide.md")
            policy["build_mode"] = "PUBLIC_SAFE=2"

        baseline, candidate = self._commit_profile(fixture, change)
        result = self._issue(fixture, push=True)

        self.assertEqual(result["schema_version"], 4)
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
        allowed = assert_policy_delta(self, fixture, path.relative_to(fixture.authority).as_posix(),
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
        self.assertEqual(authority["schema_version"], 4)
        self.assertEqual(authority["rule_changes"][0]["candidate_sha256"],
                         hashlib.sha256(json.dumps({"docs/private-notes.md": "Private."}).encode()).hexdigest())
        self.assertFalse(result["pushed"])
        self.assertNotEqual(fixture.remote_main(), result["private_commit"], "nothing is pushed without --push")

    def test_path_only_removal_is_a_v4_delta_with_no_rule_change(self) -> None:
        fixture = _Fixture(self)
        self._commit_profile(fixture, lambda policy: policy["exclude_paths"].remove("docs/private-notes.md"))
        result = self._issue(fixture)
        _, authority = self._authority(fixture, result)
        self.assertEqual(authority["schema_version"], 4)
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
        self.assertIn("issued a V4 policy delta", out.getvalue())
        self.assertIn("rule_changes: build_mode", out.getvalue())


class ClaimBindingAdmissionTest(unittest.TestCase):
    """V4 admissions bind each new path to its origin claim, and to its bytes only for a byte-identical import."""

    THIRD_PARTY = {"origin_kind": "third_party", "origin": "Synthetic upstream header, byte-identical.",
                   "license": "MIT"}
    RECORD = {"id": "PROV-UPSTREAM", "classification": "upstream-third-party", "evidence_tier": "H",
              "paths": ["src/lib/up.h"], "upstream": "synthetic-upstream",
              "upstream_revision": "commit 0123456789abcdef0123456789abcdef01234567", "upstream_license": "MIT"}

    def _entry(self, fixture: _Fixture, result: dict, path: str) -> dict:
        return next(item for item in fixture.record(result["policy_delta_record"])["include_added"]
                    if item["path"] == path)

    def _forget_v4(self, fixture: _Fixture, result: dict) -> None:
        """Make ``result`` look like an admission recorded before V4 deltas existed."""
        git(fixture.authority, "rm", "-q", result["policy_delta_record"])
        git(fixture.authority, "commit", "-qm", "an admission from before V4 deltas")
        git(fixture.pr, "branch", "-q", "-D", result["admit_branch"])

    def test_bindings_default_by_origin_and_are_recorded_only_when_chosen(self) -> None:
        fixture = _Fixture(self, records=[self.RECORD])
        fixture.add("src/lib/up.h", "int up(void);\n")
        fixture.add("docs/notice.md", "# Notice\n")
        result = fixture.admit("src/lib/up.h", "docs/notice.md", origins={"src/lib/up.h": self.THIRD_PARTY})
        statements = {s["path"]: s for s in fixture.record(result["records"][0])["reviewed_new_paths"]}
        self.assertNotIn("binding", statements["src/lib/up.h"], "the default for a third_party origin")
        self.assertNotIn("binding", statements["docs/notice.md"])
        up, notice = self._entry(fixture, result, "src/lib/up.h"), self._entry(fixture, result, "docs/notice.md")
        self.assertEqual((up["binding"], up["sha256"]), ("bytes", hashlib.sha256(b"int up(void);\n").hexdigest()))
        self.assertEqual(up["claim"], {
            "classification": "upstream_derived", "record_id": "PROV-UPSTREAM",
            "record_classification": "upstream-third-party", "upstream": "synthetic-upstream",
            "upstream_revision": self.RECORD["upstream_revision"], "upstream_license": "MIT",
        })
        self.assertEqual(notice["binding"], "claim")
        self.assertNotIn("sha256", notice)

        # A reviewer's LOCAL DELTA (claim) or a byte-pinned document (bytes) is recorded explicitly.
        git(fixture.pr, "branch", "-q", "-D", result["admit_branch"])
        chosen = fixture.admit(
            "src/lib/up.h", "docs/notice.md", slug="new-pages-v2",
            origins={"src/lib/up.h": {**self.THIRD_PARTY, "binding": "claim"}, "docs/notice.md": {"binding": "bytes"}},
        )
        statements = {s["path"]: s for s in fixture.record(chosen["records"][0])["reviewed_new_paths"]}
        self.assertEqual((statements["src/lib/up.h"]["binding"], statements["docs/notice.md"]["binding"]),
                         ("claim", "bytes"))
        # The same profile change selects the same V4 name; a fresh admission records its new decision there.
        self.assertEqual(chosen["policy_delta_record"], result["policy_delta_record"])
        self.assertEqual(self._entry(fixture, chosen, "src/lib/up.h")["binding"], "claim")
        self.assertEqual(self._entry(fixture, chosen, "docs/notice.md")["binding"], "bytes")
        self.assertIn(chosen["policy_delta_record"],
                      git(fixture.authority, "show", "--name-only", "--format=", chosen["private_commit"]))

        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/notice.md", slug="bad", origins={"docs/notice.md": {"binding": "trust-me"}})
        self.assertEqual(caught.exception.code, "ADMISSION_BINDING_INVALID")

    def test_cli_binding_flag(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page\n")
        out = io.StringIO()
        with mock.patch.object(admit_paths.tempfile, "tempdir", str(fixture.tmp)), contextlib.redirect_stdout(out):
            code = admit_paths.main([
                str(fixture.pr), "docs/new-page.md", "--approval-basis", "Owner request.", "--binding", "bytes",
                "--private-repo", str(fixture.authority), "--date", "2026-10-22",
            ])
        self.assertEqual(code, 0)
        self.assertIn("docs/new-page.md: reviewed_documentation (bound to its bytes)", out.getvalue())
        self.assertIn("V4 policy delta docs/provenance/policy-deltas-v4/", out.getvalue())

    def test_rekey_of_an_edited_claim_bound_path_reuses_its_admission(self) -> None:
        """Tonight's case: admitted, then a hook trimmed a blank line. One re-key, no new decision."""
        fixture = _Fixture(self)
        fixture.add("docs/b.md", "# B\n\n\n")
        first = fixture.admit("docs/b.md")
        recorded = (fixture.authority / first["records"][0]).read_bytes()
        self._forget_v4(fixture, first)
        fixture.add("docs/b.md", "# B\n")

        rekeyed = fixture.admit("docs/b.md", rekey=True, approval_basis=None)
        self.assertEqual(rekeyed["records"][0], first["records"][0])
        self.assertEqual((fixture.authority / first["records"][0]).read_bytes(), recorded, "no new decision")
        self.assertEqual(self._entry(fixture, rekeyed, "docs/b.md")["binding"], "claim")

    def test_rekey_of_an_edited_byte_bound_path_is_refused(self) -> None:
        fixture = _Fixture(self, records=[self.RECORD])
        fixture.add("src/lib/up.h", "int up(void);\n")
        first = fixture.admit("src/lib/up.h", origins={"src/lib/up.h": self.THIRD_PARTY})
        self._forget_v4(fixture, first)
        fixture.add("src/lib/up.h", "int up(void);\n\n")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("src/lib/up.h", rekey=True, approval_basis=None)
        self.assertEqual(caught.exception.code, "REKEY_ADMISSION_MISSING")

    def test_rekey_refuses_admissions_that_record_different_decisions(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/b.md", "# B\n")
        first = fixture.admit("docs/b.md")
        git(fixture.pr, "branch", "-q", "-D", first["admit_branch"])
        second = fixture.admit("docs/b.md", slug="new-pages-v2", origins={"docs/b.md": {"binding": "bytes"}})
        self._forget_v4(fixture, second)
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/b.md", rekey=True, approval_basis=None)
        self.assertEqual(caught.exception.code, "REKEY_ADMISSION_AMBIGUOUS")


class ComposeV4Test(unittest.TestCase):
    """``--compose-v4``: one landing batch carries several admitted profile changes under one V4 name."""

    RECORD = ClaimBindingAdmissionTest.RECORD
    THIRD_PARTY = ClaimBindingAdmissionTest.THIRD_PARTY

    def setUp(self) -> None:
        self._worktrees = 0

    @staticmethod
    def _baseline(fixture: _Fixture) -> bytes:
        return git_bytes(fixture.pr, "show", f"{fixture.base}:{admit_paths.POLICY}")

    def _component(self, fixture: _Fixture, change, added: dict[str, tuple[str, str]] | None = None,
                   *, baseline: bytes | None = None) -> str:
        """Commit the V4 delta of ``change`` on the base profile, as an admission writes it; return its name.

        ``added`` maps each added path to ``(content, binding)``.
        """
        baseline = baseline or self._baseline(fixture)
        document = json.loads(baseline.decode("utf-8"))
        change(document)
        candidate = admit_paths._canonical_profile_bytes(document)
        claims = authority_claims(fixture)
        record, authority, _delta = admit_paths.policy_delta_authority(baseline, candidate, {
            path: {"sha256": hashlib.sha256(content.encode()).hexdigest(), "binding": binding, "claim": claims(path)}
            for path, (content, binding) in (added or {}).items()
        })
        target = fixture.authority / record
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(authority)
        git(fixture.authority, "add", record)
        git(fixture.authority, "commit", "-qm", f"component {Path(record).stem[:8]}")
        return Path(record).stem

    def _candidate(self, fixture: _Fixture, changes: list, files: dict[str, str]) -> tuple[Path, bytes]:
        """A batch worktree on the base whose HEAD carries ``files`` and every profile change."""
        self._worktrees += 1
        worktree = fixture.worktree(f"compose-batch-{self._worktrees}")
        for path, content in files.items():
            write(worktree, path, content)
        document = json.loads(self._baseline(fixture).decode("utf-8"))
        for change in changes:
            change(document)
        profile = admit_paths._canonical_profile_bytes(document)
        (worktree / admit_paths.POLICY).write_bytes(profile)
        git(worktree, "add", "-A")
        git(worktree, "commit", "-qm", "synthetic landing batch")
        return worktree, profile

    @staticmethod
    def _adds(*paths: str):
        def change(document: dict) -> None:
            document["include_paths"] = sorted(set(document["include_paths"]) | set(paths))
        return change

    def _compose(self, fixture: _Fixture, worktree: Path, keys: list[str], **kwargs) -> dict:
        returned: dict[str, Any] = admit_paths.compose_policy_delta_v4(
            worktree=worktree, base=fixture.base, components=keys, private_repo=fixture.authority, **kwargs)
        return returned

    def _assert_derived(self, fixture: _Fixture, result: dict, *, candidate: bytes, files: dict[str, bytes],
                        bindings: dict[str, str]) -> None:
        """The composed delta is the derivation, byte for byte, under the batch's name, and the verifier passes it."""
        baseline = self._baseline(fixture)
        baseline_document = json.loads(baseline.decode("utf-8"))
        candidate_document = json.loads(candidate.decode("utf-8"))
        record = f"{admit_paths.POLICY_DELTAS_V4}/{result['key']}.json"
        self.assertEqual(result["records"], [record])
        self.assertEqual(result["key"], provenance_ledger.policy_delta_v4_key(baseline_document, candidate_document))
        derived = provenance_ledger.derive_policy_delta_v4(
            baseline_document, candidate_document,
            digest_of=lambda path: hashlib.sha256(files[path]).hexdigest() if path in files else None,
            claim_of=authority_claims(fixture), binding_of=bindings.__getitem__,
        )
        self.assertEqual((fixture.authority / record).read_bytes(), provenance_ledger.policy_delta_v3_bytes(derived))
        verified = assert_policy_delta(self, fixture, record, baseline=baseline, candidate=candidate, files=files)
        self.assertEqual(verified, provenance_ledger._classify_policy_delta(baseline_document, candidate_document))
        # One private commit holding only the composed delta, naming every component; nothing pushed.
        self.assertEqual(git(fixture.authority, "show", "--name-only", "--format=", "HEAD").splitlines(), [record])
        message = git(fixture.authority, "log", "-1", "--format=%B")
        for key in result["components"]:
            self.assertIn(key, message)
        self.assertIn("no new decision", message)
        self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")
        self.assertFalse(result["pushed"])

    def test_two_admitted_pull_requests_compose_into_the_batch_delta(self) -> None:
        fixture = _Fixture(self)
        paths = ["docs/compose-a.md", "docs/compose-b.md"]
        entries = []
        for path in paths:
            slug = Path(path).stem
            pr = fixture.worktree(slug)
            fixture.add_pr_path(pr, path, f"# {slug}\n")
            entries.append({"pr_worktree": str(pr), "paths": [path], "slug": slug,
                            "approval_basis": f"Owner decision: {slug}."})
        batch = admit_paths.admit_batch(batch=entries, private_repo=fixture.authority, base=fixture.base,
                                        date="2026-10-22", scratch_root=fixture.tmp)
        keys = [Path(delta["record"]).stem for delta in batch["deltas"]]
        admissions = {path: (fixture.authority / path).read_bytes() for path in batch["records"]}
        remote_before = fixture.remote_main()

        files = {path: f"# {Path(path).stem}\n" for path in paths}
        worktree, candidate = self._candidate(fixture, [self._adds(*paths)], files)
        # Without the union, the batch's name selects no delta: the gap this mode fills.
        self.assertNotIn(provenance_ledger.policy_delta_v4_key(
            json.loads(self._baseline(fixture)), json.loads(candidate)), keys)
        result = self._compose(fixture, worktree, keys)

        self._assert_derived(fixture, result, candidate=candidate,
                             files={path: content.encode() for path, content in files.items()},
                             bindings=dict.fromkeys(paths, "claim"))
        self.assertEqual(result["delta"]["include_added"], paths)
        self.assertEqual(fixture.remote_main(), remote_before)
        for path, raw in admissions.items():
            self.assertEqual((fixture.authority / path).read_bytes(), raw, "the components are left as recorded")

        # Composing again records nothing new; --push publishes private main.
        again = self._compose(fixture, worktree, keys, push=True)
        self.assertEqual(again["private_commit"], result["private_commit"])
        self.assertEqual(fixture.remote_main(), result["private_commit"])

    def test_three_components_with_removals_exclusions_rules_and_a_byte_bound_import(self) -> None:
        fixture = _Fixture(self, records=[self.RECORD])

        def removes_guide(document: dict) -> None:
            document["include_paths"].remove("docs/guide.md")

        def rule_and_exclusion(document: dict) -> None:
            document["build_mode"] = "PUBLIC_SAFE=2"
            document["exclude_paths"].remove("docs/private-notes.md")

        added = {"src/lib/up.h": ("int up(void);\n", "bytes"), "docs/notice.md": ("# Notice\n", "claim")}
        keys = [
            self._component(fixture, self._adds(*added), added),
            self._component(fixture, removes_guide),
            self._component(fixture, rule_and_exclusion),
        ]
        worktree, candidate = self._candidate(
            fixture, [self._adds(*added), removes_guide, rule_and_exclusion],
            {path: content for path, (content, _binding) in added.items()})
        result = self._compose(fixture, worktree, keys, assisted_by="Test Tool (test-model)")

        self._assert_derived(fixture, result, candidate=candidate,
                             files={path: content.encode() for path, (content, _binding) in added.items()},
                             bindings={path: binding for path, (_content, binding) in added.items()})
        self.assertEqual(result["delta"], {
            "include_added": ["docs/notice.md", "src/lib/up.h"], "include_removed": ["docs/guide.md"],
            "exclude_added": [], "exclude_removed": ["docs/private-notes.md"], "rule_changes": ["build_mode"],
        })
        self.assertIn("Assisted-by: Test Tool (test-model)", git(fixture.authority, "log", "-1", "--format=%B"))

    def _assert_refused(self, fixture: _Fixture, code: str, worktree: Path, keys: list[str]) -> None:
        before = git(fixture.authority, "rev-parse", "HEAD")
        with self.assertRaises(AdmitError) as caught:
            self._compose(fixture, worktree, keys)
        self.assertEqual(caught.exception.code, code, str(caught.exception))
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), before)
        self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

    def test_refusals(self) -> None:
        fixture = _Fixture(self, records=[self.RECORD])
        doc = {path: (f"# {Path(path).stem}\n", "claim") for path in
               ("docs/a.md", "docs/b.md", "docs/c.md", "docs/shared.md")}

        def pick(*paths: str) -> dict:
            return {path: doc[path] for path in paths}

        a = self._component(fixture, self._adds("docs/a.md"), pick("docs/a.md"))
        b = self._component(fixture, self._adds("docs/b.md"), pick("docs/b.md"))
        both, _ = self._candidate(fixture, [self._adds("docs/a.md", "docs/b.md")],
                                  {"docs/a.md": doc["docs/a.md"][0], "docs/b.md": doc["docs/b.md"][0]})

        with self.subTest("one component"):
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENTS_INVALID", both, [a])
        with self.subTest("a component twice"):
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENTS_INVALID", both, [a, a])
        with self.subTest("unknown component"):
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENT_MISSING", both, [a, "0" * 64])
        with self.subTest("a component filed under another name"):
            alias = hashlib.sha256(b"alias").hexdigest()
            target = fixture.authority / admit_paths.POLICY_DELTAS_V4 / f"{alias}.json"
            target.write_bytes((fixture.authority / admit_paths.POLICY_DELTAS_V4 / f"{b}.json").read_bytes())
            git(fixture.authority, "add", "-A")
            git(fixture.authority, "commit", "-qm", "misfiled delta")
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENT_KEY_MISMATCH", both, [a, alias])

        with self.subTest("overlapping include_added"):
            shared_a = self._component(fixture, self._adds("docs/a.md", "docs/shared.md"),
                                       pick("docs/a.md", "docs/shared.md"))
            shared_b = self._component(fixture, self._adds("docs/b.md", "docs/shared.md"),
                                       pick("docs/b.md", "docs/shared.md"))
            worktree, _ = self._candidate(
                fixture, [self._adds("docs/a.md", "docs/b.md", "docs/shared.md")],
                {path: doc[path][0] for path in ("docs/a.md", "docs/b.md", "docs/shared.md")})
            self._assert_refused(fixture, "COMPOSE_V4_PATH_OVERLAP", worktree, [shared_a, shared_b])

        def removes(*paths: str):
            def change(document: dict) -> None:
                document["include_paths"] = sorted(set(document["include_paths"]) - set(paths))
            return change

        with self.subTest("overlapping include_removed"):
            first = self._component(fixture, removes("docs/guide.md"))
            second = self._component(fixture, removes("docs/guide.md", "README.md"))
            worktree, _ = self._candidate(fixture, [removes("docs/guide.md", "README.md")], {})
            self._assert_refused(fixture, "COMPOSE_V4_PATH_OVERLAP", worktree, [first, second])

        def unexcludes(document: dict) -> None:
            document["exclude_paths"].remove("docs/private-notes.md")

        def excludes(path: str):
            def change(document: dict) -> None:
                document["exclude_paths"] = sorted(set(document["exclude_paths"]) | {path})
            return change

        with self.subTest("overlapping exclude entries"):
            first = self._component(fixture, unexcludes)
            second = self._component(fixture, lambda document: (unexcludes(document), excludes("docs/x.md")(document)))
            worktree, _ = self._candidate(fixture, [unexcludes, excludes("docs/x.md")], {})
            self._assert_refused(fixture, "COMPOSE_V4_EXCLUDE_OVERLAP", worktree, [first, second])

        def build_mode(document: dict) -> None:
            document["build_mode"] = "PUBLIC_SAFE=2"

        def description(document: dict) -> None:
            document["description"] = "synthetic"

        with self.subTest("rule changes in two components"):
            first = self._component(fixture, build_mode)
            second = self._component(fixture, description)
            worktree, _ = self._candidate(fixture, [build_mode, description], {})
            self._assert_refused(fixture, "COMPOSE_V4_RULE_CHANGES_SHARED", worktree, [first, second])

        with self.subTest("a rule admitted against another base value"):
            other_base = json.loads(self._baseline(fixture))
            other_base["build_mode"] = "PUBLIC_SAFE=0"
            stale = self._component(fixture, build_mode,
                                    baseline=admit_paths._canonical_profile_bytes(other_base))
            worktree, _ = self._candidate(fixture, [self._adds("docs/a.md"), build_mode],
                                          {"docs/a.md": doc["docs/a.md"][0]})
            self._assert_refused(fixture, "COMPOSE_V4_UNION_MISMATCH", worktree, [a, stale])

        with self.subTest("a path no component covers"):
            worktree, _ = self._candidate(fixture, [self._adds("docs/a.md", "docs/b.md", "docs/c.md")],
                                          {path: doc[path][0] for path in ("docs/a.md", "docs/b.md", "docs/c.md")})
            self._assert_refused(fixture, "COMPOSE_V4_PATH_UNCOVERED", worktree, [a, b])

        with self.subTest("a component change the batch does not make"):
            worktree, _ = self._candidate(fixture, [self._adds("docs/a.md")], {"docs/a.md": doc["docs/a.md"][0]})
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENT_NOT_IN_CANDIDATE", worktree, [a, b])

        upstream = {"src/lib/up.h": ("int up(void);\n", "bytes")}
        up = self._component(fixture, self._adds("src/lib/up.h"), upstream)
        with self.subTest("an edited byte-bound import"):
            worktree, _ = self._candidate(fixture, [self._adds("src/lib/up.h", "docs/a.md")],
                                          {"src/lib/up.h": "int up(void);\n\n", "docs/a.md": doc["docs/a.md"][0]})
            self._assert_refused(fixture, "COMPOSE_V4_BYTES_MISMATCH", worktree, [up, a])

        with self.subTest("a trusted claim changed since its admission"):
            worktree, _ = self._candidate(fixture, [self._adds("src/lib/up.h", "docs/a.md")],
                                          {"src/lib/up.h": upstream["src/lib/up.h"][0],
                                           "docs/a.md": doc["docs/a.md"][0]})
            ledger_path = fixture.authority / admit_paths.TRUSTED_LEDGER
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
            for record in ledger["records"]:
                if record["id"] == self.RECORD["id"]:
                    record["upstream_revision"] = "commit " + "f" * 40
            write(fixture.authority, admit_paths.TRUSTED_LEDGER, json.dumps(ledger, indent=2) + "\n")
            git(fixture.authority, "commit", "-qam", "the upstream record now names another revision")
            self._assert_refused(fixture, "COMPOSE_V4_CLAIM_MISMATCH", worktree, [up, a])

    def test_more_refusals(self) -> None:
        fixture = _Fixture(self, records=[self.RECORD])
        a = self._component(fixture, self._adds("docs/a.md"), {"docs/a.md": ("# a\n", "claim")})
        b = self._component(fixture, self._adds("docs/b.md"), {"docs/b.md": ("# b\n", "claim")})
        both, candidate = self._candidate(fixture, [self._adds("docs/a.md", "docs/b.md")],
                                          {"docs/a.md": "# a\n", "docs/b.md": "# b\n"})
        v4_dir = fixture.authority / admit_paths.POLICY_DELTAS_V4

        def commit_misfiled(name: str, raw: bytes) -> str:
            key = hashlib.sha256(name.encode()).hexdigest()
            (v4_dir / f"{key}.json").write_bytes(raw)
            git(fixture.authority, "add", "-A")
            git(fixture.authority, "commit", "-qm", f"misfiled {name}")
            return key

        with self.subTest("a component that does not parse"):
            junk = commit_misfiled("junk", b"not a json document\n")
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENT_INVALID", both, [a, junk])
        with self.subTest("a component that parses but is not canonical"):
            raw = (v4_dir / f"{b}.json").read_bytes()
            loose = commit_misfiled("loose", json.dumps(json.loads(raw), indent=4).encode() + b"\n")
            self._assert_refused(fixture, "COMPOSE_V4_COMPONENT_INVALID", both, [a, loose])
        for name in ("not hex", "A" * 64, "a" * 63):
            with self.subTest(f"a name that is not 64 lowercase hex: {name!r}"):
                self._assert_refused(fixture, "COMPOSE_V4_COMPONENTS_INVALID", both, [a, name])

        with self.subTest("one path in different lists of two components"):
            adds_x = self._component(fixture, self._adds("docs/x.md"), {"docs/x.md": ("# x\n", "claim")})
            excludes_x = self._component(
                fixture, lambda document: document.__setitem__(
                    "exclude_paths", sorted({*document["exclude_paths"], "docs/x.md"})))
            worktree, _ = self._candidate(
                fixture, [self._adds("docs/x.md"), lambda document: document.__setitem__(
                    "exclude_paths", sorted({*document["exclude_paths"], "docs/x.md"}))], {"docs/x.md": "# x\n"})
            self._assert_refused(fixture, "COMPOSE_V4_PATH_OVERLAP", worktree, [adds_x, excludes_x])

        with self.subTest("a base that is not an ancestor of the batch"):
            elsewhere = git(fixture.pr, "commit-tree", f"{fixture.base}^{{tree}}", "-p", fixture.base,
                            "-m", "another line of history")
            before = git(fixture.authority, "rev-parse", "HEAD")
            with self.assertRaises(AdmitError) as caught:
                admit_paths.compose_policy_delta_v4(worktree=both, base=elsewhere, components=[a, b],
                                                    private_repo=fixture.authority)
            self.assertEqual(caught.exception.code, "TRUSTED_BASE_NOT_ANCESTOR")
            self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), before)

        with self.subTest("a batch profile that is not canonical"):
            worktree = fixture.worktree("compose-loose-profile")
            write(worktree, "docs/a.md", "# a\n")
            write(worktree, "docs/b.md", "# b\n")
            document = json.loads(candidate.decode("utf-8"))
            (worktree / admit_paths.POLICY).write_text(json.dumps(document, indent=4) + "\n", encoding="utf-8")
            git(worktree, "add", "-A")
            git(worktree, "commit", "-qm", "a profile in another format")
            self._assert_refused(fixture, "POLICY_PROFILE_NOT_CANONICAL", worktree, [a, b])

        with self.subTest("an authority checkout with an uncommitted record"):
            stray = fixture.authority / "docs/provenance/uncommitted.json"
            stray.write_text("{}\n", encoding="utf-8")
            before = git(fixture.authority, "rev-parse", "HEAD")
            with self.assertRaises(AdmitError) as caught:
                self._compose(fixture, both, [a, b])
            self.assertEqual(caught.exception.code, "AUTHORITY_DIRTY")
            self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), before)
            self.assertTrue(stray.is_file(), "the stray file is left as it was")
            stray.unlink()
            self.assertEqual(git(fixture.authority, "status", "--porcelain"), "")

    def test_cli_mode(self) -> None:
        fixture = _Fixture(self)
        files = {"docs/a.md": "# a\n", "docs/b.md": "# b\n"}
        keys = [self._component(fixture, self._adds(path), {path: (content, "claim")})
                for path, content in files.items()]
        worktree, _ = self._candidate(fixture, [self._adds(*files)], files)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            refused = admit_paths.main(["--compose-v4", str(worktree), "--components", *keys,
                                        "--private-repo", str(fixture.authority)])
            self.assertEqual(refused, 1)
            self.assertIn("--compose-v4 needs --base and --components", err.getvalue())
            refused = admit_paths.main(["--compose-v4", str(worktree), "--base", fixture.base, "--components",
                                        *keys, "--approval-basis", "a new decision",
                                        "--private-repo", str(fixture.authority)])
            self.assertEqual(refused, 1)
            code = admit_paths.main(["--compose-v4", str(worktree), "--base", fixture.base, "--components", *keys,
                                     "--private-repo", str(fixture.authority)])
        self.assertEqual(code, 0, err.getvalue())
        self.assertIn("composed V4 policy delta", out.getvalue())
        self.assertIn("include_added: docs/a.md, docs/b.md", out.getvalue())
        self.assertIn("Left to do:", out.getvalue())


class HookCheckTest(unittest.TestCase):
    """Before reading or pinning a byte, admit_paths applies the byte-rewriting hooks' rules.

    The rules run in pure Python on the blobs at the pull request head, with the trusted base's
    exclusions. Nothing from the pull request is executed (#1889 security review).
    """

    def setUp(self) -> None:
        self.enterContext(mock.patch.object(admit_paths, "check_rewriting_hooks", _REAL_HOOK_CHECK))
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory(prefix="nakagawa-admit-hooks-test-")))

    def _repo(self, base_files: dict[str, bytes], head_files: dict[str, bytes]) -> tuple[Path, str, str]:
        repo = self.tmp / "repo"
        repo.mkdir()
        git(repo, "init", "-q")
        git(repo, "config", "core.autocrlf", "false")
        for files in (base_files, head_files):
            for relative, data in files.items():
                path = repo / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            (repo / "anchor").write_bytes(str(len(files)).encode() + b"\n")
            git(repo, "add", "-A")
            git(repo, "commit", "-qm", "commit")
            if files is base_files:
                base = git(repo, "rev-parse", "HEAD")
        return repo, base, git(repo, "rev-parse", "HEAD")

    def test_refuses_when_the_hooks_would_rewrite_and_admits_after_the_fix_is_committed(self) -> None:
        fixture = _Fixture(self)
        fixture.add("docs/new-page.md", "# New page  \n\n\n")
        private_before = git(fixture.authority, "rev-parse", "HEAD")
        with self.assertRaises(AdmitError) as caught:
            fixture.admit("docs/new-page.md")
        self.assertEqual(caught.exception.code, "ADMIT_HOOKS_WOULD_REWRITE")
        self.assertIn("docs/new-page.md: trailing whitespace on line 1 (trailing-whitespace)", str(caught.exception))
        self.assertIn("more than one line ending", str(caught.exception))
        self.assertEqual(git(fixture.authority, "rev-parse", "HEAD"), private_before, "nothing recorded")
        self.assertEqual(git(fixture.pr, "branch", "--list", "admit/*"), "", "no admission branch")
        self.assertEqual(git(fixture.pr, "status", "--porcelain"), "", "the pull request worktree is untouched")

        digest = fixture.add("docs/new-page.md", "# New page\n")
        result = fixture.admit("docs/new-page.md")
        (statement,) = fixture.record(result["records"][0])["reviewed_new_paths"]
        self.assertEqual(statement["sha256"], digest)

    def test_each_rule_matches_the_hook_it_stands_for(self) -> None:
        findings = admit_paths.rewriting_hook_findings
        self.assertEqual(findings(b""), {})
        self.assertEqual(findings(b"clean\ntext\n"), {})
        self.assertEqual(set(findings(b"a \nb\n")), {"trailing-whitespace"})
        self.assertEqual(set(findings(b"a\nb\t")), {"trailing-whitespace", "end-of-file-fixer"})
        self.assertEqual(set(findings(b"a\nb")), {"end-of-file-fixer"})
        self.assertEqual(set(findings(b"a\n\n")), {"end-of-file-fixer"})
        self.assertEqual(set(findings(b"\n")), {"end-of-file-fixer"})
        self.assertEqual(set(findings(b"a\r\nb\r\n")), {"mixed-line-ending"})
        self.assertEqual(set(findings(b"a\rb\n")), {"mixed-line-ending"})
        self.assertEqual(set(findings(b"\xef\xbb\xbfa\n")), {"fix-byte-order-marker"})
        self.assertEqual(findings(b"\x00\x01binary \r\n\n\n"), {}, "binary files are left alone, as identify types them")

    def test_exclusions_come_from_the_trusted_base_not_the_pull_request(self) -> None:
        base_config = (
            b"exclude: |\n  (?x)^(\n      third_party/|\n      build/\n  )\n"
            b"repos:\n  - repo: https://github.com/pre-commit/pre-commit-hooks\n    rev: v6.0.0\n    hooks:\n"
            b"      - id: trailing-whitespace\n        # a comment\n        exclude: ^docs/legacy\\.md$\n"
            b"      - id: end-of-file-fixer\n        exclude: >-\n          ^docs/table\\.dat$\n"
            b"      - id: check-json\n        exclude: ^docs/legacy2\\.md$\n"
        )
        repo, base, head = self._repo(
            {admit_paths.PRE_COMMIT_CONFIG: base_config},
            {
                # The candidate widens every exclusion; it must have no effect.
                admit_paths.PRE_COMMIT_CONFIG: b"exclude: .*\n",
                "docs/legacy.md": b"kept as recorded \n",
                "docs/legacy2.md": b"not excluded for this hook \n",
                "docs/table.dat": b"no final newline",
                "third_party/lib/x.c": b"int x; \r\n\r\n",
                "docs/clean.md": b"# Clean\n",
            },
        )
        check = admit_paths.check_rewriting_hooks
        self.assertEqual(
            check(repo, base, head, ["docs/legacy.md", "docs/table.dat", "third_party/lib/x.c", "docs/clean.md"]),
            list(admit_paths.REWRITING_HOOKS),
        )
        with self.assertRaises(AdmitError) as caught:
            check(repo, base, head, ["docs/legacy2.md", "docs/clean.md"])
        self.assertEqual(caught.exception.code, "ADMIT_HOOKS_WOULD_REWRITE")
        self.assertIn("docs/legacy2.md", str(caught.exception))
        self.assertNotIn("docs/clean.md:", str(caught.exception))

    def test_line_endings_follow_the_base_gitattributes(self) -> None:
        repo, base, head = self._repo(
            {".gitattributes": b"* text=auto eol=lf\n*.dat -text\n"},
            {".gitattributes": b"* -text\n", "assets/table.dat": b"a\r\nb\r\n", "docs/page.md": b"a\r\nb\r\n"},
        )
        admit_paths.check_rewriting_hooks(repo, base, head, ["assets/table.dat"])
        with self.assertRaises(AdmitError) as caught:
            admit_paths.check_rewriting_hooks(repo, base, head, ["docs/page.md"])
        self.assertIn("docs/page.md: CRLF", str(caught.exception), "the candidate's -text is ignored")

    def test_nothing_from_the_pull_request_is_executed(self) -> None:
        marker = (self.tmp / "executed").as_posix()
        payload = f"open({marker!r}, 'w').write('ran')\n".encode()
        candidate_config = (
            "repos:\n  - repo: local\n    hooks:\n      - id: trailing-whitespace\n"
            "        name: trailing-whitespace\n"
            f'        entry: "{shlex.join([Path(sys.executable).as_posix(), "-c", payload.decode()])}"\n'
            "        language: system\n"
        ).encode()
        repo, base, head = self._repo(
            {"README": b"base\n"},
            {
                admit_paths.PRE_COMMIT_CONFIG: candidate_config,
                "pre_commit.py": payload,
                "pre_commit/__init__.py": payload,
                "pre_commit/__main__.py": payload,
                "docs/new.md": b"# New \n",
                "docs/clean.md": b"# Clean\n",
            },
        )
        with self.assertRaises(AdmitError) as caught:
            admit_paths.check_rewriting_hooks(repo, base, head, ["docs/new.md"])
        self.assertEqual(caught.exception.code, "ADMIT_HOOKS_WOULD_REWRITE")
        admit_paths.check_rewriting_hooks(repo, base, head, ["docs/clean.md", "pre_commit/__main__.py"])
        self.assertFalse(Path(marker).exists(), "a pull request's hook or pre_commit module ran")
        self.assertEqual(git(repo, "status", "--porcelain"), "", "nothing was checked out or rewritten")

    def test_an_unreadable_base_configuration_is_refused(self) -> None:
        for config in (
            "repos:\n  - repo: x\n    hooks:\n      - id: trailing-whitespace\n        exclude: \"a\\.md\"\n",
            "repos:\n  - repo: x\n    hooks:\n      - id: trailing-whitespace\n      - id: trailing-whitespace\n",
            "exclude: ^(unclosed\n",
            "exclude: [a, b]\n",
        ):
            with self.subTest(config=config), self.assertRaises(AdmitError) as caught:
                admit_paths.rewriting_hook_exclusions(config)
            self.assertEqual(caught.exception.code, "ADMIT_HOOKS_CONFIG_INVALID")

    def test_the_repository_configuration_is_read(self) -> None:
        config = (ROOT / admit_paths.PRE_COMMIT_CONFIG).read_text(encoding="utf-8")
        top, hooks = admit_paths.rewriting_hook_exclusions(config)
        assert top is not None
        self.assertTrue(top.search("third_party/lib/x.c"))
        self.assertFalse(top.search("tools/admit_paths.py"))
        self.assertTrue(hooks["trailing-whitespace"].search("src/core/nk_psp_aes.c"))
        self.assertTrue(hooks["end-of-file-fixer"].search("src/rt/minimp3/LICENSE"))
        self.assertFalse(hooks["end-of-file-fixer"].search("tools/admit_paths.py"))


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
        # policy delta is the V4 record, not the historical nested candidate/baseline pair.
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
            verifier.policy_delta_key_v4(repo=pr, base_rev=self.BASE, candidate_rev=result["admission_commit"]),
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
