#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Decide the ``Trusted CI verdict`` for one pull-request head (#373, #364).

Why this exists
---------------

The ``main`` ruleset requires ``CI required``, ``Hygiene and security``,
``OSV Vulnerability Scan`` and ``dependency-review``. They come from ordinary
``pull_request`` workflows, whose definitions the pull request controls, and
GitHub matches a required check by its context name plus the GitHub Actions
integration. A pull request can therefore edit or add a workflow, keep a
required job's name and make it ``exit 0``: the context turns green without the
intended gate having run. ``integration_id=15368`` does not tell the intended
job from a candidate's job, because both are GitHub Actions.

This tool is the consumer side of the fix. It runs only from the default
branch (``.github/workflows/ci-trust-verdict.yml``, a ``pull_request_target``
and ``workflow_run`` workflow), treats every candidate byte as data, and posts
one commit status, ``Trusted CI verdict``, bound to the exact head SHA. The
ruleset then requires that status (and CodeQL, which has its own producer)
instead of the candidate-controlled contexts.

Trust boundary
--------------

Trusted: this file and its workflow, as checked out from the default branch;
GitHub's API answers about runs, jobs, check runs, statuses, labels and
permissions. Untrusted data: every blob of the candidate, read with
``git ls-tree``/``git cat-file`` and never checked out or executed.

Rules (finding codes)
---------------------

(a) Protected definitions. A protected path is anything under
``.github/workflows/`` or ``.github/actions/``, the CI tools named in
``PROTECTED_EXTRA``, and every ``tools/`` file a default-branch workflow names
outside a comment. A head whose copy differs from the default branch's is

``CI_CHANGE_UNAPPROVED``
    when the candidate changed it and the PR does not carry a current
    ``ci-change-approved`` label: applied by a user with write access, after
    this head arrived (the workflow also removes the label on every push);
``CI_DEFINITION_BEHIND``
    when only the default branch changed it. Merge ``main``; no label waives it.
``CI_CONTEXT_COLLISION``
    a candidate workflow other than the verdict workflow names the verdict
    context, or any check run is named like it (the verdict is a status).

(b) Intended producers. For each context in ``PRODUCERS`` the latest
non-cancelled run of the intended workflow path and event at this head must
contain exactly one job of that name, completed with ``success``.

``FOREIGN_PRODUCER``
    a check run or commit status under an intended context that does not come
    from a GitHub Actions run of the intended workflow path and event at this
    head: a same-name job from an edited or added workflow, another app, or a
    status.
``JOB_MISSING`` / ``JOB_FAILED`` / ``JOB_CANCELLED``
    the intended run has no such job, the job did not succeed, or every run
    was cancelled.
``DUPLICATE_CONTEXT``
    the intended run has more than one job answering to the context.

(c) Freshness.

``STALE_HEAD``
    the judged SHA is not the pull request's current head, or the head moved
    while the candidate was being fetched.
``RUN_HEAD_MISMATCH``
    a run, job or check run read for this head names another SHA (a replayed
    result).
``RUN_FOR_OTHER_PR`` / ``AMBIGUOUS_HEAD``
    the intended run belongs to a different pull request, or another open pull
    request has the same head (its base, and so its merge-ref definitions,
    may differ).
``PR_NOT_OPEN`` / ``BASE_NOT_DEFAULT``
    nothing to judge for merge into the default branch.

A run that is still going gives ``pending`` rather than a finding. Any fatal
finding makes the verdict ``failure``; ``pending`` holds only when nothing is
fatal; otherwise ``success``.

What it does not do: decide whether candidate tests are honest (that is code
review), or vouch for a status posted under the verdict context by another
GitHub Actions token. The second needs a distinct producer identity (a GitHub
App pinned in the ruleset); see docs/CI.md, "Trusted CI verdict".
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

VERDICT_CONTEXT = "Trusted CI verdict"
VERDICT_WORKFLOW = ".github/workflows/ci-trust-verdict.yml"
APPROVAL_LABEL = "ci-change-approved"
GITHUB_ACTIONS_APP_ID = 15368
WRITE_PERMISSIONS = frozenset({"admin", "maintain", "write"})
API_ROOT = "https://api.github.com"
_SHA = re.compile(r"[0-9a-f]{40}")


@dataclass(frozen=True)
class Producer:
    """A required context and the only workflow path and events allowed to produce it."""

    context: str
    workflow: str
    events: tuple[str, ...]


# The required contexts this verdict replaces, plus the provenance attestation
# (#364): its definition is always the default branch's, so here it only needs
# its producer checked. CodeQL is not listed: GitHub Advanced Security
# (integration 57789) produces it, and the ruleset keeps requiring it directly.
PRODUCERS: tuple[Producer, ...] = (
    Producer("CI required", ".github/workflows/ci.yml", ("pull_request",)),
    Producer("Hygiene and security", ".github/workflows/ci.yml", ("pull_request",)),
    Producer("OSV Vulnerability Scan", ".github/workflows/osv-scanner.yml", ("pull_request",)),
    Producer("dependency-review", ".github/workflows/dependency-review.yml", ("pull_request",)),
    Producer(
        "Trusted provenance attestation",
        ".github/workflows/provenance-attestation.yml",
        ("pull_request_target",),
    ),
)

PROTECTED_PREFIXES = (".github/workflows/", ".github/actions/")

# Gate code that decides what runs or what passes, protected even where no
# workflow line names it: the classifier and aggregate, test sharding, the
# pre-commit hook set, the pinned tool requirements, and this verifier.
PROTECTED_EXTRA = frozenset({
    ".pre-commit-config.yaml",
    "tools/ci_paths.py",
    "tools/ci_required.py",
    "tools/ci_test_shards.py",
    "tools/ci_test_weights.json",
    "tools/ci_trust_verdict.py",
    "tools/requirements-lock.txt",
})

# A tools/ path standing on its own, not the tail of a longer path such as an
# installed copy (``$dir/source/tools/nk_cli.py``) or a module name.
_TOOL_REFERENCE = re.compile(r"(?<![\w./-])(?:\./)?(tools/[A-Za-z0-9_./-]+\.[A-Za-z0-9]+)")


@dataclass(frozen=True)
class Finding:
    code: str
    subject: str
    message: str
    fatal: bool = True


@dataclass
class Verdict:
    state: str
    findings: list[Finding] = field(default_factory=list)
    pending: list[str] = field(default_factory=list)

    def description(self) -> str:
        """The status description: at most 140 characters, GitHub's limit."""
        if self.state == "success":
            text = "All intended CI jobs passed at this exact head from trusted definitions"
        elif self.state == "pending":
            text = "Waiting for: " + ", ".join(self.pending)
        else:
            fatal = [item for item in self.findings if item.fatal]
            codes = sorted({item.code for item in fatal})
            text = f"{len(fatal)} finding(s): " + ", ".join(codes)
        return text if len(text) <= 140 else text[:137] + "..."


# --- Snapshot: the API answers and git facts, normalised ------------------


def _pr_numbers(raw: list[dict] | None) -> list[int]:
    return [int(item["number"]) for item in raw or [] if "number" in item]


def normalise(raw: dict) -> dict:
    """Reduce raw GitHub API payloads to the fields the rules read.

    ``raw`` holds the API documents as GitHub returns them (``pull_request``,
    ``open_pull_requests``, ``label_events``, ``label_actor_permission``,
    ``workflow_runs``, ``jobs`` keyed by run id, ``check_runs``, ``statuses``)
    and the git facts the collector computed (``protected_changes``,
    ``context_collisions``, ``fetched_head``, ``trusted_sha``).
    """
    pr = raw.get("pull_request")
    pull_request = None
    if pr is not None:
        head_repo = (pr.get("head") or {}).get("repo") or {}
        base_repo = (pr.get("base") or {}).get("repo") or {}
        pull_request = {
            "number": int(pr["number"]),
            "state": pr.get("state"),
            "head_sha": (pr.get("head") or {}).get("sha"),
            "base_ref": (pr.get("base") or {}).get("ref"),
            "fork": head_repo.get("full_name") != base_repo.get("full_name"),
            "labels": sorted(label.get("name", "") for label in pr.get("labels") or []),
        }
    approval = None
    events = [
        event for event in raw.get("label_events") or []
        if event.get("event") in ("labeled", "unlabeled")
        and (event.get("label") or {}).get("name") == APPROVAL_LABEL
    ]
    if events:
        last = max(events, key=lambda event: (event.get("created_at") or "", event.get("id") or 0))
        approval = {
            "event": last["event"],
            "actor": (last.get("actor") or {}).get("login"),
            "created_at": last.get("created_at"),
            "actor_permission": raw.get("label_actor_permission"),
        }
    return {
        "head_sha": raw["head_sha"],
        "default_branch": raw["default_branch"],
        "trusted_sha": raw.get("trusted_sha"),
        "fetched_head": raw.get("fetched_head"),
        "pull_request": pull_request,
        "open_pull_requests": [
            {
                "number": int(item["number"]),
                "head_sha": (item.get("head") or {}).get("sha"),
                "base_ref": (item.get("base") or {}).get("ref"),
            }
            for item in raw.get("open_pull_requests") or []
        ],
        "approval": approval,
        "workflow_runs": [
            {
                "id": int(run["id"]),
                "path": run.get("path"),
                "event": run.get("event"),
                "status": run.get("status"),
                "conclusion": run.get("conclusion"),
                "head_sha": run.get("head_sha"),
                "created_at": run.get("created_at") or "",
                "check_suite_id": run.get("check_suite_id"),
                "pull_requests": _pr_numbers(run.get("pull_requests")),
            }
            for run in raw.get("workflow_runs") or []
        ],
        "jobs": {
            str(run_id): [
                {
                    "id": int(job["id"]),
                    "name": job.get("name"),
                    "status": job.get("status"),
                    "conclusion": job.get("conclusion"),
                    "head_sha": job.get("head_sha"),
                }
                for job in jobs
            ]
            for run_id, jobs in (raw.get("jobs") or {}).items()
        },
        "check_runs": [
            {
                "id": int(check["id"]),
                "name": check.get("name"),
                "app_id": (check.get("app") or {}).get("id"),
                "check_suite_id": (check.get("check_suite") or {}).get("id"),
                "status": check.get("status"),
                "conclusion": check.get("conclusion"),
                "head_sha": check.get("head_sha"),
            }
            for check in raw.get("check_runs") or []
        ],
        "statuses": [
            {"context": status.get("context"), "state": status.get("state")}
            for status in raw.get("statuses") or []
        ],
        "protected_changes": list(raw.get("protected_changes") or []),
        "context_collisions": sorted(raw.get("context_collisions") or []),
    }


# --- Rules -----------------------------------------------------------------


def _when(stamp: str | None) -> datetime | None:
    if not stamp:
        return None
    return datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def _head_arrival(snapshot: dict) -> datetime | None:
    """When this head reached a pull request: its earliest pull-request run."""
    stamps = [
        _when(run["created_at"]) for run in snapshot["workflow_runs"]
        if run["head_sha"] == snapshot["head_sha"]
        and run["event"] in ("pull_request", "pull_request_target")
    ]
    stamps = [stamp for stamp in stamps if stamp is not None]
    return min(stamps) if stamps else None


def _approval(snapshot: dict) -> tuple[bool, str]:
    pr = snapshot["pull_request"] or {}
    if APPROVAL_LABEL not in pr.get("labels", []):
        return False, f"no {APPROVAL_LABEL!r} label"
    event = snapshot["approval"]
    if event is None or event["event"] != "labeled":
        return False, f"no current {APPROVAL_LABEL!r} label event"
    if event["actor_permission"] not in WRITE_PERMISSIONS:
        return False, (f"{APPROVAL_LABEL!r} was applied by {event['actor']!r}, "
                       f"whose permission is {event['actor_permission']!r}, not write")
    arrival = _head_arrival(snapshot)
    applied = _when(event["created_at"])
    if arrival is None or applied is None or applied <= arrival:
        return False, (f"{APPROVAL_LABEL!r} predates this head; "
                       "review the new head and apply the label again")
    return True, f"{APPROVAL_LABEL!r} applied by {event['actor']!r} after this head arrived"


def _check_pull_request(snapshot: dict) -> list[Finding]:
    head = snapshot["head_sha"]
    pr = snapshot["pull_request"]
    if pr is None:
        return [Finding("STALE_HEAD", head, "no open pull request has this head")]
    findings = []
    if pr["state"] != "open":
        findings.append(Finding("PR_NOT_OPEN", f"#{pr['number']}", f"pull request is {pr['state']}"))
    if pr["base_ref"] != snapshot["default_branch"]:
        findings.append(Finding(
            "BASE_NOT_DEFAULT", f"#{pr['number']}",
            f"base is {pr['base_ref']!r}, not the default branch {snapshot['default_branch']!r}",
        ))
    if pr["head_sha"] != head:
        findings.append(Finding(
            "STALE_HEAD", head,
            f"pull request #{pr['number']} now has head {pr['head_sha']}; this result is stale",
        ))
    fetched = snapshot["fetched_head"]
    if fetched is not None and fetched != head:
        findings.append(Finding(
            "STALE_HEAD", head, f"the pull request head moved to {fetched} while it was being read",
        ))
    for other in snapshot["open_pull_requests"]:
        if other["head_sha"] == head and other["number"] != pr["number"]:
            findings.append(Finding(
                "AMBIGUOUS_HEAD", f"#{other['number']}",
                f"open pull request #{other['number']} (base {other['base_ref']!r}) has the same head; "
                "its runs may use other definitions",
            ))
    return findings


def _check_definitions(snapshot: dict) -> list[Finding]:
    findings = []
    approved, reason = _approval(snapshot)
    for change in snapshot["protected_changes"]:
        path = change["path"]
        if change["kind"] == "behind":
            findings.append(Finding(
                "CI_DEFINITION_BEHIND", path,
                "the default branch changed this CI definition after the branch point; merge main",
            ))
        elif approved:
            findings.append(Finding("CI_CHANGE_APPROVED", path, reason, fatal=False))
        else:
            findings.append(Finding(
                "CI_CHANGE_UNAPPROVED", path,
                f"the candidate changes a CI definition or gate tool ({change['kind']}); {reason}",
            ))
    for path in snapshot["context_collisions"]:
        findings.append(Finding(
            "CI_CONTEXT_COLLISION", path,
            f"candidate workflow names the reserved context {VERDICT_CONTEXT!r}",
        ))
    for check in snapshot["check_runs"]:
        if check["name"] == VERDICT_CONTEXT:
            findings.append(Finding(
                "CI_CONTEXT_COLLISION", f"check run {check['id']}",
                f"a check run is named {VERDICT_CONTEXT!r}; the verdict is only ever a commit status",
            ))
    return findings


def _check_producer(snapshot: dict, producer: Producer, pending: list[str]) -> list[Finding]:
    head = snapshot["head_sha"]
    pr = snapshot["pull_request"] or {}
    runs_by_suite = {run["check_suite_id"]: run for run in snapshot["workflow_runs"]}
    findings = []

    for check in snapshot["check_runs"]:
        if check["name"] != producer.context:
            continue
        run = runs_by_suite.get(check["check_suite_id"])
        if check["head_sha"] != head:
            findings.append(Finding(
                "RUN_HEAD_MISMATCH", producer.context,
                f"check run {check['id']} is for {check['head_sha']}, not this head",
            ))
        if check["app_id"] != GITHUB_ACTIONS_APP_ID or run is None:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"check run {check['id']} (app {check['app_id']}) is not from a workflow run at this head",
            ))
        elif run["path"] != producer.workflow or run["event"] not in producer.events:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"check run {check['id']} comes from {run['path']} ({run['event']}), "
                f"not {producer.workflow} ({'/'.join(producer.events)})",
            ))
    for status in snapshot["statuses"]:
        if status["context"] == producer.context:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"a commit status ({status['state']}) uses this context; only the job may produce it",
            ))

    runs = [
        run for run in snapshot["workflow_runs"]
        if run["path"] == producer.workflow and run["event"] in producer.events
    ]
    for run in runs:
        if run["head_sha"] != head:
            findings.append(Finding(
                "RUN_HEAD_MISMATCH", producer.context,
                f"run {run['id']} is for {run['head_sha']}, not this head",
            ))
    runs = [run for run in runs if run["head_sha"] == head]
    live = [run for run in runs if run["conclusion"] != "cancelled"]
    if not runs:
        pending.append(producer.context)
        return findings
    if not live:
        findings.append(Finding(
            "JOB_CANCELLED", producer.context, f"every {producer.workflow} run at this head was cancelled; re-run it",
        ))
        return findings
    latest = max(live, key=lambda run: (run["created_at"], run["id"]))
    if latest["pull_requests"] and pr.get("number") not in latest["pull_requests"]:
        findings.append(Finding(
            "RUN_FOR_OTHER_PR", producer.context,
            f"run {latest['id']} belongs to pull request(s) {latest['pull_requests']}",
        ))
    jobs = [job for job in snapshot["jobs"].get(str(latest["id"]), []) if job["name"] == producer.context]
    if len(jobs) > 1:
        findings.append(Finding(
            "DUPLICATE_CONTEXT", producer.context,
            f"run {latest['id']} has {len(jobs)} jobs named {producer.context!r}",
        ))
        return findings
    if not jobs:
        if latest["status"] == "completed":
            findings.append(Finding(
                "JOB_MISSING", producer.context,
                f"run {latest['id']} of {producer.workflow} finished without a {producer.context!r} job",
            ))
        else:
            pending.append(producer.context)
        return findings
    job = jobs[0]
    if job["head_sha"] != head:
        findings.append(Finding(
            "RUN_HEAD_MISMATCH", producer.context, f"job {job['id']} is for {job['head_sha']}, not this head",
        ))
    elif job["status"] != "completed":
        pending.append(producer.context)
    elif job["conclusion"] != "success":
        findings.append(Finding(
            "JOB_FAILED", producer.context, f"job {job['id']} concluded {job['conclusion']!r}",
        ))
    return findings


def evaluate(snapshot: dict) -> Verdict:
    """Apply every rule to a normalised snapshot. Pure: no I/O."""
    findings = _check_pull_request(snapshot) + _check_definitions(snapshot)
    pending: list[str] = []
    for producer in PRODUCERS:
        findings += _check_producer(snapshot, producer, pending)
    if any(item.fatal for item in findings):
        state = "failure"
    elif pending:
        state = "pending"
    else:
        state = "success"
    return Verdict(state, findings, pending)


# --- Git facts (candidate objects are data, never checked out) -------------


def _git(repo: Path, *args: str) -> bytes:
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True,
    ).stdout


def tree_entries(repo: Path, rev: str) -> dict[str, str]:
    """Map every path in ``rev``'s tree to ``mode:object``."""
    entries = {}
    for record in _git(repo, "ls-tree", "-r", "-z", "--full-tree", rev).split(b"\0"):
        if not record:
            continue
        meta, _, path = record.partition(b"\t")
        mode, _kind, obj = meta.decode("ascii").split(" ")
        entries[path.decode("utf-8", "surrogateescape")] = f"{mode}:{obj}"
    return entries


def referenced_tools(workflow_text: str) -> set[str]:
    """``tools/`` paths a workflow names outside a comment line."""
    found = set()
    for line in workflow_text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        found.update(match.group(1) for match in _TOOL_REFERENCE.finditer(line))
    return found


def protected_paths(trusted: dict[str, str], candidate: dict[str, str], workflow_texts: list[str]) -> set[str]:
    """Paths whose bytes define or decide the CI gates."""
    paths = {path for path in set(trusted) | set(candidate) if path.startswith(PROTECTED_PREFIXES)}
    paths |= PROTECTED_EXTRA
    for text in workflow_texts:
        paths |= {path for path in referenced_tools(text) if path in trusted}
    return paths


def protected_changes(
    trusted: dict[str, str], candidate: dict[str, str], merge_base: dict[str, str], paths: set[str],
) -> list[dict]:
    """Protected paths whose candidate copy differs from the default branch's.

    ``kind`` is ``behind`` when the candidate still has the merge base's copy
    (only the default branch moved), otherwise added, deleted or modified.
    """
    changes = []
    for path in sorted(paths):
        ours, theirs, base = trusted.get(path), candidate.get(path), merge_base.get(path)
        if ours == theirs:
            continue
        if theirs == base:
            kind = "behind"
        elif ours is None:
            kind = "added"
        elif theirs is None:
            kind = "deleted"
        else:
            kind = "modified"
        changes.append({"path": path, "kind": kind})
    return changes


def context_collisions(repo: Path, candidate_rev: str, candidate: dict[str, str]) -> list[str]:
    needle = VERDICT_CONTEXT.encode("utf-8")
    hits = []
    for path, entry in sorted(candidate.items()):
        if not path.startswith(".github/workflows/") or path == VERDICT_WORKFLOW:
            continue
        if needle in _git(repo, "cat-file", "blob", entry.split(":", 1)[1]):
            hits.append(path)
    return hits


def git_facts(repo: Path, repository: str, number: int, head_sha: str, remote: str | None = None) -> dict:
    """Fetch ``refs/pull/N/head`` as objects only and compare its CI definitions."""
    ref = f"refs/nk-candidate/pr-{number}"
    _git(repo, "fetch", "--no-tags", "--no-recurse-submodules", "--quiet",
         remote or f"https://github.com/{repository}.git", f"+refs/pull/{number}/head:{ref}")
    fetched = _git(repo, "rev-parse", ref).decode("ascii").strip()
    facts: dict = {"fetched_head": fetched, "trusted_sha": _git(repo, "rev-parse", "HEAD").decode("ascii").strip()}
    if fetched != head_sha:
        return facts | {"protected_changes": [], "context_collisions": []}
    trusted = tree_entries(repo, "HEAD")
    candidate = tree_entries(repo, head_sha)
    merge_base_sha = _git(repo, "merge-base", "HEAD", head_sha).decode("ascii").strip()
    merge_base = tree_entries(repo, merge_base_sha)
    texts = [
        _git(repo, "cat-file", "blob", entry.split(":", 1)[1]).decode("utf-8", "replace")
        for path, entry in sorted(trusted.items())
        if path.startswith(".github/workflows/") and path.endswith((".yml", ".yaml"))
    ]
    paths = protected_paths(trusted, candidate, texts)
    return facts | {
        "protected_changes": protected_changes(trusted, candidate, merge_base, paths),
        "context_collisions": context_collisions(repo, head_sha, candidate),
    }


# --- GitHub API ------------------------------------------------------------


class GitHub:
    def __init__(self, repository: str, token: str) -> None:
        self.repository = repository
        self.token = token

    def _request(self, method: str, path: str, body: dict | None = None) -> tuple[object, dict]:
        url = path if path.startswith("https://") else f"{API_ROOT}/repos/{self.repository}/{path}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        request = urllib.request.Request(url, data=data, method=method, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "nakagawa-recomp-ci-trust-verdict",
        })
        with urllib.request.urlopen(request, timeout=60) as response:
            payload = response.read()
            return (json.loads(payload) if payload else None), dict(response.headers)

    def get(self, path: str) -> object:
        return self._request("GET", path)[0]

    def paginate(self, path: str, key: str | None = None) -> list:
        items: list = []
        url: str | None = path + ("&" if "?" in path else "?") + "per_page=100"
        while url:
            payload, headers = self._request("GET", url)
            items.extend(payload[key] if key else payload)
            match = re.search(r'<([^>]+)>;\s*rel="next"', headers.get("Link", ""))
            url = match.group(1) if match else None
        return items

    def post(self, path: str, body: dict) -> object:
        return self._request("POST", path, body)[0]


def collect(gh: GitHub, repo: Path, default_branch: str, head_sha: str, number: int | None) -> dict | None:
    """Read everything the rules need. ``None`` when no pull request into the default branch has this head."""
    open_prs = gh.paginate("pulls?state=open")
    if number is None:
        matches = [pr for pr in open_prs if pr["head"]["sha"] == head_sha
                   and pr["base"]["ref"] == default_branch]
        if not matches:
            return None
        number = min(int(pr["number"]) for pr in matches)
    pr = gh.get(f"pulls/{number}")
    assert isinstance(pr, dict)
    raw: dict = {
        "head_sha": head_sha,
        "default_branch": default_branch,
        "pull_request": pr,
        "open_pull_requests": open_prs,
        "label_events": gh.paginate(f"issues/{number}/events"),
        "workflow_runs": gh.paginate(f"actions/runs?head_sha={head_sha}", "workflow_runs"),
        "check_runs": gh.paginate(f"commits/{head_sha}/check-runs?filter=all", "check_runs"),
        "statuses": gh.paginate(f"commits/{head_sha}/statuses"),
    }
    events = [event for event in raw["label_events"]
              if event.get("event") in ("labeled", "unlabeled")
              and (event.get("label") or {}).get("name") == APPROVAL_LABEL]
    if events:
        actor = (max(events, key=lambda event: event.get("created_at") or "").get("actor") or {}).get("login")
        if actor:
            try:
                answer = gh.get(f"collaborators/{urllib.parse.quote(actor)}/permission")
                raw["label_actor_permission"] = (answer or {}).get("permission")  # type: ignore[union-attr]
            except urllib.error.HTTPError:
                raw["label_actor_permission"] = None
    workflows = {producer.workflow for producer in PRODUCERS}
    raw["jobs"] = {
        str(run["id"]): gh.paginate(f"actions/runs/{run['id']}/jobs?filter=latest", "jobs")
        for run in raw["workflow_runs"] if run.get("path") in workflows
    }
    return raw | git_facts(repo, gh.repository, number, head_sha)


def summary_markdown(verdict: Verdict, snapshot: dict) -> str:
    pr = snapshot["pull_request"] or {}
    lines = [
        f"### {VERDICT_CONTEXT}: {verdict.state}",
        "",
        f"Head `{snapshot['head_sha']}` of #{pr.get('number', '?')}, judged with the default branch at "
        f"`{snapshot.get('trusted_sha') or 'unknown'}`.",
        "",
    ]
    if verdict.pending:
        lines += ["Waiting for: " + ", ".join(f"`{name}`" for name in verdict.pending), ""]
    if verdict.findings:
        lines += ["| Code | Subject | Detail |", "| --- | --- | --- |"]
        lines += [
            f"| {'' if item.fatal else '(info) '}`{item.code}` | `{item.subject}` | {item.message} |"
            for item in verdict.findings
        ]
    else:
        lines.append("No findings.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo-dir", type=Path, default=Path("."),
                        help="the trusted default-branch checkout (full history)")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--default-branch", default="main")
    parser.add_argument("--head-sha", help="the head to judge (workflow_run, pull_request_target)")
    parser.add_argument("--pull-request", type=int, help="the pull request (pull_request_target, dispatch)")
    parser.add_argument("--post", action="store_true", help="post the verdict as a commit status")
    parser.add_argument("--target-url", default="", help="link for the status (this run)")
    parser.add_argument("--summary", type=Path, help="append a Markdown report here")
    parser.add_argument("--snapshot", type=Path, help="evaluate a saved raw snapshot offline instead")
    parser.add_argument("--snapshot-out", type=Path, help="save the raw snapshot read from GitHub")
    args = parser.parse_args(argv)
    if args.snapshot and args.post:
        parser.error("--post judges live GitHub state; it cannot be combined with --snapshot")

    if args.snapshot:
        snapshot = normalise(json.loads(args.snapshot.read_text(encoding="utf-8")))
        verdict = evaluate(snapshot)
        _report(verdict, snapshot, args.summary)
        return 1 if verdict.state == "failure" else 0

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token or not args.repository:
        parser.error("GITHUB_TOKEN and --repository are required unless --snapshot is given")
    gh = GitHub(args.repository, token)
    head = args.head_sha
    if head is None:
        if args.pull_request is None:
            parser.error("give --head-sha or --pull-request")
        head = gh.get(f"pulls/{args.pull_request}")["head"]["sha"]  # type: ignore[index]
    if not _SHA.fullmatch(head or ""):
        parser.error("head SHA must be 40 lowercase hex digits")

    def post(state: str, description: str) -> None:
        if args.post:
            gh.post(f"statuses/{head}", {
                "state": state,
                "context": VERDICT_CONTEXT,
                "description": description,
                **({"target_url": args.target_url} if args.target_url else {}),
            })

    try:
        raw = collect(gh, args.repo_dir, args.default_branch, head, args.pull_request)
        if raw is None:
            print(f"no open pull request into {args.default_branch} has head {head}; no verdict")
            return 0
        if args.snapshot_out:
            args.snapshot_out.write_text(json.dumps(raw, indent=1, sort_keys=True), encoding="utf-8")
        snapshot = normalise(raw)
        verdict = evaluate(snapshot)
    except Exception as error:
        # An earlier verdict for this head may still say success. Replace it:
        # a head that cannot be judged now is not a passing head.
        post("error", f"verifier could not judge this head: {type(error).__name__}")
        raise
    _report(verdict, snapshot, args.summary)
    post(verdict.state, verdict.description())
    return 1 if verdict.state == "failure" else 0


def _report(verdict: Verdict, snapshot: dict, summary: Path | None) -> None:
    report = summary_markdown(verdict, snapshot)
    print(report)
    if summary:
        with summary.open("a", encoding="utf-8") as handle:
            handle.write(report)


if __name__ == "__main__":
    sys.exit(main())
