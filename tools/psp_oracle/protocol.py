# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Strict, line-oriented result protocol for source-owned PSP probes.

The protocol deliberately carries scalar values only.  It does not accept
pointers, raw memory dumps, screenshots, or retail/game-derived payloads.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
from typing import Any, Iterable


SCHEMA = 1
META_PREFIX = "NAKAGAWA_PSP_META"
TEST_PREFIX = "NAKAGAWA_PSP_TEST"
STATUSES = frozenset({"PASS", "FAIL", "SKIP", "HANG", "TIMEOUT", "ERROR"})
_KEY_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]{0,63}$")
_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,96}$")
_HEX_RE = re.compile(r"^0x[0-9a-fA-F]{1,16}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

# The checked-in fixture deliberately emits placeholder provenance so that it can
# be built and run before a PSP is attached.  Placeholders are syntactically
# valid, so the parser accepts them; they must never be promoted into acceptance
# evidence.  ``provenance_issues`` is the programmatic form of that boundary.
_ALL_ZERO_RE = re.compile(r"^0+$")
UNMEASURED_TOKENS = frozenset({"unknown", "unset", "placeholder", "none", "n/a", "na", "tbd"})
EXPECTED_SOURCE = {"psp": "psp", "nakagawa": "nakagawa"}


class ProtocolError(ValueError):
    """A result stream violates the source-owned protocol."""


@dataclass(frozen=True)
class TestResult:
    test_id: str
    case_id: str
    status: str
    values: tuple[tuple[str, str], ...]

    def key(self) -> tuple[str, str]:
        return self.test_id, self.case_id

    def as_dict(self) -> dict[str, Any]:
        return {
            "test_id": self.test_id,
            "case_id": self.case_id,
            "status": self.status,
            "values": dict(self.values),
        }


@dataclass(frozen=True)
class ParsedOutput:
    metadata: tuple[tuple[str, str], ...]
    results: tuple[TestResult, ...]

    def metadata_dict(self) -> dict[str, str]:
        return dict(self.metadata)


def _fields(tokens: Iterable[str], *, line_number: int) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            raise ProtocolError(f"line {line_number}: field is not key=value")
        key, value = token.split("=", 1)
        if not _KEY_RE.fullmatch(key) or not value:
            raise ProtocolError(f"line {line_number}: invalid field {token!r}")
        if key in fields:
            raise ProtocolError(f"line {line_number}: duplicate field {key}")
        if any(ord(char) < 0x20 or char.isspace() for char in value):
            raise ProtocolError(f"line {line_number}: whitespace/control in {key}")
        fields[key] = value
    return fields


def _validate_metadata(metadata: dict[str, str], *, line_number: int) -> None:
    required = {"schema", "source", "model", "firmware", "binary_sha256", "source_commit"}
    missing = sorted(required - metadata.keys())
    if missing:
        raise ProtocolError(
            f"line {line_number}: metadata missing required fields: {', '.join(missing)}"
        )
    if metadata["schema"] != str(SCHEMA):
        raise ProtocolError(f"line {line_number}: unsupported schema {metadata['schema']!r}")
    if metadata["source"] not in {"psp", "nakagawa", "ppsspp"}:
        raise ProtocolError(f"line {line_number}: unsupported source {metadata['source']!r}")
    if not _SHA256_RE.fullmatch(metadata["binary_sha256"]):
        raise ProtocolError(f"line {line_number}: binary_sha256 must be lowercase SHA-256")
    if not re.fullmatch(r"[0-9a-f]{40,64}", metadata["source_commit"]):
        raise ProtocolError(f"line {line_number}: source_commit must be a git object id")


def provenance_issues(metadata: dict[str, str]) -> tuple[str, ...]:
    """Report why a stream's metadata is not measured hardware provenance.

    An empty tuple means every provenance field carries a host-measured value.
    A non-empty tuple means the stream still carries fixture placeholders and
    must not be recorded as acceptance evidence for any issue.
    """

    problems: list[str] = []
    for field in ("model", "firmware"):
        value = metadata.get(field)
        if value is None:
            problems.append(f"{field} is absent")
        elif value.strip().lower() in UNMEASURED_TOKENS:
            problems.append(f"{field} is the fixture placeholder {value!r}")
    digest = metadata.get("binary_sha256")
    if digest is None:
        problems.append("binary_sha256 is absent")
    elif _ALL_ZERO_RE.fullmatch(digest):
        problems.append("binary_sha256 is the all-zero fixture placeholder")
    commit = metadata.get("source_commit")
    if commit is None:
        problems.append("source_commit is absent")
    elif _ALL_ZERO_RE.fullmatch(commit):
        problems.append("source_commit is the all-zero fixture placeholder")
    return tuple(problems)


def parse_output(text: str, *, require_metadata: bool = True) -> ParsedOutput:
    """Parse a complete deterministic result stream.

    Blank lines and ``#`` comments are ignored.  Duplicate metadata/result
    keys, malformed scalar fields, and duplicate test cases are rejected.
    """

    metadata: dict[str, str] = {}
    results: list[TestResult] = []
    seen_results: set[tuple[str, str]] = set()
    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        prefix = tokens.pop(0)
        if prefix == META_PREFIX:
            fields = _fields(tokens, line_number=line_number)
            if set(fields) & set(metadata):
                duplicate = sorted(set(fields) & set(metadata))[0]
                raise ProtocolError(f"line {line_number}: duplicate metadata field {duplicate}")
            metadata.update(fields)
            continue
        if prefix != TEST_PREFIX:
            raise ProtocolError(f"line {line_number}: unknown record prefix {prefix!r}")
        fields = _fields(tokens, line_number=line_number)
        required = {"schema", "test_id", "case_id", "status"}
        missing = sorted(required - fields.keys())
        if missing:
            raise ProtocolError(
                f"line {line_number}: test record missing fields: {', '.join(missing)}"
            )
        if fields["schema"] != str(SCHEMA):
            raise ProtocolError(f"line {line_number}: unsupported test schema")
        for field in ("test_id", "case_id"):
            if not _ID_RE.fullmatch(fields[field]):
                raise ProtocolError(f"line {line_number}: invalid {field}")
        if fields["status"] not in STATUSES:
            raise ProtocolError(f"line {line_number}: invalid status {fields['status']!r}")
        key = fields["test_id"], fields["case_id"]
        if key in seen_results:
            raise ProtocolError(f"line {line_number}: duplicate test case {key!r}")
        seen_results.add(key)
        values = {
            key: value
            for key, value in fields.items()
            if key not in {"schema", "test_id", "case_id", "status"}
        }
        for key, value in values.items():
            if key.startswith("out") or key in {"result", "error", "detail"}:
                if key != "error" and key != "detail" and not _HEX_RE.fullmatch(value):
                    raise ProtocolError(f"line {line_number}: {key} must be hexadecimal")
        results.append(
            TestResult(
                test_id=fields["test_id"],
                case_id=fields["case_id"],
                status=fields["status"],
                values=tuple(sorted(values.items())),
            )
        )
    if require_metadata:
        _validate_metadata(metadata, line_number=0)
    if not results:
        raise ProtocolError("result stream contains no test records")
    return ParsedOutput(tuple(sorted(metadata.items())), tuple(sorted(results, key=TestResult.key)))


def compare_outputs(psp: ParsedOutput, nakagawa: ParsedOutput) -> dict[str, Any]:
    """Compare two parsed streams without assigning causality to a difference."""

    psp_results = {result.key(): result for result in psp.results}
    nak_results = {result.key(): result for result in nakagawa.results}
    comparisons: list[dict[str, Any]] = []
    for key in sorted(set(psp_results) | set(nak_results)):
        psp_result = psp_results.get(key)
        nak_result = nak_results.get(key)
        if psp_result is None:
            status = "NAKAGAWA_ONLY"
        elif nak_result is None:
            status = "PSP_ONLY"
        elif psp_result == nak_result:
            status = "MATCH"
        else:
            status = "DIFFERENCE"
        comparisons.append(
            {
                "test_id": key[0],
                "case_id": key[1],
                "comparison": status,
                "psp": psp_result.as_dict() if psp_result else None,
                "nakagawa": nak_result.as_dict() if nak_result else None,
            }
        )
    classifications = {item["comparison"] for item in comparisons}
    if classifications == {"MATCH"}:
        classification = "MATCH"
    elif "DIFFERENCE" in classifications:
        classification = "DIFFERENCE"
    elif classifications == {"PSP_ONLY"}:
        classification = "PSP_ONLY"
    elif classifications == {"NAKAGAWA_ONLY"}:
        classification = "NAKAGAWA_ONLY"
    else:
        classification = "INCONCLUSIVE"
    psp_metadata = dict(psp.metadata)
    nakagawa_metadata = dict(nakagawa.metadata)
    blockers: list[str] = []
    for role, metadata in (("psp", psp_metadata), ("nakagawa", nakagawa_metadata)):
        actual = metadata.get("source")
        if actual != EXPECTED_SOURCE[role]:
            blockers.append(f"{role}: stream declares source={actual!r}, not {EXPECTED_SOURCE[role]!r}")
        blockers.extend(f"{role}: {problem}" for problem in provenance_issues(metadata))
    return {
        "schema": SCHEMA,
        "classification": classification,
        # A comparison is a fact; acceptance evidence additionally requires
        # measured provenance on both sides.  See docs/HARDWARE_ORACLE.md.
        "acceptance_eligible": not blockers,
        "acceptance_blockers": blockers,
        "psp_metadata": psp_metadata,
        "nakagawa_metadata": nakagawa_metadata,
        "comparisons": comparisons,
    }


def compare_texts(psp_text: str, nakagawa_text: str) -> dict[str, Any]:
    """Parse and compare streams, retaining parse failures as INCONCLUSIVE."""

    try:
        psp = parse_output(psp_text)
        nakagawa = parse_output(nakagawa_text)
    except ProtocolError as exc:
        return {
            "schema": SCHEMA,
            "classification": "INCONCLUSIVE",
            "acceptance_eligible": False,
            "acceptance_blockers": [f"result stream did not parse: {exc}"],
            "error": str(exc),
        }
    return compare_outputs(psp, nakagawa)


def dump_json(value: Any) -> str:
    """Canonical JSON used by the runner and tests."""

    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
