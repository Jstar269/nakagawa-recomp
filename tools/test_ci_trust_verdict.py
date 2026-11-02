# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Canary tests for the Trusted CI verdict (#373, #364).

Each attack from #373 is replayed as synthetic GitHub API payloads, shaped like
the real ``actions/runs``, ``runs/{id}/jobs``, ``commits/{sha}/check-runs`` and
``issues/{n}/events`` answers, and must turn the verdict red. No request
leaves the process and no real pull request is opened.
"""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error
from typing import Any

try:
    import ci_trust_verdict as ctv
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_trust_verdict``.
    from tools import ci_trust_verdict as ctv

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
HEAD = "a" * 40
OTHER = "b" * 40
PR = 7
CI = ".github/workflows/ci.yml"
OSV = ".github/workflows/osv-scanner.yml"
DEP = ".github/workflows/dependency-review.yml"
PROV = ".github/workflows/provenance-attestation.yml"
PUSHED = "2026-10-28T10:00:00Z"


def _run(run_id: int, path: str, event: str = "pull_request", *, sha: str = HEAD, created: str = PUSHED,
         status: str = "completed", conclusion: str | None = "success", prs: tuple[int, ...] = (PR,)) -> dict:
    return {
        "id": run_id, "path": path, "event": event, "head_sha": sha, "created_at": created,
        "status": status, "conclusion": conclusion, "check_suite_id": run_id * 10,
        "pull_requests": [{"number": number, "base": {"ref": "main"}} for number in prs],
    }


def _job(job_id: int, name: str, *, sha: str = HEAD, status: str = "completed",
         conclusion: str | None = "success") -> dict:
    return {"id": job_id, "name": name, "head_sha": sha, "status": status, "conclusion": conclusion}


def _check(job: dict, suite: int, *, app: int = ctv.GITHUB_ACTIONS_APP_ID) -> dict:
    return {
        "id": job["id"], "name": job["name"], "head_sha": job["head_sha"], "status": job["status"],
        "conclusion": job["conclusion"], "app": {"id": app}, "check_suite": {"id": suite},
    }


def _pull_request(*, head: str = HEAD, base: str = "main", fork: bool = False,
                  labels: tuple[str, ...] = ()) -> dict:
    owner = "someone/nakagawa-recomp" if fork else "Jstar269/nakagawa-recomp"
    return {
        "number": PR, "state": "open",
        "head": {"sha": head, "repo": {"full_name": owner}},
        "base": {"ref": base, "repo": {"full_name": "Jstar269/nakagawa-recomp"}},
        "labels": [{"name": name} for name in labels],
    }


class Scenario:
    """A healthy pull-request head; each canary breaks one thing."""

    def __init__(self, *, fork: bool = False) -> None:
        self.raw: dict = {
            "head_sha": HEAD,
            "default_branch": "main",
            "trusted_sha": "c" * 40,
            "fetched_head": HEAD,
            "pull_request": _pull_request(fork=fork),
            "open_pull_requests": [],
            "label_events": [],
            "workflow_runs": [],
            "jobs": {},
            "check_runs": [],
            "statuses": [],
            "protected_changes": [],
            "context_collisions": [],
        }
        self.raw["open_pull_requests"].append(copy.deepcopy(self.raw["pull_request"]))
        self.add_run(1, CI, ["Classify change", "Hygiene and security", "CI required"])
        self.add_run(2, OSV, ["OSV Vulnerability Scan"])
        self.add_run(3, DEP, ["dependency-review"])
        self.add_run(4, PROV, ["Trusted provenance attestation"], event="pull_request_target")
        self.add_run(5, ctv.VERDICT_WORKFLOW, ["Compute trusted CI verdict"], event="pull_request_target")

    def add_run(self, run_id: int, path: str, job_names: list[str], **run_args) -> dict:
        run = _run(run_id, path, **run_args)
        self.raw["workflow_runs"].append(run)
        jobs = [_job(run_id * 100 + index, name, sha=run["head_sha"]) for index, name in enumerate(job_names)]
        self.raw["jobs"][str(run_id)] = jobs
        self.raw["check_runs"] += [_check(job, run["check_suite_id"]) for job in jobs]
        return run

    def job(self, run_id: int, name: str) -> dict:
        return next(job for job in self.raw["jobs"][str(run_id)] if job["name"] == name)

    def change(self, path: str, kind: str = "modified") -> None:
        self.raw["protected_changes"].append({"path": path, "kind": kind})

    def label(self, *, at: str = "2026-10-28T10:30:00Z", permission: str = "admin",
              event: str = "labeled", actor: str = "maintainer") -> None:
        self.raw["pull_request"]["labels"] = (
            [{"name": ctv.APPROVAL_LABEL}] if event == "labeled" else []
        )
        self.raw["label_events"].append({
            "id": len(self.raw["label_events"]) + 1, "event": event, "created_at": at,
            "actor": {"login": actor}, "label": {"name": ctv.APPROVAL_LABEL},
        })
        self.raw["label_actor_permission"] = permission

    def verdict(self) -> ctv.Verdict:
        return ctv.evaluate(ctv.normalise(self.raw))


class VerdictAssertions(unittest.TestCase):
    def assertVerdict(self, scenario: Scenario, state: str, *codes: str) -> ctv.Verdict:
        verdict = scenario.verdict()
        fatal = {item.code for item in verdict.findings if item.fatal}
        self.assertEqual(verdict.state, state, verdict.findings)
        for code in codes:
            self.assertIn(code, fatal, verdict.findings)
        if state == "success":
            self.assertFalse(fatal)
        self.assertLessEqual(len(verdict.description()), 140)
        return verdict


class HealthyHeadTests(VerdictAssertions):
    def test_every_intended_job_from_its_own_workflow_passes(self) -> None:
        self.assertVerdict(Scenario(), "success")

    def test_a_running_job_is_pending_not_failed(self) -> None:
        scenario = Scenario()
        scenario.job(1, "CI required").update(status="in_progress", conclusion=None)
        verdict = self.assertVerdict(scenario, "pending")
        self.assertEqual(verdict.pending, ["CI required"])

    def test_a_workflow_that_has_not_started_is_pending(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"] = [run for run in scenario.raw["workflow_runs"] if run["path"] != OSV]
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"]
                                      if check["check_suite"]["id"] != 20]
        verdict = self.assertVerdict(scenario, "pending")
        self.assertIn("OSV Vulnerability Scan", verdict.description())

    def test_an_unfinished_run_without_the_job_yet_is_pending(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][0].update(status="in_progress", conclusion=None)
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"] if job["name"] != "CI required"]
        self.assertVerdict(scenario, "pending")

    def test_a_rerun_after_a_cancelled_run_counts(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][0]["created_at"] = "2026-10-28T09:00:00Z"
        scenario.add_run(9, CI, ["Hygiene and security", "CI required"],
                         created="2026-10-28T09:30:00Z", conclusion="cancelled")
        self.assertVerdict(scenario, "success")


class SameNameJobCanaryTests(VerdictAssertions):
    """Canary: a candidate adds or edits a workflow whose job is named like a required context."""

    def test_added_workflow_with_a_required_job_name_is_foreign(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, ".github/workflows/evil.yml", ["CI required"])
        scenario.change(".github/workflows/evil.yml", "added")
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER", "CI_CHANGE_UNAPPROVED")

    def test_an_approved_workflow_change_still_cannot_produce_a_required_context(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, ".github/workflows/evil.yml", ["Hygiene and security"])
        scenario.change(".github/workflows/evil.yml", "added")
        scenario.label()
        verdict = self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")
        self.assertNotIn("CI_CHANGE_UNAPPROVED", {item.code for item in verdict.findings})

    def test_check_run_from_another_app_is_foreign(self) -> None:
        scenario = Scenario()
        spoof = _check(_job(777, "dependency-review"), 9999, app=4242)
        scenario.raw["check_runs"].append(spoof)
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")

    def test_provenance_context_from_a_pull_request_run_is_foreign(self) -> None:
        # The provenance file gains a pull_request trigger: its run is then
        # candidate-defined, so the context must not count from it.
        scenario = Scenario()
        scenario.add_run(6, PROV, ["Trusted provenance attestation"], event="pull_request")
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")


class DeletedJobCanaryTests(VerdictAssertions):
    """Canary: the candidate's ci.yml drops a required job."""

    def test_missing_job_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"]
                                     if job["name"] != "Hygiene and security"]
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "JOB_MISSING", "CI_CHANGE_UNAPPROVED")

    def test_a_label_does_not_excuse_a_missing_job(self) -> None:
        scenario = Scenario()
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"] if job["name"] != "CI required"]
        scenario.change(CI)
        scenario.label()
        self.assertVerdict(scenario, "failure", "JOB_MISSING")

    def test_deleted_workflow_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"] = [run for run in scenario.raw["workflow_runs"] if run["path"] != DEP]
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"]
                                      if check["check_suite"]["id"] != 30]
        scenario.change(DEP, "deleted")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


class DuplicateContextCanaryTests(VerdictAssertions):
    def test_two_jobs_under_one_required_name_fail(self) -> None:
        scenario = Scenario()
        extra = _job(150, "CI required")
        scenario.raw["jobs"]["1"].append(extra)
        scenario.raw["check_runs"].append(_check(extra, 10))
        self.assertVerdict(scenario, "failure", "DUPLICATE_CONTEXT")

    def test_commit_status_under_a_required_name_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["statuses"].append({"context": "CI required", "state": "success"})
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")

    def test_check_run_named_like_the_verdict_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["check_runs"].append(_check(_job(888, ctv.VERDICT_CONTEXT), 50))
        self.assertVerdict(scenario, "failure", "CI_CONTEXT_COLLISION")

    def test_candidate_workflow_naming_the_verdict_fails_even_when_approved(self) -> None:
        scenario = Scenario()
        scenario.raw["context_collisions"] = [".github/workflows/ci.yml"]
        scenario.change(CI)
        scenario.label()
        self.assertVerdict(scenario, "failure", "CI_CONTEXT_COLLISION")


class ExitZeroSwapCanaryTests(VerdictAssertions):
    """Canary: failing source paired with a candidate gate that returns success."""

    def test_workflow_body_swapped_for_exit_zero_fails_despite_green_jobs(self) -> None:
        scenario = Scenario()  # every job reports success, because the body is `exit 0`
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_aggregate_tool_swapped_for_exit_zero_fails(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_required.py")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_honest_failure_stays_red(self) -> None:
        scenario = Scenario()
        scenario.job(1, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_skipped_required_job_is_not_success(self) -> None:
        scenario = Scenario()
        scenario.job(2, "OSV Vulnerability Scan").update(conclusion="skipped")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")


class StaleHeadCanaryTests(VerdictAssertions):
    def test_result_for_a_superseded_head_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["head"]["sha"] = OTHER
        self.assertVerdict(scenario, "failure", "STALE_HEAD")

    def test_head_moving_while_it_is_read_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["fetched_head"] = OTHER
        self.assertVerdict(scenario, "failure", "STALE_HEAD")

    def test_job_for_another_sha_is_a_replay(self) -> None:
        scenario = Scenario()
        scenario.job(3, "dependency-review")["head_sha"] = OTHER
        self.assertVerdict(scenario, "failure", "RUN_HEAD_MISMATCH")

    def test_run_for_another_sha_is_a_replay(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, OSV, ["OSV Vulnerability Scan"], sha=OTHER, created="2026-10-28T11:00:00Z")
        self.assertVerdict(scenario, "failure", "RUN_HEAD_MISMATCH")

    def test_older_success_cannot_mask_the_latest_failure(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, CI, ["Hygiene and security", "CI required"], created="2026-10-28T11:00:00Z")
        scenario.job(6, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_run_belonging_to_another_pull_request_fails(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, CI, ["Hygiene and security", "CI required"],
                         created="2026-10-28T11:00:00Z", prs=(99,))
        self.assertVerdict(scenario, "failure", "RUN_FOR_OTHER_PR")

    def test_same_head_open_against_another_base_fails(self) -> None:
        scenario = Scenario()
        twin = _pull_request(base="old-branch")
        twin["number"] = 99
        scenario.raw["open_pull_requests"].append(twin)
        self.assertVerdict(scenario, "failure", "AMBIGUOUS_HEAD")

    def test_definitions_behind_main_fail_even_with_a_label(self) -> None:
        scenario = Scenario()
        scenario.change(CI, "behind")
        scenario.label()
        self.assertVerdict(scenario, "failure", "CI_DEFINITION_BEHIND")

    def test_pull_request_into_another_branch_gets_no_pass(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["base"]["ref"] = "release"
        self.assertVerdict(scenario, "failure", "BASE_NOT_DEFAULT")

    def test_closed_pull_request_gets_no_pass(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["state"] = "closed"
        self.assertVerdict(scenario, "failure", "PR_NOT_OPEN")


class ForkCanaryTests(VerdictAssertions):
    """Canary: fork pull requests are judged from the API, the same way, with no candidate code."""

    def test_fork_head_with_honest_runs_passes(self) -> None:
        scenario = Scenario(fork=True)
        for run in scenario.raw["workflow_runs"]:
            run["pull_requests"] = []  # GitHub leaves this empty for fork runs
        verdict = self.assertVerdict(scenario, "success")
        self.assertTrue(ctv.normalise(scenario.raw)["pull_request"]["fork"])
        self.assertEqual(verdict.findings, [])

    def test_fork_workflow_edit_fails_without_a_label(self) -> None:
        scenario = Scenario(fork=True)
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_from_a_user_without_write_access_does_not_approve(self) -> None:
        scenario = Scenario(fork=True)
        scenario.change(CI)
        scenario.label(permission="triage", actor="helper")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


class CiToolApprovalCanaryTests(VerdictAssertions):
    """Canary: a CI tool edit needs the maintainer label, bound to this head."""

    def test_ci_tool_edit_without_label_fails(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_applied_after_this_head_arrived_approves(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        verdict = self.assertVerdict(scenario, "success")
        self.assertEqual([item.code for item in verdict.findings], ["CI_CHANGE_APPROVED"])

    def test_label_from_an_earlier_head_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T09:59:00Z")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_removed_label_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        scenario.label(at="2026-10-28T10:06:00Z", event="unlabeled")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_name_alone_without_an_event_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change(".pre-commit-config.yaml")
        scenario.raw["pull_request"]["labels"] = [{"name": ctv.APPROVAL_LABEL}]
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


class ProtectedPathTests(unittest.TestCase):
    def test_tool_references_skip_comments_and_longer_paths(self) -> None:
        text = (
            "      # tools/hle_registry_meta.py is documentation here\n"
            "        run: python tools/ci_paths.py --github-output x\n"
            '          "$py" tools/vfpu_fuzz_gen.py --synthetic\n'
            '          test -f "$install_dir/source/tools/nk_cli.py"\n'
            "          python -m unittest -v tools.test_site_mobile\n"
            "            -r tools/requirements-lock.txt\n"
        )
        self.assertEqual(
            ctv.referenced_tools(text),
            {"tools/ci_paths.py", "tools/vfpu_fuzz_gen.py", "tools/requirements-lock.txt"},
        )

    def test_change_kinds(self) -> None:
        trusted = {"a": "1", "b": "2", "c": "3"}
        candidate = {"a": "1", "b": "9", "d": "4", "c": "0"}
        merge_base = {"a": "1", "b": "2", "c": "0"}
        changes = ctv.protected_changes(trusted, candidate, merge_base, {"a", "b", "c", "d", "e"})
        self.assertEqual(changes, [
            {"path": "b", "kind": "modified"},
            {"path": "c", "kind": "behind"},
            {"path": "d", "kind": "added"},
        ])

    def test_live_workflows_protect_the_gate_tools(self) -> None:
        texts = [path.read_text(encoding="utf-8") for path in sorted(WORKFLOWS.glob("*.y*ml"))]
        tree = {path.relative_to(ROOT).as_posix(): "x" for path in (ROOT / "tools").iterdir()}
        tree |= {".pre-commit-config.yaml": "x"}
        paths = ctv.protected_paths(tree, {}, texts)
        for path in ("tools/ci_required.py", "tools/ci_paths.py", "tools/tree_lint.py",
                     "tools/provenance_attest_verify.py", "tools/ci_trust_verdict.py",
                     "tools/ci_test_shards.py", "tools/betterleaks_canary.py", ".pre-commit-config.yaml"):
            self.assertIn(path, paths)
        self.assertNotIn("tools/nk_cli.py", paths)
        self.assertNotIn("tools/hle_registry_meta.py", paths)

    def test_git_facts_on_a_real_repository(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            env = os.environ | {
                "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
            }

            def git(*args: str) -> str:
                return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                                      text=True, env=env).stdout.strip()

            def write(path: str, text: str) -> None:
                target = repo / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8", newline="\n")

            git("init", "-q", "-b", "main")
            write(".github/workflows/ci.yml", "jobs:\n  a:\n    steps:\n      - run: python tools/gate.py\n")
            write("tools/gate.py", "print('gate')\n")
            write("src/x.c", "int x;\n")
            git("add", "-A")
            git("commit", "-q", "-m", "base")
            git("checkout", "-q", "-b", "candidate")
            write("tools/gate.py", "raise SystemExit(0)\n")
            write(".github/workflows/spoof.yml", f"jobs:\n  s:\n    name: {ctv.VERDICT_CONTEXT}\n")
            write("src/x.c", "int x = 1;\n")
            git("add", "-A")
            git("commit", "-q", "-m", "candidate")
            head = git("rev-parse", "HEAD")
            git("update-ref", f"refs/pull/{PR}/head", head)
            git("checkout", "-q", "main")

            # The trusted checkout knows only main; the candidate arrives by fetch, as data.
            trusted = Path(tmp) / "trusted"
            subprocess.run(["git", "clone", "-q", "--single-branch", "-b", "main", str(repo), str(trusted)],
                           check=True, capture_output=True, env=env)
            facts = ctv.git_facts(trusted, "owner/repo", PR, head, remote=str(repo))
            self.assertEqual(facts["fetched_head"], head)
            self.assertEqual(facts["trusted_sha"], git("rev-parse", "main"))
            self.assertEqual(facts["protected_changes"], [
                {"path": ".github/workflows/spoof.yml", "kind": "added"},
                {"path": "tools/gate.py", "kind": "modified"},
            ])
            self.assertEqual(facts["context_collisions"], [".github/workflows/spoof.yml"])
            # Nothing of the candidate was checked out in the trusted tree.
            self.assertEqual((trusted / "tools/gate.py").read_text(encoding="utf-8"), "print('gate')\n")

            moved = ctv.git_facts(trusted, "owner/repo", PR, OTHER, remote=str(repo))
            self.assertEqual(moved["fetched_head"], head)
            self.assertEqual(moved["protected_changes"], [])


class WorkflowWiringTests(unittest.TestCase):
    """The verdict workflow keeps the shape its trust argument depends on."""

    def setUp(self) -> None:
        self.text = (ROOT / ctv.VERDICT_WORKFLOW).read_text(encoding="utf-8")

    @staticmethod
    def _top_name(path: Path) -> str:
        match = re.search(r"(?m)^name:[ \t]*['\"]?(.+?)['\"]?[ \t]*$", path.read_text(encoding="utf-8"))
        assert match is not None
        return match.group(1)

    def test_runs_only_trusted_definitions(self) -> None:
        on_block = self.text[self.text.index("\non:\n"):self.text.index("\nconcurrency:")]
        self.assertIn("pull_request_target:", on_block)
        self.assertIn("workflow_run:", on_block)
        self.assertNotRegex(on_block, r"(?m)^  pull_request:")
        self.assertNotIn("merge_group", on_block)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", self.text)
        self.assertIn("persist-credentials: false", self.text)
        self.assertNotIn("head.ref", self.text)
        self.assertNotIn("github.head_ref", self.text)
        self.assertEqual(self.text.count("actions/checkout@"), 2)   # the approval job and the verdict job

    def test_least_privilege(self) -> None:
        self.assertIn("\npermissions: {}\n", self.text)
        self.assertNotRegex(self.text, r"contents: write|actions: write|checks: write")
        self.assertEqual(self.text.count("pull-requests: write"), 1)
        self.assertEqual(self.text.count("statuses: write"), 1)
        for uses in re.findall(r"(?m)^\s*(?:- )?uses: (\S+)", self.text):
            self.assertRegex(uses, r"@[0-9a-f]{40}$")

    def test_retriggers_name_every_producing_workflow_but_provenance(self) -> None:
        match = re.search(r"workflows: \[([^\]]*)\]", self.text)
        assert match is not None
        names = {name.strip().strip('"') for name in match.group(1).split(",")}
        expected = {self._top_name(ROOT / producer.workflow) for producer in ctv.PRODUCERS
                    if producer.events == ("pull_request",)}
        self.assertEqual(names, expected)

    def test_does_not_name_the_provenance_context(self) -> None:
        # provenance_attest_verify.py refuses any other workflow naming its context.
        self.assertNotIn("Trusted provenance attestation", self.text)

    def test_verdict_job_is_not_named_like_the_status(self) -> None:
        self.assertNotRegex(self.text, rf"(?m)^    name: {re.escape(ctv.VERDICT_CONTEXT)}\s*$")

    def test_no_other_workflow_names_the_verdict_context(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            if path.name == Path(ctv.VERDICT_WORKFLOW).name:
                continue
            with self.subTest(workflow=path.name):
                self.assertNotIn(ctv.VERDICT_CONTEXT, path.read_text(encoding="utf-8"))

    def test_the_ruleset_requires_the_verdict_not_the_contexts_it_judges(self) -> None:
        try:
            from test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        except ModuleNotFoundError:
            from tools.test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        required = MAIN_RULESET_SNAPSHOT["required_status_checks"]
        assert isinstance(required, dict)
        actions = {name for name, app in required.items() if app == ctv.GITHUB_ACTIONS_APP_ID}
        # Since 2026-10-29 the only GitHub Actions context the ruleset requires is the verdict;
        # a producer context required by name again would be spoofable by any same-named job.
        self.assertEqual(actions, {ctv.VERDICT_CONTEXT})
        self.assertNotIn(ctv.VERDICT_CONTEXT, {producer.context for producer in ctv.PRODUCERS})

    def test_each_producer_workflow_declares_its_job_and_event(self) -> None:
        for producer in ctv.PRODUCERS:
            with self.subTest(context=producer.context):
                text = (ROOT / producer.workflow).read_text(encoding="utf-8")
                self.assertRegex(text, rf"(?m)^(    name: {re.escape(producer.context)}|  {re.escape(producer.context)}:)\s*$")
                for event in producer.events:
                    self.assertRegex(text, rf"(?m)^  {event}:")


class FakeGitHub(ctv.GitHub):
    """Answers the collector's reads from a canned scenario and records posts."""

    def __init__(self, scenario: Scenario, permission_error: bool = False) -> None:
        super().__init__("Jstar269/nakagawa-recomp", "token")
        self.raw = scenario.raw
        self.permission_error = permission_error
        self.posts: list[tuple[str, dict]] = []
        self.reads: list[str] = []

    def get(self, path: str) -> object:
        self.reads.append(path)
        if path == f"pulls/{PR}":
            return self.raw["pull_request"]
        if path.startswith("collaborators/"):
            if self.permission_error:
                raise ctv.urllib.error.HTTPError(path, 403, "forbidden", {}, None)  # type: ignore[arg-type]
            return {"permission": self.raw.get("label_actor_permission")}
        raise AssertionError(path)

    def paginate(self, path: str, key: str | None = None) -> list:
        self.reads.append(path)
        if path == "pulls?state=open":
            returned: list[Any] = self.raw["open_pull_requests"]
            return returned
        if path == f"issues/{PR}/events":
            returned = self.raw["label_events"]
            return returned
        if path.startswith("actions/runs?head_sha="):
            returned = self.raw["workflow_runs"]
            return returned
        if path.startswith("actions/runs/"):
            returned = self.raw["jobs"][path.split("/")[2]]
            return returned
        if path.startswith(f"commits/{HEAD}/check-runs"):
            returned = self.raw["check_runs"]
            return returned
        if path == f"commits/{HEAD}/statuses":
            returned = self.raw["statuses"]
            return returned
        raise AssertionError(path)

    def post(self, path: str, body: dict) -> object:
        self.posts.append((path, body))
        return None


def _facts(changes: list[dict] | None = None) -> dict:
    return {"fetched_head": HEAD, "trusted_sha": "c" * 40,
            "protected_changes": changes or [], "context_collisions": []}


class CollectorTests(unittest.TestCase):
    """The live path: what the collector reads, and what main() posts."""

    def test_collect_by_head_finds_the_pull_request_and_reads_every_source(self) -> None:
        scenario = Scenario()
        scenario.label(at="2026-10-28T10:05:00Z", permission="write")
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()) as facts:
            raw = ctv.collect(gh, Path("."), "main", HEAD, None)
        assert raw is not None
        facts.assert_called_once_with(Path("."), gh.repository, PR, HEAD)
        self.assertEqual(raw["label_actor_permission"], "write")
        self.assertEqual(set(raw["jobs"]), {"1", "2", "3", "4"})  # producer workflows only
        self.assertIn(f"commits/{HEAD}/check-runs?filter=all", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_collect_reads_jobs_only_for_each_producers_newest_live_run(self) -> None:
        """The rules judge a producer from its newest live run at the head, so a superseded or cancelled run's
        jobs are not read: one API call each, and the token's hourly budget ran out on 2026-10-31.
        Failing-before: the collector read the jobs of every run of a producer workflow."""
        scenario = Scenario()
        scenario.add_run(6, CI, ["Classify change", "Hygiene and security", "CI required"],
                         created="2026-10-28T09:00:00Z")
        scenario.add_run(8, OSV, ["OSV Vulnerability Scan"], created="2026-10-28T10:10:00Z",
                         conclusion="cancelled")
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            raw = ctv.collect(gh, Path("."), "main", HEAD, None)
        assert raw is not None
        self.assertEqual(set(raw["jobs"]), {"1", "2", "3", "4"})
        self.assertEqual([read for read in gh.reads if read.startswith(("actions/runs/6/", "actions/runs/8/"))], [])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_requests_are_counted_and_the_tokens_budget_is_logged(self) -> None:
        """Each run logs its API requests and the budget the token's last answer reported, a refusal included."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "token")
        response = mock.MagicMock()
        response.read.return_value = b"{}"
        response.headers = {"X-RateLimit-Limit": "1000", "X-RateLimit-Remaining": "37", "X-RateLimit-Used": "963",
                            "X-RateLimit-Reset": "1793473200", "X-RateLimit-Resource": "core"}
        response.__enter__.return_value = response
        with mock.patch.object(ctv.urllib.request, "urlopen", return_value=response):
            gh.get(f"pulls/{PR}")
            gh.get(f"pulls/{PR}")
        note = gh.budget_note()
        self.assertIn("2 request(s) this run", note)
        self.assertIn("core budget had 37 of 1000 left", note)
        self.assertRegex(note, r"resetting at \d\d:\d\d:\d\dZ$")
        refusal = urllib.error.HTTPError("https://api.github.com/x", 403, "rate limited",
                                         {"x-ratelimit-limit": "1000", "x-ratelimit-remaining": "0"},  # type: ignore[arg-type]
                                         None)
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=refusal):
            with self.assertRaises(urllib.error.HTTPError):
                gh.get(f"pulls/{PR}")
        self.assertIn("3 request(s) this run; the token's core budget had 0 of 1000 left", gh.budget_note())

    @staticmethod
    def _answer(limit: str, remaining: str) -> mock.MagicMock:
        response = mock.MagicMock()
        response.read.return_value = b"{}"
        response.headers = {"X-RateLimit-Limit": limit, "X-RateLimit-Remaining": remaining,
                            "X-RateLimit-Resource": "core"}
        response.__enter__.return_value = response
        return response

    def test_reads_use_the_read_token_and_the_status_uses_github_token(self) -> None:
        """VERDICT_READ_TOKEN carries the reads, on the maintainer's budget; the ruleset accepts the status only
        from GitHub Actions, so it is posted with GITHUB_TOKEN."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "actions-token", "read-token")
        seen: list[tuple[str, str]] = []

        def urlopen(request, timeout):
            seen.append((request.get_method(), request.get_header("Authorization")))
            return self._answer("5000", "4900") if request.get_method() == "GET" else self._answer("1000", "640")

        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=urlopen):
            gh.get(f"pulls/{PR}")
            gh.paginate("pulls?state=open")
            gh.post(f"statuses/{HEAD}", {"state": "success"})
        self.assertEqual(seen, [("GET", "Bearer read-token"), ("GET", "Bearer read-token"),
                                ("POST", "Bearer actions-token")])
        note = gh.budget_note()
        self.assertIn("3 request(s) this run (2 with the read token, 0 refused)", note)
        self.assertIn("the read token's core budget had 4900 of 5000 left", note)
        self.assertIn("GITHUB_TOKEN's core budget had 640 of 1000 left", note)

    def test_a_read_the_read_token_is_refused_goes_to_github_token(self) -> None:
        """A missing permission or a spent budget on the read token costs one retry, not the verdict."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "actions-token", "read-token")
        refusal = urllib.error.HTTPError("https://api.github.com/x", 403, "forbidden", {}, None)  # type: ignore[arg-type]
        seen: list[str] = []

        def urlopen(request, timeout):
            seen.append(request.get_header("Authorization"))
            if request.get_header("Authorization") == "Bearer read-token":
                raise refusal
            return self._answer("1000", "500")

        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=urlopen), mock.patch("sys.stderr"):
            self.assertEqual(gh.get(f"commits/{HEAD}/check-runs"), {})
        self.assertEqual(seen, ["Bearer read-token", "Bearer actions-token"])
        self.assertEqual(gh.read_refusals, 1)
        not_found = urllib.error.HTTPError("https://api.github.com/x", 404, "missing", {}, None)  # type: ignore[arg-type]
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=not_found), mock.patch("sys.stderr"):
            with self.assertRaises(urllib.error.HTTPError):
                gh.get(f"pulls/{PR}")   # missing for both tokens: still an error

    def test_main_passes_the_read_token_only_when_it_is_set(self) -> None:
        for value, expected in (("read-token", "read-token"), ("", None)):
            with self.subTest(read_token=value):
                gh = FakeGitHub(Scenario())
                env = {"GITHUB_TOKEN": "token", "VERDICT_READ_TOKEN": value}
                with mock.patch.object(ctv, "GitHub", return_value=gh) as client, \
                        mock.patch.dict(os.environ, env), mock.patch("sys.stdout"), \
                        mock.patch.object(ctv, "git_facts", return_value=_facts()):
                    ctv.main(["--repository", gh.repository, "--head-sha", HEAD])
                client.assert_called_once_with(gh.repository, "token", expected)

    def test_collect_finds_nothing_for_a_head_without_a_pull_request_into_main(self) -> None:
        scenario = Scenario()
        scenario.raw["open_pull_requests"][0]["base"]["ref"] = "release"
        self.assertIsNone(ctv.collect(FakeGitHub(scenario), Path("."), "main", HEAD, None))

    def test_unreadable_label_permission_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        gh = FakeGitHub(scenario, permission_error=True)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertIsNone(raw["label_actor_permission"])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def _main(self, gh: FakeGitHub, *args: str) -> int:
        with mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.dict(os.environ, {"GITHUB_TOKEN": "token"}), \
                mock.patch("sys.stdout"):
            returned: int = ctv.main(["--repository", gh.repository, "--post", "--target-url", "https://run", *args])
            return returned

    def test_main_posts_one_status_bound_to_the_head(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        self.assertEqual(gh.posts, [(f"statuses/{HEAD}", {
            "state": "success", "context": ctv.VERDICT_CONTEXT,
            "description": ctv.Verdict("success").description(), "target_url": "https://run",
        })])

    def test_main_posts_failure_and_exits_nonzero(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts([{"path": CI, "kind": "modified"}])):
            self.assertEqual(self._main(gh, "--pull-request", str(PR)), 1)
        self.assertEqual(gh.posts[0][1]["state"], "failure")
        self.assertIn("CI_CHANGE_UNAPPROVED", gh.posts[0][1]["description"])

    def test_main_replaces_an_old_verdict_with_error_when_it_cannot_judge(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", side_effect=subprocess.CalledProcessError(128, "git")), \
                self.assertRaises(subprocess.CalledProcessError):
            self._main(gh, "--head-sha", HEAD)
        self.assertEqual([body["state"] for _, body in gh.posts], ["error"])

    def test_main_posts_nothing_when_no_pull_request_has_the_head(self) -> None:
        scenario = Scenario()
        scenario.raw["open_pull_requests"] = []
        gh = FakeGitHub(scenario)
        self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        self.assertEqual(gh.posts, [])

    def test_paginate_follows_link_headers(self) -> None:
        gh = ctv.GitHub("owner/repo", "token")
        pages = {
            "actions/runs?head_sha=x&per_page=100": ({"workflow_runs": [1, 2]},
                                                     {"Link": '<https://api.github.com/next>; rel="next"'}),
            "https://api.github.com/next": ({"workflow_runs": [3]}, {}),
        }
        with mock.patch.object(gh, "_request", side_effect=lambda method, url, body=None: pages[url]):
            self.assertEqual(gh.paginate("actions/runs?head_sha=x", "workflow_runs"), [1, 2, 3])

    def test_requests_carry_the_token_and_api_version(self) -> None:
        gh = ctv.GitHub("owner/repo", "secret")
        response = mock.MagicMock()
        response.read.return_value = b'{"ok": true}'
        response.headers = {}
        response.__enter__.return_value = response
        with mock.patch.object(ctv.urllib.request, "urlopen", return_value=response) as urlopen:
            self.assertEqual(gh.post(f"statuses/{HEAD}", {"state": "pending"}), {"ok": True})
        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, f"https://api.github.com/repos/owner/repo/statuses/{HEAD}")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("Authorization"), "Bearer secret")
        self.assertEqual(request.get_header("X-github-api-version"), "2022-11-28")

    def _response(self, body: bytes) -> mock.MagicMock:
        response = mock.MagicMock()
        response.read.return_value = body
        response.headers = {}
        response.__enter__.return_value = response
        return response

    def _http_error(self, code: int) -> urllib.error.HTTPError:
        return urllib.error.HTTPError("https://api.github.com/x", code, "error", {}, None)

    def test_a_gateway_error_is_retried_until_github_answers(self) -> None:
        # One HTTP 502 made the verifier post "could not judge this head" on several pull requests (2026-10-30).
        gh = ctv.GitHub("owner/repo", "token")
        answers = [self._http_error(502), urllib.error.URLError("reset"), self._response(b'{"ok": true}')]
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=answers) as urlopen, \
                mock.patch.object(ctv.time, "sleep") as sleep:
            self.assertEqual(gh.get("pulls/1"), {"ok": True})
        self.assertEqual(urlopen.call_count, 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], list(ctv.RETRY_DELAYS))

    def test_a_lasting_gateway_error_still_fails_after_the_last_attempt(self) -> None:
        gh = ctv.GitHub("owner/repo", "token")
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=[self._http_error(503)] * 3), \
                mock.patch.object(ctv.time, "sleep"):
            with self.assertRaises(urllib.error.HTTPError) as raised:
                gh.get("pulls/1")
        self.assertEqual(raised.exception.code, 503)

    def test_a_refusal_is_not_retried(self) -> None:
        # 404 and 403 are answers, not outages: retrying would only spend the token budget.
        for code in (403, 404, 422):
            with self.subTest(code=code):
                gh = ctv.GitHub("owner/repo", "token")
                with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=[self._http_error(code)]) as urlopen, \
                        mock.patch.object(ctv.time, "sleep") as sleep:
                    with self.assertRaises(urllib.error.HTTPError):
                        gh.get("pulls/1")
                self.assertEqual(urlopen.call_count, 1)
                sleep.assert_not_called()

    def test_an_unfinished_producer_is_reported_pending_from_the_runs_alone(self) -> None:
        # The full read made a dozen calls on every completion and exhausted the token budget (2026-10-30).
        scenario = Scenario()
        scenario.raw["workflow_runs"][0].update(status="in_progress", conclusion=None)
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts") as facts:
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        facts.assert_not_called()
        self.assertEqual(gh.reads, ["pulls?state=open", f"actions/runs?head_sha={HEAD}"])
        self.assertEqual([(path, body["state"], body["description"]) for path, body in gh.posts], [
            (f"statuses/{HEAD}", "pending", "Waiting for: CI required, Hygiene and security"),
        ])

    def test_a_producer_with_no_run_yet_is_waited_for(self) -> None:
        scenario = Scenario()
        runs = [run for run in scenario.raw["workflow_runs"] if run["path"] != OSV]
        self.assertEqual(ctv.unfinished_producers(runs, HEAD), ["OSV Vulnerability Scan"])

    def test_a_producer_whose_runs_were_all_cancelled_gets_the_full_judgement(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][1].update(conclusion="cancelled")
        self.assertEqual(ctv.unfinished_producers(scenario.raw["workflow_runs"], HEAD), [])
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 1)
        self.assertIn("JOB_CANCELLED", gh.posts[0][1]["description"])

    def test_runs_for_other_heads_do_not_count_as_finished(self) -> None:
        scenario = Scenario()
        stale = copy.deepcopy(scenario.raw["workflow_runs"])
        for run in stale:
            run["head_sha"] = "d" * 40
        self.assertEqual(ctv.unfinished_producers(stale, HEAD),
                         [producer.context for producer in ctv.PRODUCERS])

    def test_label_events_are_read_only_when_the_label_is_on(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertNotIn(f"issues/{PR}/events", gh.reads)
        labelled = Scenario()
        labelled.label(at="2026-10-28T10:05:00Z", permission="write")
        gh = FakeGitHub(labelled)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            ctv.collect(gh, Path("."), "main", HEAD, PR)
        self.assertIn(f"issues/{PR}/events", gh.reads)

    def test_main_rejects_a_malformed_head(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch("sys.stderr"), self.assertRaises(SystemExit):
            self._main(gh, "--head-sha", "HEAD")


D1 = "1" * 64
D2 = "2" * 64
BEFORE = "d" * 40
LABELLED_AT = "2026-10-28T09:59:00Z"   # before PUSHED: the label predates the head, so only a carry can approve it
VERDICT_RUN_PATH = ctv.VERDICT_WORKFLOW


def _record(run_id: int, digest: str, *, sha: str = HEAD, path: str = VERDICT_RUN_PATH,
            event: str = "pull_request_target", conclusion: str | None = "success",
            status: str = "completed", name: str | None = None, job_sha: str | None = None,
            number: int = PR, run_prs: tuple[int, ...] = (PR,), base: str = "main") -> tuple[dict, dict]:
    """A run and its one job, shaped like the approval record the label workflow leaves."""
    run = _run(run_id, path, event, sha=sha, prs=run_prs)
    for item in run["pull_requests"]:
        item["base"]["ref"] = base
    job = _job(run_id * 100, name or f"{ctv.APPROVAL_RECORD_PREFIX}{number} {digest}", sha=job_sha or sha,
               status=status, conclusion=conclusion)
    return run, job


def _carried(digest: str | None = D1, records: tuple[str, ...] = (D1,), **label) -> Scenario:
    """A head that changes a CI tool, carrying a label applied before the head arrived."""
    scenario = Scenario()
    scenario.change("tools/ci_paths.py")
    scenario.label(at=label.pop("at", LABELLED_AT), **label)
    scenario.raw["head_digest"] = digest
    scenario.raw["approval_records"] = list(records)
    return scenario


class CarriedApprovalTests(VerdictAssertions):
    """A label applied before this head arrived approves it only when its protected content is unchanged."""

    def test_unchanged_protected_content_keeps_the_approval(self) -> None:
        verdict = self.assertVerdict(_carried(), "success")
        self.assertEqual([item.code for item in verdict.findings], ["CI_CHANGE_APPROVED"])
        self.assertIn("byte-identical", verdict.findings[0].message)

    def test_changed_protected_content_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(digest=D2), "failure", "CI_CHANGE_UNAPPROVED")

    def test_no_record_at_this_head_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(records=()), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_record_of_another_digest_is_not_this_content(self) -> None:
        self.assertVerdict(_carried(records=(D2,)), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_digest_that_could_not_be_computed_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(digest=None), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_retargeted_pull_request_does_not_carry_an_approval(self) -> None:
        scenario = _carried()
        scenario.raw["label_events"].append({"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T10:20:00Z"})
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_replace_the_label(self) -> None:
        scenario = _carried()
        scenario.raw["pull_request"]["labels"] = []
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_excuse_a_label_from_a_user_without_write(self) -> None:
        self.assertVerdict(_carried(permission="triage", actor="helper"), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_survive_the_label_being_removed(self) -> None:
        scenario = _carried()
        scenario.label(at="2026-10-28T10:06:00Z", event="unlabeled")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_carried_approval_still_needs_every_intended_job(self) -> None:
        scenario = _carried()
        scenario.job(1, "CI required")["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")


class ApprovalRecordForgeryTests(unittest.TestCase):
    """Only a successful job of a pull_request_target run of the verdict workflow at this head is a record."""

    def digests(self, *pairs: tuple[dict, dict], head: str = HEAD) -> set[str]:
        runs = [run for run, _ in pairs]
        jobs = {run["id"]: [job] for run, job in pairs}
        found: set[str] = ctv.record_digests(runs, lambda run_id: jobs[run_id], head, PR, "main")
        return found

    def test_the_genuine_record_is_read(self) -> None:
        self.assertEqual(self.digests(_record(1, D1)), {D1})

    def test_a_job_of_a_pull_request_run_is_ignored(self) -> None:
        # A candidate controls its own pull_request workflows, including a copy of this file under this path.
        self.assertEqual(self.digests(_record(1, D1, event="pull_request")), set())

    def test_a_job_of_a_workflow_run_or_dispatch_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, event="workflow_run"), _record(2, D1, event="workflow_dispatch")),
                         set())

    def test_a_job_of_another_workflow_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, path=".github/workflows/evil.yml")), set())

    def test_a_record_of_another_head_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, sha=OTHER)), set())
        self.assertEqual(self.digests(_record(2, D1, job_sha=OTHER)), set())

    def test_a_failed_cancelled_or_unfinished_record_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, conclusion="failure"), _record(2, D1, conclusion="cancelled"),
                                      _record(3, D1, conclusion=None, status="in_progress")), set())

    def test_malformed_names_are_ignored(self) -> None:
        prefix = "CI approval record v1 "
        for name in (f"{prefix}pr{PR} {D1[:-1]}", f"{prefix}pr{PR} {D1}0", f"{prefix}pr{PR} {(chr(97) * 64).upper()}",
                     f"{prefix}pr{PR}{D1}", f"{prefix}{D1}", f"{prefix}pr0{PR} {D1}", f"{prefix}pr-{PR} {D1}",
                     f"CI approval record v2 pr{PR} {D1}", f"x {prefix}pr{PR} {D1}", f"{prefix}pr{PR} {D1} ",
                     prefix, "Compute trusted CI verdict"):
            with self.subTest(name=name):
                self.assertEqual(self.digests(_record(1, D1, name=name)), set())

    def test_a_record_for_another_pull_request_at_the_same_sha_is_ignored(self) -> None:
        """Two pull requests can share a head SHA; a record names the pull request it was written for."""
        self.assertEqual(self.digests(_record(1, D1, number=PR + 1)), set())
        self.assertEqual(self.digests(_record(2, D1, number=PR)), {D1})

    def test_a_run_reported_for_another_pull_request_or_base_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, run_prs=(PR + 1,))), set())
        self.assertEqual(self.digests(_record(2, D1, base="release")), set())
        self.assertEqual(self.digests(_record(3, D1, run_prs=())), {D1})   # a fork's run reports no pull requests

    def test_a_retargeted_pull_request_is_never_carried(self) -> None:
        gh = ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))
        gh.events.append({"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T10:20:00Z"})
        with mock.patch.object(ctv, "candidate_digest", return_value=D1), self.assertRaises(ctv.ApprovalRefused):
            ctv.carry_approval(gh, Path("."), PR, BEFORE, HEAD, "main")

    def test_a_forged_record_does_not_approve_in_the_collector(self) -> None:
        """Failing-before: there was no record to forge, and a stale label was always withdrawn by the workflow."""
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED_AT)
        run, job = _record(6, D1, event="pull_request")
        scenario.raw["workflow_runs"].append(run)
        scenario.raw["jobs"]["6"] = [job]
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=D1):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertEqual(raw["approval_records"], [])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")


class CarriedApprovalCollectorTests(unittest.TestCase):
    def collect(self, scenario: Scenario, digest: str = D1, **errors) -> dict:
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=digest, **errors), \
                mock.patch("sys.stderr"):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        result: dict = raw
        return result

    def scenario(self) -> Scenario:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED_AT)
        run, job = _record(6, D1)
        scenario.raw["workflow_runs"].append(run)
        scenario.raw["jobs"]["6"] = [job]
        return scenario

    def test_a_recorded_unchanged_head_is_approved(self) -> None:
        raw = self.collect(self.scenario())
        self.assertEqual(raw["approval_records"], [D1])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_changed_content_is_not(self) -> None:
        self.assertEqual(ctv.evaluate(ctv.normalise(self.collect(self.scenario(), digest=D2))).state, "failure")

    def test_a_failed_digest_computation_drops_the_approval(self) -> None:
        raw = self.collect(self.scenario(), side_effect=subprocess.CalledProcessError(128, "git"))
        self.assertNotIn("head_digest", raw)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def test_a_failed_record_lookup_drops_the_approval(self) -> None:
        scenario = self.scenario()

        class Failing(FakeGitHub):
            def paginate(self, path: str, key: str | None = None) -> list:
                if path.startswith("actions/runs/6/jobs"):
                    raise urllib.error.URLError("connection reset")
                return super().paginate(path, key)

        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=D1), mock.patch("sys.stderr"):
            raw = ctv.collect(Failing(scenario), Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def test_nothing_is_hashed_without_a_label_or_without_a_protected_change(self) -> None:
        unlabelled = self.scenario()
        unlabelled.raw["pull_request"]["labels"] = []
        behind = self.scenario()
        behind.raw["protected_changes"] = [{"path": CI, "kind": "behind"}]
        for scenario in (unlabelled, behind):
            gh = FakeGitHub(scenario)
            with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                    mock.patch.object(ctv, "approval_digest") as digest:
                ctv.collect(gh, Path("."), "main", HEAD, PR)
            digest.assert_not_called()


class ApprovalGitHub(ctv.GitHub):
    """The label workflow's reads: the pull request, its label events, a user's permission and recorded runs."""

    def __init__(self, *, head: str = HEAD, labels: tuple[str, ...] = (ctv.APPROVAL_LABEL,),
                 permission: str | None = "write", events: list[dict] | None = None,
                 records: tuple[tuple[dict, dict], ...] = (), broken: str | None = None) -> None:
        super().__init__("Jstar269/nakagawa-recomp", "token")
        self.pull = _pull_request(head=head, labels=labels)
        self.permission = permission
        self.events = events if events is not None else [{
            "id": 1, "event": "labeled", "created_at": LABELLED_AT, "actor": {"login": "maintainer"},
            "label": {"name": ctv.APPROVAL_LABEL},
        }]
        self.records = list(records)
        self.broken = broken

    def _check(self, path: str) -> None:
        if self.broken and path.startswith(self.broken):
            raise urllib.error.URLError("down")

    def get(self, path: str) -> object:
        self._check(path)
        if path == f"pulls/{PR}":
            return self.pull
        if path.startswith("collaborators/"):
            return {"permission": self.permission}
        raise AssertionError(path)

    def paginate(self, path: str, key: str | None = None) -> list:
        self._check(path)
        if path == f"issues/{PR}/events":
            return self.events
        if path.startswith("actions/runs?head_sha="):
            sha = path.split("=", 1)[1]
            return [run for run, _ in self.records if run["head_sha"] == sha]
        if path.startswith("actions/runs/"):
            run_id = int(path.split("/")[2])
            return [job for run, job in self.records if run["id"] == run_id]
        raise AssertionError(path)


class RecordAndCarryTests(unittest.TestCase):
    """The label workflow's two steps: record on a label, carry on a push. Any doubt refuses."""

    def record(self, gh: ApprovalGitHub, digest: str = D1) -> str:
        with mock.patch.object(ctv, "candidate_digest", return_value=digest):
            digest_out: str = ctv.record_approval(gh, Path("."), PR, HEAD, "main")
            return digest_out

    def carry(self, gh: ApprovalGitHub, digest: str = D1, before: str = BEFORE) -> str:
        with mock.patch.object(ctv, "candidate_digest", return_value=digest):
            digest_out: str = ctv.carry_approval(gh, Path("."), PR, before, HEAD, "main")
            return digest_out

    def test_a_label_from_a_user_with_write_records_the_digest(self) -> None:
        self.assertEqual(self.record(ApprovalGitHub()), D1)

    def test_a_label_from_a_user_without_write_records_nothing(self) -> None:
        for permission in ("triage", "read", "none", None):
            with self.subTest(permission=permission), self.assertRaises(ctv.ApprovalRefused):
                self.record(ApprovalGitHub(permission=permission))

    def test_a_removed_label_or_a_moved_head_records_nothing(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(labels=()))
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(head=OTHER))   # the head moved after the event named HEAD
        unlabeled = [{"id": 2, "event": "unlabeled", "created_at": "2026-10-28T10:00:00Z",
                      "actor": {"login": "maintainer"}, "label": {"name": ctv.APPROVAL_LABEL}}]
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(events=ApprovalGitHub().events + unlabeled))

    def test_the_head_moving_while_it_is_hashed_records_nothing(self) -> None:
        gh = ApprovalGitHub()

        def move(*_args, **_kwargs) -> str:
            gh.pull["head"]["sha"] = OTHER
            return D1

        with mock.patch.object(ctv, "candidate_digest", side_effect=move), self.assertRaises(ctv.ApprovalRefused):
            ctv.record_approval(gh, Path("."), PR, HEAD, "main")

    def test_an_unchanged_digest_is_carried(self) -> None:
        self.assertEqual(self.carry(ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))), D1)

    def test_a_changed_digest_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),)), digest=D2)

    def test_no_record_at_the_previous_head_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub())

    def test_a_forged_record_at_the_previous_head_is_not_carried(self) -> None:
        for forged in (_record(6, D1, sha=BEFORE, event="pull_request"),
                       _record(6, D1, sha=BEFORE, path=".github/workflows/evil.yml"),
                       _record(6, D1, sha=BEFORE, conclusion="failure"),
                       _record(6, D1, sha=HEAD)):   # a record at the NEW head says nothing about the old one
            with self.subTest(forged=forged[0]["path"], event=forged[0]["event"]), self.assertRaises(ctv.ApprovalRefused):
                self.carry(ApprovalGitHub(records=(forged,)))

    def test_a_record_at_an_older_head_is_not_carried_through_a_gap(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=OTHER),)))

    def test_a_label_that_is_not_from_write_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(permission="triage", records=(_record(6, D1, sha=BEFORE),)))

    def test_a_push_that_did_not_move_the_head_is_refused(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=HEAD),)), before=HEAD)

    def test_a_closed_pull_request_or_other_base_is_not_carried(self) -> None:
        for change in ({"state": "closed"}, {"base": {"ref": "release"}}):
            gh = ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))
            gh.pull |= change
            with self.subTest(change=change), self.assertRaises(ctv.ApprovalRefused):
                self.carry(gh)

    def test_the_candidate_ref_not_matching_the_head_refuses_the_digest(self) -> None:
        with mock.patch.object(ctv, "fetch_candidate", return_value=OTHER), self.assertRaises(ctv.ApprovalRefused):
            ctv.candidate_digest(ApprovalGitHub(), Path("."), PR, HEAD)

    def _main(self, gh: ApprovalGitHub, *args: str, output: Path) -> tuple[int, str]:
        with mock.patch.object(ctv, "GitHub", return_value=gh), mock.patch.dict(os.environ, {"GITHUB_TOKEN": "t"}), \
                mock.patch.object(ctv, "candidate_digest", return_value=D1), \
                mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            code = ctv.main(["--repository", gh.repository, "--pull-request", str(PR), "--head-sha", HEAD,
                             "--github-output", str(output), *args])
        return code, output.read_text(encoding="utf-8") if output.exists() else ""

    def test_the_command_writes_the_digest_for_the_job_name_only_on_success(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            records = (_record(6, D1, sha=BEFORE),)
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "record", output=output), (0, f"digest={D1}\n"))
            output.unlink()
            self.assertEqual(self._main(ApprovalGitHub(records=records), "--approval", "carry",
                                        "--before-sha", BEFORE, output=output), (0, f"digest={D1}\n"))
            output.unlink()
            # Fail closed: no record, an API failure, or a malformed SHA leave the output empty and exit non-zero.
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "carry", "--before-sha", BEFORE,
                                        output=output), (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(records=records, broken="actions/runs"), "--approval", "carry",
                                        "--before-sha", BEFORE, output=output), (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(broken="issues/"), "--approval", "record", output=output),
                             (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "carry", "--before-sha", "nope",
                                        output=output), (2, ""))


class ApprovalDigestTests(unittest.TestCase):
    """The digest over a real repository: what changes it, and what must not."""

    WORKFLOW = "jobs:\n  a:\n    steps:\n      - run: python tools/gate.py\n"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "origin"
        self.repo.mkdir()
        self.env = os.environ | {
            "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        self.git("init", "-q", "-b", "main")
        # Without this, ``git add -A`` in push() re-reads the working tree on Linux and resets an index-only
        # mode change (update-index --chmod) back to 0644 before it is committed.
        self.git("config", "core.fileMode", "false")
        self.write(".github/workflows/ci.yml", self.WORKFLOW)
        self.write("tools/gate.py", "print('gate')\n")
        self.write("tools/other.py", "print('not a CI tool')\n")
        self.write("tools/ci_trust_verdict.py", "# the classifier\n")
        self.write("src/x.c", "int x;\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.trusted = Path(self._tmp.name) / "trusted"
        subprocess.run(["git", "clone", "-q", "--single-branch", "-b", "main", str(self.repo), str(self.trusted)],
                       check=True, capture_output=True, env=self.env)
        self.git("checkout", "-q", "-b", "candidate")

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, text=True,
                              env=self.env).stdout.strip()

    def write(self, path: str, text: str) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")

    def push(self, message: str = "push") -> str:
        """Commit the working tree as a new pull-request head, fetch it into the trusted clone, and hash it."""
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        head = self.git("rev-parse", "HEAD")
        self.git("update-ref", f"refs/pull/{PR}/head", head)
        self.head = head
        gh = ctv.GitHub("owner/repo", "token")
        fetched = ctv.fetch_candidate(self.trusted, gh.repository, PR, remote=str(self.repo))
        self.assertEqual(fetched, head)
        digest: str = ctv.candidate_digest(gh, self.trusted, PR, head, remote=str(self.repo))
        return digest

    def test_a_change_to_no_protected_path_keeps_the_digest(self) -> None:
        first = self.push()
        self.write("src/x.c", "int x = 1;\n")
        self.write("tools/other.py", "print('still not a CI tool')\n")
        self.write("docs/CONTRIBUTING.md", "text\n")
        self.assertEqual(self.push(), first)
        self.assertEqual(self.push("empty"), first)

    def test_a_change_to_a_protected_path_changes_the_digest(self) -> None:
        first = self.push()
        for path, text in ((".github/workflows/ci.yml", self.WORKFLOW + "      - run: true\n"),
                           ("tools/gate.py", "raise SystemExit(0)\n"),
                           ("tools/ci_trust_verdict.py", "# the classifier, changed\n")):
            with self.subTest(path=path):
                self.write(path, text)
                changed = self.push()
                self.assertNotEqual(changed, first)
                self.git("checkout", "-q", "HEAD~1", "--", path)
                self.assertEqual(self.push(), first)   # changing a file and reverting it is the same content

    def test_a_pattern_matching_path_that_is_added_changes_the_digest(self) -> None:
        first = self.push()
        for path in (".github/workflows/new.yml", ".github/actions/x/action.yml"):
            with self.subTest(path=path):
                self.write(path, "name: x\n")
                self.assertNotEqual(self.push(), first)
                self.git("rm", "-q", path)
                self.assertEqual(self.push(), first)

    def test_a_protected_path_that_is_removed_or_renamed_changes_the_digest(self) -> None:
        first = self.push()
        self.git("rm", "-q", "tools/gate.py")
        self.assertNotEqual(self.push(), first)
        self.git("checkout", "-q", "main", "--", "tools/gate.py")
        self.assertEqual(self.push(), first)
        self.git("mv", "tools/gate.py", "tools/gate2.py")   # same bytes under a name no workflow runs
        renamed = self.push()
        self.assertNotEqual(renamed, first)
        self.git("mv", "tools/gate2.py", "tools/gate.py")
        self.git("mv", ".github/workflows/ci.yml", ".github/workflows/ci2.yml")
        self.assertNotIn(self.push(), (first, renamed))

    def test_a_tool_that_only_the_candidates_workflow_names_is_hashed(self) -> None:
        """Failing-before: a head adds a gate tool through its own workflow edit and the label is applied; a later
        push turns the tool into ``exit 0`` without touching a workflow, and the digest did not change."""
        self.write(".github/workflows/ci.yml", self.WORKFLOW + "      - run: python tools/new_gate.py\n")
        self.write("tools/new_gate.py", "print('real gate')\n")
        approved = self.push()
        self.write("tools/new_gate.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), approved)
        # The same holds for an existing tool that only the candidate's workflow newly runs.
        self.write(".github/workflows/ci.yml", self.WORKFLOW + "      - run: python tools/other.py\n")
        self.write("tools/other.py", "print('now a gate')\n")
        newly_run = self.push()
        self.write("tools/other.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), newly_run)
        # ... and for an action's own workflow-style file.
        self.write(".github/actions/x/action.yml", "runs:\n  steps:\n    - run: python tools/third.py\n")
        self.write("tools/third.py", "print(1)\n")
        third = self.push()
        self.write("tools/third.py", "print(2)\n")
        self.assertNotEqual(self.push(), third)

    def test_a_file_mode_change_changes_the_digest(self) -> None:
        first = self.push()
        self.git("update-index", "--chmod=+x", "tools/gate.py")
        self.assertNotEqual(self.push(), first)

    def test_the_trusted_classification_not_the_candidates_picks_the_paths(self) -> None:
        """A candidate that edits a workflow to stop naming a tool still has that tool hashed: the tool set comes
        from the default branch's workflows."""
        self.write(".github/workflows/ci.yml", "jobs: {}\n")
        first = self.push()
        self.write("tools/gate.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), first)

    def test_carry_on_a_real_repository(self) -> None:
        """Failing-before: every push withdrew the approval. Now a push of unrelated files carries it and a push of
        a CI file does not."""
        approved = self.push()
        before = self.head
        gh = ApprovalGitHub(records=(_record(6, approved, sha=before),))

        def carry() -> str:
            gh.pull["head"]["sha"] = self.head
            digest: str = ctv.carry_approval(gh, self.trusted, PR, before, self.head, "main", remote=str(self.repo))
            return digest

        self.write("src/x.c", "int x = 2;\n")
        self.push()
        self.assertEqual(carry(), approved)
        self.write("tools/gate.py", "raise SystemExit(0)\n")
        self.push()
        with self.assertRaises(ctv.ApprovalRefused):
            carry()


class ApprovalWorkflowWiringTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = (ROOT / ctv.VERDICT_WORKFLOW).read_text(encoding="utf-8")

    def test_the_record_job_name_is_what_the_verifier_reads(self) -> None:
        match = re.search(r"(?m)^    name: (CI approval record v1 pr\$\{\{ github\.event\.pull_request\.number \}\} "
                          r"\$\{\{ needs\.approval\.outputs\.digest \}\})$", self.text)
        assert match is not None
        name = match.group(1).replace("${{ needs.approval.outputs.digest }}", D1)
        name = name.replace("${{ github.event.pull_request.number }}", str(PR))
        self.assertRegex(name, ctv._APPROVAL_RECORD)
        self.assertTrue(name.startswith(ctv.APPROVAL_RECORD_PREFIX))

    def test_the_verdict_waits_for_the_approval_jobs(self) -> None:
        self.assertIn("    needs: [approval, approval_record]\n", self.text)
        self.assertNotIn("clear_approval", self.text)

    def test_only_a_carried_push_keeps_the_label(self) -> None:
        # The label is removed on every push or reopen unless the carry step produced a digest.
        self.assertIn("github.event.action != 'labeled' && steps.carry.outputs.digest == ''", self.text)
        self.assertIn("if: ${{ github.event.action == 'synchronize' }}", self.text)   # reopened is never carried
        self.assertIn("!cancelled()", self.text)

    def test_the_approval_jobs_run_default_branch_code_with_the_least_they_need(self) -> None:
        approval = self.text[self.text.index("\n  approval:\n"):self.text.index("\n  approval_record:\n")]
        record = self.text[self.text.index("\n  approval_record:\n"):self.text.index("\n  verdict:\n")]
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", approval)
        self.assertIn("python tools/ci_trust_verdict.py", approval)
        self.assertNotIn("statuses: write", approval)
        self.assertIn("permissions: {}", record)
        self.assertNotIn("checkout", record)
        self.assertNotIn("secrets.", record)


class CommandLineTests(unittest.TestCase):
    def _main(self, raw: dict, *extra: str) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snapshot.json"
            path.write_text(json.dumps(raw), encoding="utf-8")
            with mock.patch("sys.stdout"):
                returned: int = ctv.main(["--snapshot", str(path), *extra])
                return returned

    def test_offline_snapshot_exit_codes(self) -> None:
        self.assertEqual(self._main(Scenario().raw), 0)
        broken = Scenario()
        broken.change(CI)
        self.assertEqual(self._main(broken.raw), 1)

    def test_snapshot_cannot_be_posted(self) -> None:
        with mock.patch("sys.stderr"), self.assertRaises(SystemExit):
            self._main(Scenario().raw, "--post")


if __name__ == "__main__":
    unittest.main()
