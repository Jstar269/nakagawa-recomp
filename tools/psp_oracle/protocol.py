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
# Tokens are compared after ``_placeholder_key`` folds case and drops separators,
# so "n/a", "Not-Measured" and "un measured" all match.  A label with no letter or
# digit ("?", "-") names nothing and is a placeholder too.
UNMEASURED_TOKENS = frozenset({
    "unknown", "unset", "placeholder", "none", "na", "tbd", "tbc", "unmeasured",
    "notmeasured", "unverified", "unavailable", "unspecified", "missing", "null", "nil",
    "empty", "todo", "fixme", "dummy", "fake",
})
_PLACEHOLDER_SEPARATORS_RE = re.compile(r"[\s._/-]+")


def _placeholder_key(value: str) -> str:
    return _PLACEHOLDER_SEPARATORS_RE.sub("", value.strip().lower())


def is_unmeasured(value: str) -> bool:
    """Return True when a provenance label is a placeholder, not a measurement."""
    key = _placeholder_key(value)
    return not any(ch.isalnum() for ch in key) or key in UNMEASURED_TOKENS
EXPECTED_SOURCE = {"psp": "psp", "nakagawa": "nakagawa"}
DMAC_SIZE_MATRIX_SIZES = (0xBFFF, 0xC000, 0xC001, 0xD000, 0xF000, 0xFFFF, 0x10000, 0x100000)
DMAC_SIZE_MATRIX_TRIALS = 3
DMAC_SIZE_MATRIX_ALIGNMENT = 0x1000
DMAC_SIZE_MATRIX_REDZONE = 0x1000
DMAC_SIZE_MATRIX_PARTITION = 2

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

PSP_MODEL_INTERPRETATION_RULE = "PSPSDK_PMODEL_ORDINAL_V1"


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


def model_identity_fields(
    physical_model_label: str | None,
    software_model_raw_value: str | int | None,
    *,
    rule_id: str = PSP_MODEL_INTERPRETATION_RULE,
) -> dict[str, str]:
    """Return separated model identity fields without normalizing the raw value.

    ``software_model_raw_value`` is copied into the returned field exactly as
    supplied (integer inputs use their decimal representation). An unknown rule
    or raw code affects only the derived family and agreement fields.
    """

    if software_model_raw_value is None:
        raw_value = "NOT_CAPTURED"
    elif isinstance(software_model_raw_value, bool):
        raw_value = str(software_model_raw_value)
    else:
        raw_value = str(software_model_raw_value)

    physical_label = physical_model_label or "NOT_CAPTURED"
    interpreted_family = "UNKNOWN"
    agreement = "UNKNOWN"
    if rule_id == PSP_MODEL_INTERPRETATION_RULE and raw_value != "NOT_CAPTURED":
        try:
            if re.fullmatch(r"0[xX][0-9a-fA-F]+", raw_value):
                raw_code = int(raw_value, 16)
            elif re.fullmatch(r"[0-9]+", raw_value):
                raw_code = int(raw_value, 10)
            else:
                raise ValueError("raw model value is not an integer")
            _generation, interpreted_family = decode_psp_model_code(raw_code)
        except ValueError:
            interpreted_family = "UNKNOWN"

        normalized_physical = re.sub(r"[^a-z0-9]", "", physical_label.lower())
        physical_family = next(
            (
                family
                for prefix, family in (
                    ("pspn1000", "PSP-N1000"),
                    ("pspe1000", "PSP-E1000"),
                    ("psp1000", "PSP-1000"),
                    ("psp2000", "PSP-2000"),
                    ("psp3000", "PSP-3000"),
                )
                if normalized_physical.startswith(prefix)
            ),
            None,
        )
        if interpreted_family != "UNKNOWN" and physical_family is not None:
            agreement = "AGREES" if physical_family == interpreted_family else "DISAGREES"

    return {
        "PHYSICAL_MODEL_LABEL": physical_label,
        "SOFTWARE_MODEL_RAW_VALUE": raw_value,
        "INTERPRETED_MODEL_FAMILY": interpreted_family,
        "MODEL_INTERPRETATION_RULE": rule_id,
        "MODEL_IDENTITY_AGREEMENT": agreement,
    }


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
    if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", metadata["source_commit"]):
        raise ProtocolError(
            f"line {line_number}: source_commit must be a full 40- or 64-digit git object id"
        )


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
        elif is_unmeasured(value):
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


def _validate_dmac_size_matrix_sizes(
    text: str, expected_sizes: tuple[int, ...], *, trials: int
) -> ParsedOutput:
    parsed = parse_output(text)
    expected = {
        ("memcpy" if api == 0 else "try", size): (api, size)
        for api in (0, 1)
        for size in expected_sizes
    }
    observed: set[tuple[str, int]] = set()
    for record in parsed.results:
        if record.test_id != "PSP-DMAC-001" or not record.case_id.startswith("size-matrix-"):
            raise ProtocolError("DMAC size matrix contains a non-matrix record")
        match = re.fullmatch(r"size-matrix-(memcpy|try)-0x([0-9a-f]{8})", record.case_id)
        if not match:
            raise ProtocolError(f"invalid DMAC matrix case_id {record.case_id!r}")
        api_name, raw_size = match.groups()
        key = api_name, int(raw_size, 16)
        if key not in expected or key in observed:
            raise ProtocolError(f"unexpected or duplicate DMAC matrix case {record.case_id!r}")
        observed.add(key)
        values = dict(record.values)
        required = {f"out{i}" for i in range(19)}
        if not required <= values.keys():
            raise ProtocolError(f"DMAC matrix case {record.case_id!r} is missing scalar fields")
        numbers = {key: int(value, 0) for key, value in values.items() if key.startswith("out")}
        api, size = expected[key]
        allocation_bytes = (
            size + 2 * DMAC_SIZE_MATRIX_REDZONE + DMAC_SIZE_MATRIX_ALIGNMENT - 1
        ) & ~(DMAC_SIZE_MATRIX_ALIGNMENT - 1)
        source_address = numbers["out15"]
        destination_address = numbers["out16"]
        source_block_uid = numbers["out17"]
        destination_block_uid = numbers["out18"]
        checks = {
            "requested size": (numbers["out0"], size),
            "copied prefix": (numbers["out1"], size),
            "sentinel mutation": (numbers["out2"], 0),
            "source integrity": (numbers["out3"], 1),
            "API": (numbers["out5"], api),
            "trial count": (numbers["out6"], trials),
            "failed trials": (numbers["out7"], 0),
            "source-guard mutation": (numbers["out8"], 0),
            "post-request guard mutation": (numbers["out9"], 0),
            "pre-request guard mutation": (numbers["out10"], 0),
            "transfer alignment": (numbers["out11"], DMAC_SIZE_MATRIX_ALIGNMENT),
            "allocation size": (numbers["out12"], allocation_bytes),
            "partition owner": (numbers["out13"], DMAC_SIZE_MATRIX_PARTITION),
            "redzone size": (numbers["out14"], DMAC_SIZE_MATRIX_REDZONE),
        }
        if record.status != "PASS":
            raise ProtocolError(f"DMAC matrix case {record.case_id!r} is {record.status}")
        for label, (actual, wanted) in checks.items():
            if actual != wanted:
                raise ProtocolError(
                    f"DMAC matrix case {record.case_id!r}: {label} {actual:#x} != {wanted:#x}"
                )
        if source_address % DMAC_SIZE_MATRIX_ALIGNMENT:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: source is misaligned")
        if destination_address % DMAC_SIZE_MATRIX_ALIGNMENT:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: destination is misaligned")
        if source_address < DMAC_SIZE_MATRIX_REDZONE or destination_address < DMAC_SIZE_MATRIX_REDZONE:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: allocation address underflows")
        source_block_start = source_address - DMAC_SIZE_MATRIX_REDZONE
        destination_block_start = destination_address - DMAC_SIZE_MATRIX_REDZONE
        source_block_end = source_block_start + allocation_bytes
        destination_block_end = destination_block_start + allocation_bytes
        if source_block_end > 0x100000000 or destination_block_end > 0x100000000:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: allocation address wraps")
        if source_block_start < destination_block_end and destination_block_start < source_block_end:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: allocated spans overlap")
        if source_block_uid <= 0 or destination_block_uid <= 0:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: allocation UID is invalid")
        if source_block_uid == destination_block_uid:
            raise ProtocolError(f"DMAC matrix case {record.case_id!r}: allocation UIDs are not distinct")
    if observed != set(expected):
        missing = sorted(set(expected) - observed)
        raise ProtocolError(f"DMAC size matrix is incomplete; missing {missing}")
    return parsed


def validate_dmac_size_matrix(
    text: str, *, trials: int = DMAC_SIZE_MATRIX_TRIALS
) -> ParsedOutput:
    """Validate all sizes and both APIs emitted by ``dma-size-matrix``."""

    return _validate_dmac_size_matrix_sizes(
        text, DMAC_SIZE_MATRIX_SIZES, trials=trials
    )


def validate_dmac_size_matrix_size(
    text: str, requested_size: int, *, trials: int = DMAC_SIZE_MATRIX_TRIALS
) -> ParsedOutput:
    """Validate both API cells for one isolated matrix-size session."""

    if requested_size not in DMAC_SIZE_MATRIX_SIZES:
        raise ProtocolError(f"unsupported DMAC matrix size {requested_size:#x}")
    return _validate_dmac_size_matrix_sizes(text, (requested_size,), trials=trials)


def dump_json(value: Any) -> str:
    """Canonical JSON used by the runner and tests."""

    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def _schema_type_matches(value: Any, expected: str) -> bool:
    if expected == "null":
        return value is None
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return False


#: Keywords the in-tree validator enforces, plus pure annotations. A schema using any
#: other keyword would be silently under-enforced here while a conforming validator
#: applied it, so the schema is refused instead (see _schema_keyword_errors).
_JSON_SCHEMA_ENFORCED = frozenset({
    "$ref", "type", "const", "enum", "required", "properties", "additionalProperties",
    "minItems", "maxItems", "uniqueItems", "items", "minLength", "maxLength",
    "pattern", "minimum", "maximum",
})
_JSON_SCHEMA_ANNOTATIONS = frozenset({
    "$schema", "$id", "$comment", "$defs", "title", "description",
})
GE_CORPUS_SCHEMA_ID = "https://github.com/Jstar269/nakagawa-recomp/raw/main/assets/ge_corpus.schema.json"


def _schema_keyword_errors(schema: Any, path: str = "") -> list[tuple[str, str]]:
    """Reject schema keywords the validator would silently ignore (typos included)."""

    if not isinstance(schema, dict):
        return []
    errors: list[tuple[str, str]] = []
    unknown = sorted(set(schema) - _JSON_SCHEMA_ENFORCED - _JSON_SCHEMA_ANNOTATIONS)
    if unknown:
        errors.append((path, "schema uses keyword(s) this validator does not enforce: " + ", ".join(unknown)))
    for name in ("properties", "$defs"):
        children = schema.get(name)
        if isinstance(children, dict):
            for key, child in children.items():
                errors.extend(_schema_keyword_errors(child, f"{path}/{name}/{key}"))
    if isinstance(schema.get("items"), dict):
        errors.extend(_schema_keyword_errors(schema["items"], f"{path}/items"))
    return errors


def _json_schema_errors(
    value: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    path: str = "",
) -> list[tuple[str, str]]:
    """Validate the JSON Schema keywords used by the checked-in GE contract."""

    errors: list[tuple[str, str]] = []
    reference = schema.get("$ref")
    if reference is not None:
        prefix = "#/$defs/"
        definitions = root_schema.get("$defs")
        name = reference[len(prefix):] if isinstance(reference, str) and reference.startswith(prefix) else None
        target = definitions.get(name) if isinstance(definitions, dict) and name else None
        if not isinstance(target, dict):
            return [(path, f"unsupported or missing schema reference {reference!r}")]
        return _json_schema_errors(value, target, root_schema, path)

    expected_type = schema.get("type")
    expected_types = expected_type if isinstance(expected_type, list) else [expected_type]
    expected_types = [item for item in expected_types if isinstance(item, str)]
    if expected_types and not any(_schema_type_matches(value, item) for item in expected_types):
        actual = "null" if value is None else type(value).__name__
        return [(path, f"must have type {' or '.join(expected_types)} (got {actual})")]

    if "const" in schema and (type(value) is not type(schema["const"]) or value != schema["const"]):
        errors.append((path, f"must equal {schema['const']!r}"))
    enum_values = schema.get("enum")
    if isinstance(enum_values, list) and not any(
        type(value) is type(candidate) and value == candidate for candidate in enum_values
    ):
        errors.append((path, f"must be one of {enum_values!r}"))

    if isinstance(value, dict):
        required = schema.get("required", [])
        if isinstance(required, list):
            for name in required:
                if isinstance(name, str) and name not in value:
                    errors.append((f"{path}.{name}" if path else name, "is required"))
        properties = schema.get("properties", {})
        if not isinstance(properties, dict):
            properties = {}
        for name, child in properties.items():
            if name in value and isinstance(child, dict):
                child_path = f"{path}.{name}" if path else name
                errors.extend(_json_schema_errors(value[name], child, root_schema, child_path))
        if schema.get("additionalProperties") is False:
            for name in sorted(set(value) - set(properties)):
                child_path = f"{path}.{name}" if path else name
                errors.append((child_path, "is not allowed by the schema"))
    elif isinstance(value, list):
        minimum = schema.get("minItems")
        maximum = schema.get("maxItems")
        if isinstance(minimum, int) and len(value) < minimum:
            errors.append((path, f"must contain at least {minimum} item(s)"))
        if isinstance(maximum, int) and len(value) > maximum:
            errors.append((path, f"must contain at most {maximum} item(s)"))
        if schema.get("uniqueItems") is True:
            serialized = [json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value]
            if len(serialized) != len(set(serialized)):
                errors.append((path, "items must be unique"))
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                errors.extend(_json_schema_errors(item, item_schema, root_schema, f"{path}[{index}]"))
    elif isinstance(value, str):
        minimum = schema.get("minLength")
        maximum = schema.get("maxLength")
        if isinstance(minimum, int) and len(value) < minimum:
            errors.append((path, f"must contain at least {minimum} character(s)"))
        if isinstance(maximum, int) and len(value) > maximum:
            errors.append((path, f"must contain at most {maximum} character(s)"))
        pattern = schema.get("pattern")
        if isinstance(pattern, str):
            try:
                matches = re.fullmatch(pattern, value) is not None
            except re.error:
                matches = False
                errors.append((path, "schema contains an invalid pattern"))
            else:
                if not matches:
                    errors.append((path, f"does not match required pattern {pattern!r}"))
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        if isinstance(minimum, (int, float)) and value < minimum:
            errors.append((path, f"must be at least {minimum}"))
        if isinstance(maximum, (int, float)) and value > maximum:
            errors.append((path, f"must be at most {maximum}"))
    return errors


def _ge_case_semantic_errors(case: dict[str, Any], path: str) -> list[tuple[str, str]]:
    errors: list[tuple[str, str]] = []
    framebuffer = case.get("framebuffer")
    pixels = case.get("selected_pixels")
    commands = case.get("command_words")
    relocations = case.get("relocations")
    if isinstance(framebuffer, dict):
        # A packed value must fit the declared pixel format: 16-bit formats cap at 0xFFFF.
        limit = {"5650": 0xFFFF, "5551": 0xFFFF, "4444": 0xFFFF, "8888": 0xFFFFFFFF}.get(
            framebuffer.get("format"))
        if limit is not None:
            initial = framebuffer.get("initial_pixel")
            if isinstance(initial, int) and not isinstance(initial, bool) and initial > limit:
                errors.append((f"{path}.framebuffer.initial_pixel",
                               f"does not fit the {framebuffer.get('format')} pixel format"))
            if isinstance(pixels, list):
                for index, pixel in enumerate(pixels):
                    value = pixel.get("pixel_value") if isinstance(pixel, dict) else None
                    if isinstance(value, int) and not isinstance(value, bool) and value > limit:
                        errors.append((f"{path}.selected_pixels[{index}].pixel_value",
                                       f"does not fit the {framebuffer.get('format')} pixel format"))
        width = framebuffer.get("width")
        height = framebuffer.get("height")
        stride = framebuffer.get("stride_pixels")
        if all(isinstance(item, int) and not isinstance(item, bool) for item in (width, height, stride)):
            if stride < width:
                errors.append((f"{path}.framebuffer.stride_pixels", "must be at least framebuffer.width"))
            if isinstance(pixels, list):
                seen_positions: set[tuple[int, int]] = set()
                for index, pixel in enumerate(pixels):
                    if not isinstance(pixel, dict):
                        continue
                    x, y = pixel.get("x"), pixel.get("y")
                    if not all(isinstance(item, int) and not isinstance(item, bool) for item in (x, y)):
                        continue
                    pixel_path = f"{path}.selected_pixels[{index}]"
                    if x >= width or y >= height:
                        errors.append((pixel_path, "coordinate is outside the framebuffer"))
                    if (x, y) in seen_positions:
                        errors.append((pixel_path, "coordinate is duplicated"))
                    seen_positions.add((x, y))
    vertices = case.get("vertex_words")
    if isinstance(commands, list) and isinstance(vertices, list):
        for index, command in enumerate(commands):
            if isinstance(command, str) and command[2:4] == "04":
                try:
                    count = int(command, 16) & 0xFFFF
                except ValueError:
                    continue
                if count == 0 or len(vertices) % count != 0:
                    errors.append((f"{path}.command_words[{index}]",
                                   f"PRIM vertex count {count} does not divide the "
                                   f"{len(vertices)} vertex_words"))
    if isinstance(commands, list) and isinstance(relocations, list):
        for index, relocation in enumerate(relocations):
            if not isinstance(relocation, dict):
                continue
            command_index = relocation.get("command_index")
            if not isinstance(command_index, int) or isinstance(command_index, bool):
                continue
            relocation_path = f"{path}.relocations[{index}]"
            if command_index >= len(commands):
                errors.append((relocation_path + ".command_index", "is outside command_words"))
                continue
            command = commands[command_index]
            opcode = {"BASE": "10", "VADDR": "01"}.get(relocation.get("part"))
            if isinstance(command, str) and opcode and command[2:4] != opcode:
                errors.append((relocation_path + ".part", f"does not point to GE command {opcode}"))
    return errors


def _ge_case_status(case: dict[str, Any]) -> tuple[str, str]:
    case_id = case["case_id"]
    source_tier = case["source_tier"]
    envelope = case["evidence_envelope"]
    outputs_present = bool(case["framebuffer_sha256"]) and all(
        pixel["pixel_value"] is not None for pixel in case["selected_pixels"]
    )
    if envelope is None:
        if source_tier == "PSP_HARDWARE":
            return "REFUSED", "case is labelled PSP_HARDWARE but has no evidence envelope."
        detail = "unbound output values are not evidence" if outputs_present else "no result is recorded"
        return "NOT_RUN", f"IN THE WORKS (#343): no PSP_HARDWARE evidence envelope is present; {detail}."

    envelope_class = envelope["EVIDENCE_CLASS"]
    hardware_claimed = source_tier == "PSP_HARDWARE" or envelope_class == "PSP_HARDWARE"
    if not hardware_claimed:
        if source_tier in {"LOCAL_COSIM", "PPSSPP_CORROBORATIVE"}:
            return "NOT_RUN", f"source tier {source_tier} has no PSP hardware raster record."
        return "REFUSED", "evidence envelope is present without a recognized source tier."
    if source_tier != "PSP_HARDWARE":
        return "REFUSED", "evidence envelope is labelled PSP_HARDWARE but the case source_tier disagrees."
    if envelope_class != "PSP_HARDWARE":
        return "REFUSED", "case source_tier is PSP_HARDWARE but EVIDENCE_CLASS is not PSP_HARDWARE."
    if envelope["ACCEPTANCE_ELIGIBLE"] is not True:
        return "REFUSED", "PSP_HARDWARE envelope is not acceptance-eligible."
    if not outputs_present:
        return "REFUSED", "accepted PSP_HARDWARE envelope has no complete pixel vector and framebuffer digest."

    try:
        parsed = parse_output(envelope["RAW_RESULT"])
    except ProtocolError as exc:
        return "REFUSED", f"raw result protocol is invalid: {exc}"
    metadata = parsed.metadata_dict()
    if metadata.get("source") != "psp":
        return "REFUSED", f"raw result source {metadata.get('source')!r} is not PSP hardware."
    if metadata.get("model") != envelope["CONSOLE_MODEL"]:
        return "REFUSED", "raw result model does not match CONSOLE_MODEL."
    if metadata.get("firmware") != envelope["FW"]:
        return "REFUSED", "raw result firmware does not match FW."
    if metadata.get("source_commit") != envelope["SOURCE_COMMIT"]:
        return "REFUSED", "raw result source_commit does not match SOURCE_COMMIT."
    if metadata.get("binary_sha256") != envelope["BINARY_SHA256"]:
        return "REFUSED", "raw result binary_sha256 does not match BINARY_SHA256."
    metadata_problems = provenance_issues(metadata)
    if metadata_problems:
        return "REFUSED", "raw result has unmeasured identity: " + "; ".join(metadata_problems)
    if envelope["CASE_ID"] != case_id:
        return "REFUSED", "oracle CASE_ID does not match the corpus case_id."
    records = [
        record for record in parsed.results
        if record.test_id == "PSP-GE-001" and record.case_id == case_id
    ]
    if len(records) != 1 or records[0].status != "PASS":
        return "REFUSED", "raw result lacks one passing PSP-GE-001 record for this case."
    result_values = dict(records[0].values)
    digest = case["framebuffer_sha256"]
    if result_values.get("framebuffer_sha256") != digest:
        return "REFUSED", "raw result framebuffer_sha256 does not match the case digest."
    for pixel in case["selected_pixels"]:
        key = f"pixel_{pixel['x']}_{pixel['y']}"
        observed = result_values.get(key)
        if not isinstance(observed, str) or not re.fullmatch(r"0x[0-9a-f]{8}", observed):
            return "REFUSED", f"raw result is missing a valid {key} selected-pixel value."
        if int(observed, 16) != pixel["pixel_value"]:
            return "REFUSED", f"raw result {key} does not match the case pixel vector."
    return "MEASURED", "acceptance-eligible PSP_HARDWARE record matches this case and its pixel results."


def ge_corpus_report(corpus: Any, schema: Any) -> dict[str, Any]:
    """Validate a GE corpus against its checked-in schema and classify each case."""

    boundary = "GE_RASTER_PIXEL_CONFORMANCE"
    issue = 343
    if not isinstance(schema, dict):
        return {
            "status": "REFUSED",
            "semantic_boundary": boundary,
            "tracking_issue": issue,
            "cases": [{"case_id": "<schema>", "status": "REFUSED", "reason": "schema document is not an object."}],
        }
    identity_errors: list[tuple[str, str]] = []
    schema_properties = schema.get("properties") if isinstance(schema.get("properties"), dict) else {}
    if (
        schema.get("$id") != GE_CORPUS_SCHEMA_ID
        or schema_properties.get("semantic_boundary", {}).get("const") != boundary
        or schema_properties.get("tracking_issue", {}).get("const") != issue
    ):
        identity_errors.append(("<schema>", "document is not the GE pixel corpus contract"))
    identity_errors.extend(_schema_keyword_errors(schema))
    if identity_errors:
        return {
            "status": "REFUSED",
            "semantic_boundary": boundary,
            "tracking_issue": issue,
            "cases": [{
                "case_id": "<schema>",
                "status": "REFUSED",
                "reason": "; ".join(f"{path}: {message}" for path, message in identity_errors),
            }],
        }
    schema_errors = _json_schema_errors(corpus, schema, schema)
    if not isinstance(corpus, dict):
        schema_errors.append(("", "corpus document must be an object"))
    cases = corpus.get("cases") if isinstance(corpus, dict) else None
    if not isinstance(cases, list):
        cases = []
    root_errors = [(path, reason) for path, reason in schema_errors if not path.startswith("cases[")]
    if root_errors:
        reason = "; ".join(
            f"{path or 'corpus'}: {message}" for path, message in root_errors
        )
        return {
            "status": "REFUSED",
            "semantic_boundary": boundary,
            "tracking_issue": issue,
            "cases": [{"case_id": "<corpus>", "status": "REFUSED", "reason": reason}],
        }

    case_reports: list[dict[str, str]] = []
    seen_case_ids: set[str] = set()
    for index, item in enumerate(cases):
        case_id = item.get("case_id") if isinstance(item, dict) else None
        report_id = case_id if isinstance(case_id, str) else f"case[{index}]"
        path_prefix = f"cases[{index}]"
        errors = [
            (path, reason) for path, reason in schema_errors if path.startswith(path_prefix)
        ]
        if isinstance(item, dict):
            errors.extend(_ge_case_semantic_errors(item, path_prefix))
        if isinstance(case_id, str):
            if case_id in seen_case_ids:
                errors.append((path_prefix + ".case_id", "duplicates an earlier case id"))
            seen_case_ids.add(case_id)
        if errors:
            reason = "; ".join(
                f"schema violation at {path}: {message}" for path, message in errors
            )
            case_reports.append({"case_id": report_id, "status": "REFUSED", "reason": reason})
        else:
            status, reason = _ge_case_status(item)
            case_reports.append({"case_id": report_id, "status": status, "reason": reason})
    report_status = "REFUSED" if any(item["status"] == "REFUSED" for item in case_reports) else "IN_THE_WORKS"
    return {
        "status": report_status,
        "semantic_boundary": boundary,
        "tracking_issue": issue,
        "cases": case_reports,
    }
