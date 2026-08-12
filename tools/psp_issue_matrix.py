# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate a current, machine-readable matrix for every open GitHub issue.

The generator intentionally snapshots only public issue metadata and a short
sanitized first claim.  It never reads private game inputs or local logs.  Run
it again before a new campaign; the checked-in JSON is a dated handoff, not a
replacement for the canonical issue body.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "docs" / "PSP_ISSUE_MATRIX.json"
STATES = {
    "LOCAL_IMPLEMENTATION_READY",
    "LOCAL_ACCEPTANCE_READY",
    "LOCAL_PRIVATE_ROUTE_READY",
    "PSP_HARDWARE_READY",
    "UPSTREAM_BLOCKED",
    "LEGAL_HUMAN_BLOCKED",
    "ENVIRONMENT_BLOCKED",
    "MAJOR_FUTURE_WORK",
}

LEGAL = {27, 98, 99, 102, 104, 149, 152, 154}
UPSTREAM = {248}
ENVIRONMENT = {54, 105}
PRIVATE_ROUTE = {31, 32, 33, 35, 139, 153, 196}
IMPLEMENTATION = {148, 151, 178, 182, 184, 187, 195, 197}
LOCAL_ACCEPTANCE = {
    45, 48, 51, 56, 57, 72, 76, 89, 174, 176, 179, 180, 181, 183, 186, 188, 189
}
HARDWARE = {
    1, 2, 3, 13, 14, 16, 20, 23, 24, 26, 34, 36, 38, 40, 44, 55, 61, 62, 63,
    64, 68, 69, 70, 74, 75, 77, 78, 79, 80, 82, 83, 84, 86, 87, 88, 90, 91,
    92, 93, 94, 116,
}

HARDWARE_IDS = {
    **{number: "PSP-KERNEL-001" for number in {1, 2, 3, 13, 14, 16, 20, 26, 61, 74, 79, 84, 88, 92, 93, 116}},
    **{number: "PSP-IO-001" for number in {55, 63, 68, 72, 91}},
    **{number: "PSP-DISPLAY-001" for number in {23, 24, 40, 44, 64, 83, 87, 89}},
    **{number: "PSP-SYSTEM-001" for number in {34, 62, 77, 78, 80, 86, 94}},
    **{number: "PSP-AUDIO-001" for number in {38, 69, 70, 75}},
    **{number: "PSP-KERNEL-001" for number in {36, 82, 90}},
}

DEPENDENCIES = {
    31: [38, 69],
    32: [31, 38, 69, 70],
    35: [181],
    76: [181],
    116: [1],
    139: [196],
    153: [189],
    196: [139],
}


def _run_gh() -> list[dict[str, Any]]:
    command = [
        "gh", "issue", "list", "--state", "open", "--limit", "100", "--json",
        "number,title,body,labels,url,updatedAt",
    ]
    try:
        completed = subprocess.run(
            command, cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8"
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"cannot query open issues with gh: {type(exc).__name__}") from exc
    return json.loads(completed.stdout)


def _claim(body: str) -> str:
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("<!--"):
            continue
        line = re.sub(r"[A-Za-z]:[\\/][^\s)`]+", "<private-path>", line)
        line = re.sub(r"(?:^|\s)/(?:[^\s)`]+)", " <private-path>", line)
        line = re.sub(r"\s+", " ", line).strip()
        return line[:320]
    return "Canonical issue body has no extractable first claim; inspect the issue before coding."


def _state(number: int) -> str:
    if number in LEGAL:
        return "LEGAL_HUMAN_BLOCKED"
    if number in UPSTREAM:
        return "UPSTREAM_BLOCKED"
    if number in ENVIRONMENT:
        return "ENVIRONMENT_BLOCKED"
    if number in PRIVATE_ROUTE:
        return "LOCAL_PRIVATE_ROUTE_READY"
    if number in IMPLEMENTATION:
        return "LOCAL_IMPLEMENTATION_READY"
    if number in LOCAL_ACCEPTANCE:
        return "LOCAL_ACCEPTANCE_READY"
    if number in HARDWARE:
        return "PSP_HARDWARE_READY"
    return "MAJOR_FUTURE_WORK"


def _local_command(state: str, number: int) -> str:
    if state == "PSP_HARDWARE_READY":
        return "python tools/psp_readiness.py --json && python tools/psp_oracle/run_psplink.py --dry-run"
    if state == "LEGAL_HUMAN_BLOCKED":
        return "python tools/publish_audit.py --tracked-only"
    if state == "UPSTREAM_BLOCKED":
        return "cd interface; npm ci; npm test; npm run lint; npm run typecheck; npm run build"
    if state == "ENVIRONMENT_BLOCKED":
        return "python tools/psp_readiness.py --json"
    if number == 182:
        return "python -m unittest discover -s tools -p 'test_ref_*.py' -v"
    if number in {174, 176, 180, 181, 184, 187, 188, 189}:
        return "python -m unittest discover -s tools -p 'test_*.py' -v"
    return "python tools/psp_readiness.py --json"


def build_matrix(issues: list[dict[str, Any]], *, generated_at: str) -> dict[str, Any]:
    rows = []
    for issue in sorted(issues, key=lambda item: int(item["number"])):
        number = int(issue["number"])
        state = _state(number)
        hardware_id = HARDWARE_IDS.get(number)
        needs_psp = state == "PSP_HARDWARE_READY"
        rows.append(
            {
                "issue": number,
                "title": issue["title"],
                "url": issue.get("url"),
                "updated_at": issue.get("updatedAt"),
                "primary_state": state,
                "current_implementation_state": "Open; this snapshot is a routing aid, not closure evidence.",
                "smallest_unresolved_claim": _claim(issue.get("body") or ""),
                "local_test_command": _local_command(state, number),
                "required_fixture": "fixtures/psp_oracle" if needs_psp else "source/tests/private route named by the canonical issue",
                "fixture_scope": "public/synthetic" if needs_psp else ("private" if state == "LOCAL_PRIVATE_ROUTE_READY" else "source-owned or canonical issue fixture"),
                "expected_observable": "Scalar result records must be deterministic; no pointers, retail bytes, or captures are acceptance evidence.",
                "independent_reference": "PPSSPP/PSPAutotests may corroborate but cannot replace PSP evidence." if needs_psp else "Use source/tests and the canonical issue acceptance criteria.",
                "real_psp_evidence_needed": needs_psp,
                "hardware_test_id": hardware_id,
                "dependency_issues": DEPENDENCIES.get(number, []),
                "recommended_next_agent": "maintainer + physical PSP session" if needs_psp else "next scoped implementation/review agent",
                "closable_now": False,
            }
        )
    return {
        "schema": 1,
        "generated_at": generated_at,
        "source": "gh issue list --state open --limit 100",
        "repository": "Jstar269/nakagawa-recomp",
        "state_values": sorted(STATES),
        "issue_count": len(rows),
        "rows": rows,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", type=Path, help="JSON from gh issue list; omit to query gh")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    issues = json.loads(args.input.read_text(encoding="utf-8-sig")) if args.input else _run_gh()
    if not isinstance(issues, list):
        raise SystemExit("issue input must be a JSON array")
    generated_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    matrix = build_matrix(issues, generated_at=generated_at)
    try:
        matrix["main_sha"] = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
            capture_output=True, text=True, encoding="utf-8"
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        matrix["main_sha"] = None
    if matrix["issue_count"] == 0:
        raise SystemExit("refusing to write an empty open-issue matrix")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"psp_issue_matrix: {matrix['issue_count']} open issues -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
