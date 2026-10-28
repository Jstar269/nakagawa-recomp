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
            git("checkout", "-q", "main")
            trusted = ctv.tree_entries(repo, "HEAD")
            candidate = ctv.tree_entries(repo, head)
            texts = [(repo / ".github/workflows/ci.yml").read_text(encoding="utf-8")]
            paths = ctv.protected_paths(trusted, candidate, texts)
            self.assertEqual(ctv.protected_changes(trusted, candidate, trusted, paths), [
                {"path": ".github/workflows/spoof.yml", "kind": "added"},
                {"path": "tools/gate.py", "kind": "modified"},
            ])
            self.assertEqual(ctv.context_collisions(repo, head, candidate), [".github/workflows/spoof.yml"])


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
        self.assertEqual(self.text.count("actions/checkout@"), 1)

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

    def test_producers_cover_the_actions_contexts_the_ruleset_requires(self) -> None:
        try:
            from test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        except ModuleNotFoundError:
            from tools.test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        required = MAIN_RULESET_SNAPSHOT["required_status_checks"]
        assert isinstance(required, dict)
        actions = {name for name, app in required.items() if app == ctv.GITHUB_ACTIONS_APP_ID}
        self.assertLessEqual(actions, {producer.context for producer in ctv.PRODUCERS})

    def test_each_producer_workflow_declares_its_job_and_event(self) -> None:
        for producer in ctv.PRODUCERS:
            with self.subTest(context=producer.context):
                text = (ROOT / producer.workflow).read_text(encoding="utf-8")
                self.assertRegex(text, rf"(?m)^(    name: {re.escape(producer.context)}|  {re.escape(producer.context)}:)\s*$")
                for event in producer.events:
                    self.assertRegex(text, rf"(?m)^  {event}:")


class CommandLineTests(unittest.TestCase):
    def _main(self, raw: dict, *extra: str) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snapshot.json"
            path.write_text(json.dumps(raw), encoding="utf-8")
            with mock.patch("sys.stdout"):
                return ctv.main(["--snapshot", str(path), *extra])

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
