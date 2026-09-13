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

# PSPSDK's ``enum PspModel`` is an ordinal generation value, not a retail
# model number.  In particular, ordinal 3 means generation 04g, which belongs
# to the PSP-3000 family; PSP-N1000 is generation 05g (ordinal 4).  Keep this
# table separate from the kernel-only ``sceKernelGetModel`` API, whose public
# header documents a different original/slim return convention.
PSP_MODEL_CODE_TO_GENERATION = {
    0: "01g",
    1: "02g",
    2: "03g",
    3: "04g",
    4: "05g",
    5: "07g",
    6: "09g",
    7: "11g",
}
PSP_GENERATION_TO_RETAIL = {
    "01g": "PSP-1000",
    "02g": "PSP-2000",
    "03g": "PSP-3000",
    "04g": "PSP-3000",
    "05g": "PSP-N1000",
    "07g": "PSP-3000",
    "09g": "PSP-3000",
    "11g": "PSP-E1000",
}


def decode_psp_model_code(raw_code: int) -> tuple[str, str]:
    """Decode a PSPSDK ``PspModel`` ordinal into generation and retail family.

    The caller must know that the value came from the PSPSDK/kubridge model
    enum.  This function intentionally rejects unknown ordinals instead of
    guessing a retail identity or applying the table to ``sceKernelGetModel``.
    """

    if isinstance(raw_code, bool) or not isinstance(raw_code, int):
        raise ValueError("PSPSDK model code must be an integer")
    try:
        generation = PSP_MODEL_CODE_TO_GENERATION[raw_code]
    except KeyError as exc:
        raise ValueError(f"unknown PSPSDK model code {raw_code}") from exc
    return generation, PSP_GENERATION_TO_RETAIL[generation]


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
    return ParsedOutput(tuple(sorted(metadata.items())), tuple(results))


@dataclass(frozen=True)
class StreamSpec:
    """The complete record contract of one source-owned probe stream.

    ``semantic_cases`` are the measurement records in emission order.  The
    terminal record is held separately and is never a measurement: it is the
    probe's own claim about how many semantic records it believes it emitted,
    which makes a truncated transport detectable against the stream itself.
    """

    test_id: str
    semantic_cases: tuple[str, ...]
    terminal_case: str

    @property
    def ordered_cases(self) -> tuple[str, ...]:
        return self.semantic_cases + (self.terminal_case,)

    @property
    def terminal_count(self) -> int:
        return len(self.semantic_cases)

    @property
    def record_count(self) -> int:
        return len(self.semantic_cases) + 1


@dataclass(frozen=True)
class SequenceReport:
    """The verdict of one channel, parsed alone.

    ``complete`` is a property of the observed record sequence and never of the
    caller's strictness flag.  ``all_passed`` is gated on ``complete`` in both
    modes, so a partial stream can never report a semantic pass.
    ``terminal_present`` is reported separately and never substitutes for
    completeness -- a probe that emits its terminal record after losing middle
    records is exactly the shape the transport defect produces.
    """

    spec: StreamSpec
    raw_record_count: int
    record_count: int
    observed_order: tuple[str, ...]
    complete: bool
    terminal_present: bool
    all_passed: bool
    results: dict[str, TestResult]
    metadata: dict[str, str]
    provenance: tuple[str, ...]


def parse_sequence(
    text: str, spec: StreamSpec, *, require_complete: bool = True
) -> SequenceReport:
    """Parse one channel of a probe stream against its exact record contract.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    permits *inspection* of an ordered prefix so a truncated capture can still
    be classified, and relaxes nothing else: unknown case IDs, foreign
    ``test_id`` values, duplicates, reordering, records after the terminal and
    a terminal whose encoded count disagrees with the protocol are rejected in
    both modes, because none of those shapes is a truncation.

    Stale-log contamination is rejected mechanically rather than by inspection.
    A second run appended to the same log carries a second ``NAKAGAWA_PSP_META``
    record (duplicate metadata field) and repeats every ``case_id`` (duplicate
    test case); both are hard errors in :func:`parse_output` and here.
    """

    parsed = parse_output(text)
    expected_index = {case: index for index, case in enumerate(spec.ordered_cases)}

    ordered: list[TestResult] = []
    seen: set[str] = set()
    for record in parsed.results:
        if record.test_id != spec.test_id:
            raise ProtocolError(
                f"{spec.test_id}: stream carries a foreign test_id {record.test_id!r} "
                f"(case {record.case_id!r})"
            )
        if record.case_id not in expected_index:
            raise ProtocolError(
                f"{spec.test_id}: unknown case_id {record.case_id!r}"
            )
        if record.case_id in seen:
            raise ProtocolError(f"{spec.test_id}: duplicate case {record.case_id!r}")
        seen.add(record.case_id)
        ordered.append(record)

    observed = tuple(record.case_id for record in ordered)
    results = {record.case_id: record for record in ordered}

    # Nothing may follow the terminal record.  Without this an appended stale
    # fragment carrying only new case IDs would be read as a longer stream.
    if spec.terminal_case in observed and observed[-1] != spec.terminal_case:
        trailing = observed[observed.index(spec.terminal_case) + 1 :]
        raise ProtocolError(
            f"{spec.test_id}: {len(trailing)} record(s) follow the terminal "
            f"{spec.terminal_case!r}: {list(trailing)}"
        )

    complete = observed == spec.ordered_cases
    if require_complete and not complete:
        missing = [case for case in spec.ordered_cases if case not in results]
        if missing:
            raise ProtocolError(
                f"{spec.test_id}: stream incomplete, missing {len(missing)} "
                f"record(s): {missing}"
            )
        raise ProtocolError(
            f"{spec.test_id}: records out of order: expected "
            f"{list(spec.ordered_cases)}, got {list(observed)}"
        )
    if not complete:
        indices = [expected_index[case] for case in observed]
        if any(indices[i] > indices[i + 1] for i in range(len(indices) - 1)):
            raise ProtocolError(
                f"{spec.test_id}: records out of order: {list(observed)}"
            )

    terminal_present = spec.terminal_case in results
    if terminal_present:
        values = dict(results[spec.terminal_case].values)
        if "out0" not in values:
            raise ProtocolError(
                f"{spec.test_id}: terminal record {spec.terminal_case!r} is missing "
                "its out0 completion count"
            )
        try:
            claimed = int(values["out0"], 0)
        except ValueError as exc:
            raise ProtocolError(
                f"{spec.test_id}: terminal count is not an integer: {values['out0']!r}"
            ) from exc
        if claimed != spec.terminal_count:
            raise ProtocolError(
                f"{spec.test_id}: terminal count mismatch: {spec.terminal_case} claims "
                f"{claimed} semantic records, protocol expects {spec.terminal_count}"
            )
        observed_semantic = len([c for c in observed if c != spec.terminal_case])
        if complete and observed_semantic != claimed:
            raise ProtocolError(
                f"{spec.test_id}: terminal count mismatch: {spec.terminal_case} claims "
                f"{claimed} semantic records, stream carries {observed_semantic}"
            )

    metadata = parsed.metadata_dict()
    return SequenceReport(
        spec=spec,
        raw_record_count=len(parsed.results),
        record_count=len(ordered),
        observed_order=observed,
        complete=complete,
        terminal_present=terminal_present,
        all_passed=complete and all(r.status == "PASS" for r in ordered),
        results=results,
        metadata=metadata,
        provenance=provenance_issues(metadata),
    )


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
