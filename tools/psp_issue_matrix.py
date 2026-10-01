# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate a current, machine-readable matrix for every open GitHub issue.

The generator snapshots public issue metadata and combines it with checked-in
routing metadata. It never reads private game inputs or local logs.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Mapping


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "docs" / "PSP_ISSUE_MATRIX.json"
DEFAULT_ROUTING = ROOT / "assets" / "issue_routing.json"
DEFAULT_ORACLE_MANIFEST = ROOT / "tools" / "psp_oracle" / "manifest.json"

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
MISSING_ROUTING_STATE = "MISSING_ROUTING"
ROUTING_STATES = {
    "legal",
    "upstream",
    "environment",
    "private_route",
    "implementation",
    "local_acceptance",
    "hardware",
    "future_work",
}
STATE_OUTPUT = {
    "legal": "LEGAL_HUMAN_BLOCKED",
    "upstream": "UPSTREAM_BLOCKED",
    "environment": "ENVIRONMENT_BLOCKED",
    "private_route": "LOCAL_PRIVATE_ROUTE_READY",
    "implementation": "LOCAL_IMPLEMENTATION_READY",
    "local_acceptance": "LOCAL_ACCEPTANCE_READY",
    "hardware": "PSP_HARDWARE_READY",
    "future_work": "MAJOR_FUTURE_WORK",
}
FINDING_CODES = {
    "missing_routing",
    "stale_route",
    "missing_dependency",
    "dependency_cycle",
    "oracle_link_disagreement",
}


class RoutingManifestError(ValueError):
    """Raised when checked-in routing metadata cannot be trusted."""


def _read_json(source: Any, label: str) -> Any:
    if isinstance(source, (str, Path)):
        path = Path(source)
        try:
            return json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise RoutingManifestError(f"cannot read {label}: {error}") from error
    return source


def _issue_number(value: Any, label: str) -> int:
    if isinstance(value, bool):
        raise RoutingManifestError(f"{label} must be a positive integer")
    try:
        number = int(value)
    except (TypeError, ValueError) as error:
        raise RoutingManifestError(f"{label} must be a positive integer") from error
    if number < 1 or str(value).strip() != str(number):
        raise RoutingManifestError(f"{label} must be a positive integer")
    return number


def load_routing_manifest(source: Any = DEFAULT_ROUTING) -> dict[int, dict[str, Any]]:
    document = _read_json(source, "routing manifest")
    if not isinstance(document, dict):
        raise RoutingManifestError("routing manifest must be a JSON object")
    if document.get("schema_version") != 1:
        raise RoutingManifestError("routing manifest schema_version must be 1")
    raw_entries = document.get("entries")
    if raw_entries is None:
        raw_entries = document.get("issues", document.get("routes"))
    if not isinstance(raw_entries, list):
        raise RoutingManifestError("routing manifest entries must be a list")

    routes: dict[int, dict[str, Any]] = {}
    allowed = {
        "issue",
        "number",
        "primary_state",
        "state",
        "depends_on",
        "oracle_groups",
        "hardware_test_id",
        "local_test_command",
        "note",
    }
    for raw in raw_entries:
        if not isinstance(raw, dict):
            raise RoutingManifestError("routing manifest contains a malformed entry")
        unknown = set(raw) - allowed
        if unknown:
            raise RoutingManifestError("routing manifest entry has unsupported fields: " + ", ".join(sorted(unknown)))
        if "issue" not in raw and "number" not in raw:
            raise RoutingManifestError("routing manifest entry has no issue number")
        number = _issue_number(raw.get("issue", raw.get("number")), "routing issue")
        if number in routes:
            raise RoutingManifestError(f"duplicate routing issue {number}")
        state = raw.get("primary_state", raw.get("state"))
        if state not in ROUTING_STATES:
            raise RoutingManifestError(f"routing issue {number} has unsupported primary_state {state!r}")
        entry: dict[str, Any] = {"issue": number, "primary_state": state}

        depends_on = raw.get("depends_on", [])
        if not isinstance(depends_on, list):
            raise RoutingManifestError(f"routing issue {number} depends_on must be a list")
        normalized_dependencies: list[int] = []
        for dependency in depends_on:
            normalized = _issue_number(dependency, f"routing issue {number} dependency")
            if normalized in normalized_dependencies:
                raise RoutingManifestError(f"routing issue {number} repeats dependency {normalized}")
            normalized_dependencies.append(normalized)
        entry["depends_on"] = normalized_dependencies

        if "oracle_groups" in raw:
            groups = raw["oracle_groups"]
            if not isinstance(groups, list) or not all(isinstance(group, str) and group for group in groups):
                raise RoutingManifestError(f"routing issue {number} oracle_groups must be a list of strings")
            if len(set(groups)) != len(groups):
                raise RoutingManifestError(f"routing issue {number} repeats an oracle group")
            entry["oracle_groups"] = sorted(groups)

        for field in ("hardware_test_id", "local_test_command", "note"):
            if field in raw:
                value = raw[field]
                if not isinstance(value, str) or not value:
                    raise RoutingManifestError(f"routing issue {number} {field} must be a non-empty string")
                entry[field] = value
        routes[number] = entry
    return routes


def load_oracle_manifest(source: Any = DEFAULT_ORACLE_MANIFEST) -> dict[str, Any]:
    document = _read_json(source, "PSP oracle manifest")
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        raise RoutingManifestError("PSP oracle manifest must contain a tests list")
    return document


def derive_oracle_links(source: Any) -> tuple[dict[int, set[str]], dict[int, set[str]]]:
    document = source if isinstance(source, dict) else load_oracle_manifest(source)
    groups: dict[int, set[str]] = {}
    tests: dict[int, set[str]] = {}
    for test in document["tests"]:
        if not isinstance(test, dict):
            raise RoutingManifestError("PSP oracle manifest contains a malformed test")
        test_id = test.get("id")
        group = test.get("group")
        if not isinstance(test_id, str) or not test_id or not isinstance(group, str) or not group:
            raise RoutingManifestError("PSP oracle test requires id and group")
        issue_values = test.get("issues", [])
        if not isinstance(issue_values, list):
            raise RoutingManifestError(f"PSP oracle test {test_id} issues must be a list")
        for value in issue_values:
            number = _issue_number(value, f"PSP oracle test {test_id} issue")
            groups.setdefault(number, set()).add(group)
            tests.setdefault(number, set()).add(test_id)
    return groups, tests


def _coerce_routes(source: Any) -> dict[int, dict[str, Any]]:
    if source is None:
        return load_routing_manifest()
    if isinstance(source, (str, Path)):
        return load_routing_manifest(source)
    if isinstance(source, list):
        return load_routing_manifest({"schema_version": 1, "entries": source})
    if isinstance(source, Mapping) and not any(
        key in source for key in ("entries", "issues", "routes", "schema_version")
    ):
        routes: dict[int, dict[str, Any]] = {}
        for raw_number, raw_entry in source.items():
            number = _issue_number(raw_number, "routing issue")
            if not isinstance(raw_entry, Mapping):
                raise RoutingManifestError(f"routing issue {number} must be an object")
            entry = dict(raw_entry)
            entry["issue"] = number
            routes[number] = entry
        return load_routing_manifest({"schema_version": 1, "entries": list(routes.values())})
    return load_routing_manifest(source)


def _normalize_issues(issues: list[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    if not isinstance(issues, list):
        raise RoutingManifestError("issue input must be a JSON array")
    normalized: dict[int, dict[str, Any]] = {}
    for issue in issues:
        if not isinstance(issue, dict):
            raise RoutingManifestError("issue input contains a malformed issue")
        if "number" not in issue:
            raise RoutingManifestError("issue input contains an issue without a number")
        number = _issue_number(issue["number"], "issue number")
        if number in normalized:
            raise RoutingManifestError(f"issue input repeats issue {number}")
        item = dict(issue)
        item["number"] = number
        item["state"] = str(item.get("state") or "OPEN").upper()
        normalized[number] = item
    return normalized


def _milestone(issue: Mapping[str, Any]) -> dict[str, Any] | None:
    value = issue.get("milestone")
    if isinstance(value, str):
        return {"title": value} if value else None
    if not isinstance(value, Mapping) or not value:
        return None
    result: dict[str, Any] = {}
    for source, target in (("number", "number"), ("title", "title"), ("dueOn", "due_on")):
        if source in value:
            result[target] = value[source]
    return result or None


def _source_label(source: Any, default: str) -> str:
    if isinstance(source, (str, Path)):
        path = Path(source)
        try:
            return path.resolve().relative_to(ROOT.resolve()).as_posix()
        except OSError:
            return path.name
        except ValueError:
            return path.name
    return default


def _run_gh() -> list[dict[str, Any]]:
    command = [
        "gh",
        "issue",
        "list",
        "--state",
        "all",
        "--limit",
        "500",
        "--json",
        "number,title,body,state,milestone,labels,url,updatedAt",
    ]
    try:
        completed = subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8")
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"cannot query issues with gh: {type(exc).__name__}") from exc
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise SystemExit("gh issue list did not return valid JSON") from exc
    if not isinstance(result, list):
        raise SystemExit("gh issue list did not return a JSON array")
    return result


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


def _route(routes: Mapping[int, Mapping[str, Any]], number: int) -> Mapping[str, Any] | None:
    return routes.get(number)


def _state(number: int, routes: Mapping[int, Mapping[str, Any]] | None = None) -> str:
    active_routes = ROUTING if routes is None else routes
    entry = _route(active_routes, number)
    if entry is None:
        return MISSING_ROUTING_STATE
    return STATE_OUTPUT[entry["primary_state"]]


def _local_command(
    state: str,
    number: int,
    routes: Mapping[int, Mapping[str, Any]] | None = None,
) -> str:
    active_routes = ROUTING if routes is None else routes
    entry = _route(active_routes, number)
    if entry and entry.get("local_test_command"):
        return str(entry["local_test_command"])
    if state == "PSP_HARDWARE_READY":
        return "python tools/psp_readiness.py --json && python tools/psp_oracle/run_psplink.py --dry-run"
    if state == "LEGAL_HUMAN_BLOCKED":
        return "python tools/publish_audit.py --tracked-only"
    if state == "ENVIRONMENT_BLOCKED":
        return "python tools/psp_readiness.py --json"
    if state == MISSING_ROUTING_STATE:
        return "Add an explicit issue entry to assets/issue_routing.json, then rerun with --strict."
    return "python tools/psp_readiness.py --json"


def _finding(code: str, issue: int | None = None, detail: str = "", **extra: Any) -> dict[str, Any]:
    result: dict[str, Any] = {"code": code}
    if issue is not None:
        result["issue"] = issue
    if detail:
        result["detail"] = detail
    result.update(extra)
    return result


def _dependency_cycles(routes: Mapping[int, Mapping[str, Any]]) -> list[list[int]]:
    graph = {
        number: [dependency for dependency in entry.get("depends_on", []) if dependency in routes]
        for number, entry in routes.items()
    }
    index = 0
    indices: dict[int, int] = {}
    lowlinks: dict[int, int] = {}
    stack: list[int] = []
    on_stack: set[int] = set()
    components: list[list[int]] = []

    def visit(node: int) -> None:
        nonlocal index
        indices[node] = index
        lowlinks[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for neighbor in graph.get(node, []):
            if neighbor not in indices:
                visit(neighbor)
                lowlinks[node] = min(lowlinks[node], lowlinks[neighbor])
            elif neighbor in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[neighbor])
        if lowlinks[node] != indices[node]:
            return
        component: list[int] = []
        while stack:
            member = stack.pop()
            on_stack.remove(member)
            component.append(member)
            if member == node:
                break
        if len(component) > 1 or node in graph.get(node, []):
            components.append(sorted(component))

    for number in sorted(graph):
        if number not in indices:
            visit(number)
    return sorted(components)


def _validate_routing(
    issues: Mapping[int, Mapping[str, Any]],
    routes: Mapping[int, Mapping[str, Any]],
    oracle_groups: Mapping[int, set[str]],
    oracle_tests: Mapping[int, set[str]],
) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    open_numbers = {number for number, issue in issues.items() if issue.get("state") == "OPEN"}
    for number in sorted(open_numbers - set(routes)):
        findings.append(
            _finding(
                "missing_routing",
                number,
                "open issue has no entry in the checked-in routing manifest",
            )
        )
    for number in sorted(set(routes) - set(issues)):
        findings.append(
            _finding(
                "stale_route",
                number,
                "routing entry does not correspond to an issue in the supplied snapshot",
            )
        )
    for number, issue in sorted(issues.items()):
        if issue.get("state") != "OPEN" and number in routes:
            findings.append(
                _finding(
                    "stale_route",
                    number,
                    f"routing entry points to a {str(issue.get('state')).lower()} issue",
                )
            )

    for number, entry in sorted(routes.items()):
        for dependency in entry.get("depends_on", []):
            if dependency not in issues:
                findings.append(
                    _finding(
                        "missing_dependency",
                        number,
                        f"dependency {dependency} is not present in the issue snapshot",
                        dependency=dependency,
                    )
                )
        if "oracle_groups" in entry:
            declared = set(entry["oracle_groups"])
            derived = set(oracle_groups.get(number, set()))
            if declared != derived:
                findings.append(
                    _finding(
                        "oracle_link_disagreement",
                        number,
                        "routing oracle_groups disagrees with tools/psp_oracle/manifest.json",
                        declared=sorted(declared),
                        derived=sorted(derived),
                    )
                )
        hardware_id = entry.get("hardware_test_id")
        if hardware_id and hardware_id not in oracle_tests.get(number, set()):
            findings.append(
                _finding(
                    "oracle_link_disagreement",
                    number,
                    f"hardware_test_id {hardware_id} is not linked to this issue by the oracle manifest",
                    declared=[hardware_id],
                    derived=sorted(oracle_tests.get(number, set())),
                )
            )
    for component in _dependency_cycles(routes):
        findings.append(
            _finding(
                "dependency_cycle",
                component[0],
                "routing dependencies form a cycle: " + " -> ".join(map(str, component + [component[0]])),
                cycle=component,
            )
        )
    findings.sort(key=lambda item: (item["code"], item.get("issue", 0), item.get("detail", "")))
    return findings


def _issue_row(
    issue: Mapping[str, Any],
    routes: Mapping[int, Mapping[str, Any]],
    oracle_groups: Mapping[int, set[str]],
    oracle_tests: Mapping[int, set[str]],
) -> dict[str, Any]:
    number = int(issue["number"])
    entry = _route(routes, number)
    state = _state(number, routes)
    needs_psp = state == "PSP_HARDWARE_READY"
    hardware_id = entry.get("hardware_test_id") if entry else None
    groups = sorted(oracle_groups.get(number, set()))
    tests = sorted(oracle_tests.get(number, set()))
    milestone = _milestone(issue)
    version_target = milestone.get("title") if milestone else None
    if entry is None:
        implementation_state = (
            "Unrouted: add explicit readiness metadata before treating this issue as a product boundary."
        )
        fixture_scope = "unassigned; routing metadata is missing"
        required_fixture = "not assigned; add an explicit routing entry"
        independent_reference = "Inspect the canonical issue and add routing metadata."
    else:
        implementation_state = "Open; this snapshot is a routing aid, not closure evidence."
        fixture_scope = (
            "public/synthetic"
            if needs_psp
            else ("private" if state == "LOCAL_PRIVATE_ROUTE_READY" else "source-owned or canonical issue fixture")
        )
        required_fixture = (
            "fixtures/psp_oracle" if needs_psp else "source/tests/private route named by the canonical issue"
        )
        independent_reference = (
            "PPSSPP/PSPAutotests may corroborate but cannot replace PSP evidence."
            if needs_psp
            else "Use source/tests and the canonical issue acceptance criteria."
        )
    row: dict[str, Any] = {
        "issue": number,
        "title": issue.get("title", ""),
        "url": issue.get("url"),
        "updated_at": issue.get("updatedAt"),
        "issue_state": issue.get("state", "OPEN"),
        "milestone": milestone,
        "version_target": version_target,
        "primary_state": state,
        "routing_status": "routed" if entry is not None else "missing_routing",
        "routing_state": entry.get("primary_state") if entry else None,
        "routing_note": entry.get("note") if entry else None,
        "current_implementation_state": implementation_state,
        "smallest_unresolved_claim": _claim(issue.get("body") or ""),
        "local_test_command": _local_command(state, number, routes),
        "required_fixture": required_fixture,
        "fixture_scope": fixture_scope,
        "expected_observable": "Scalar result records must be deterministic; no pointers, retail bytes, or captures are acceptance evidence.",
        "independent_reference": independent_reference,
        "real_psp_evidence_needed": needs_psp,
        "hardware_test_id": hardware_id,
        "oracle_groups": groups,
        "oracle_test_ids": tests,
        "depends_on": list(entry.get("depends_on", [])) if entry else [],
        "dependency_issues": list(entry.get("depends_on", [])) if entry else [],
        "recommended_next_agent": "maintainer + physical PSP session"
        if needs_psp
        else "next scoped implementation/review agent",
        "closable_now": False,
    }
    return row


def build_matrix(
    issues: list[dict[str, Any]],
    *,
    generated_at: str,
    routing: Any = None,
    routing_manifest: Any = None,
    oracle_manifest: Any = None,
) -> dict[str, Any]:
    if routing is not None and routing_manifest is not None:
        raise RoutingManifestError("pass only one of routing and routing_manifest")
    route_source = routing_manifest if routing_manifest is not None else routing
    routes = _coerce_routes(route_source)
    oracle_source = oracle_manifest if oracle_manifest is not None else DEFAULT_ORACLE_MANIFEST
    oracle_document = load_oracle_manifest(oracle_source)
    oracle_groups, oracle_tests = derive_oracle_links(oracle_document)
    normalized_issues = _normalize_issues(issues)
    findings = _validate_routing(normalized_issues, routes, oracle_groups, oracle_tests)
    rows = [
        _issue_row(issue, routes, oracle_groups, oracle_tests)
        for number, issue in sorted(normalized_issues.items())
        if issue.get("state") == "OPEN"
    ]
    return {
        "schema": 2,
        "generated_at": generated_at,
        "source": "gh issue list --state all --limit 500",
        "repository": "Jstar269/nakagawa-recomp",
        "routing_source": _source_label(route_source, "assets/issue_routing.json"),
        "oracle_source": _source_label(oracle_source, "tools/psp_oracle/manifest.json"),
        "state_values": sorted(STATES | {MISSING_ROUTING_STATE}),
        "finding_codes": sorted({finding["code"] for finding in findings}),
        "findings": findings,
        "issue_count": len(rows),
        "rows": rows,
    }


def _load_issue_input(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return _run_gh()
    if str(path) == "-":
        payload = json.load(sys.stdin)
    else:
        try:
            payload = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise SystemExit(f"cannot read issue input: {error}") from error
    if isinstance(payload, dict):
        payload = payload.get("issues")
    if not isinstance(payload, list):
        raise SystemExit("issue input must be a JSON array")
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", type=Path, help="JSON from gh issue list; use - for stdin")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--routing", type=Path, default=DEFAULT_ROUTING)
    parser.add_argument("--oracle-manifest", type=Path, default=DEFAULT_ORACLE_MANIFEST)
    parser.add_argument("--strict", action="store_true", help="return nonzero when routing findings exist")
    args = parser.parse_args(argv)
    issues = _load_issue_input(args.input)
    try:
        matrix = build_matrix(
            issues,
            generated_at=datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
            routing_manifest=args.routing,
            oracle_manifest=args.oracle_manifest,
        )
    except RoutingManifestError as error:
        raise SystemExit(f"routing metadata error: {error}") from error
    try:
        matrix["main_sha"] = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8"
        ).stdout.strip()
    except OSError:
        matrix["main_sha"] = None
    except subprocess.CalledProcessError:
        matrix["main_sha"] = None
    if matrix["issue_count"] == 0:
        raise SystemExit("refusing to write an empty open-issue matrix")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for finding in matrix["findings"]:
        issue = f" issue {finding['issue']}" if "issue" in finding else ""
        print(f"{finding['code']}:{issue} {finding.get('detail', '')}", file=sys.stderr)
    print(f"psp_issue_matrix: {matrix['issue_count']} open issues -> {args.out}")
    return 1 if args.strict and matrix["findings"] else 0


ROUTING = load_routing_manifest()
ORACLE_MANIFEST = load_oracle_manifest()
ORACLE_GROUPS, ORACLE_TESTS = derive_oracle_links(ORACLE_MANIFEST)
LEGAL = {number for number, entry in ROUTING.items() if entry["primary_state"] == "legal"}
UPSTREAM = {number for number, entry in ROUTING.items() if entry["primary_state"] == "upstream"}
ENVIRONMENT = {number for number, entry in ROUTING.items() if entry["primary_state"] == "environment"}
PRIVATE_ROUTE = {number for number, entry in ROUTING.items() if entry["primary_state"] == "private_route"}
IMPLEMENTATION = {number for number, entry in ROUTING.items() if entry["primary_state"] == "implementation"}
LOCAL_ACCEPTANCE = {number for number, entry in ROUTING.items() if entry["primary_state"] == "local_acceptance"}
HARDWARE = {number for number, entry in ROUTING.items() if entry["primary_state"] == "hardware"}
HARDWARE_IDS = {number: entry["hardware_test_id"] for number, entry in ROUTING.items() if entry.get("hardware_test_id")}
DEPENDENCIES = {
    number: list(entry.get("depends_on", [])) for number, entry in ROUTING.items() if entry.get("depends_on")
}


if __name__ == "__main__":
    raise SystemExit(main())
