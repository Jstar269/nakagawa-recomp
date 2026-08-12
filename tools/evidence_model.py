#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed evidence grading and revision identity primitives.

This module does not inspect project artifacts itself. It provides one shared,
small contract for tools that report whether an observation is heuristic,
executed, revision-bound, content-validated, stale, or unknown.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from enum import IntEnum
import re
from typing import Any, Iterable, Mapping, Sequence

_SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_UTC_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z$")
_ALLOWED_IDENTITY_KEYS = {
    "source_commit",
    "binary_sha256",
    "profile_sha256",
    "input_manifest_sha256",
    "generated_at",
}
_ALLOWED_RECORD_KEYS = {
    "executed",
    "heuristic",
    "content_validated",
    "identity",
    "claim",
    "details",
}


class EvidenceError(ValueError):
    """Invalid or ambiguous evidence data."""


class EvidenceGrade(IntEnum):
    STALE = -1
    UNKNOWN = 0
    HEURISTIC = 1
    EXECUTED = 2
    FRESHNESS_BOUND = 3
    CONTENT_VALIDATED = 4


@dataclass(frozen=True)
class EvidenceIdentity:
    source_commit: str
    binary_sha256: str
    profile_sha256: str
    generated_at: str
    input_manifest_sha256: str | None = None

    def __post_init__(self) -> None:
        _require_match(self.source_commit, _SHA1_RE, "source_commit", "40 lowercase hexadecimal characters")
        _require_match(self.binary_sha256, _SHA256_RE, "binary_sha256", "64 lowercase hexadecimal characters")
        _require_match(self.profile_sha256, _SHA256_RE, "profile_sha256", "64 lowercase hexadecimal characters")
        if self.input_manifest_sha256 is not None:
            _require_match(
                self.input_manifest_sha256,
                _SHA256_RE,
                "input_manifest_sha256",
                "64 lowercase hexadecimal characters",
            )
        _parse_utc_timestamp(self.generated_at)

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "EvidenceIdentity":
        _reject_unknown(value, _ALLOWED_IDENTITY_KEYS, "identity")
        required = {"source_commit", "binary_sha256", "profile_sha256", "generated_at"}
        missing = sorted(required - set(value))
        if missing:
            raise EvidenceError(f"identity: missing required field(s): {', '.join(missing)}")
        for key in required | ({"input_manifest_sha256"} if "input_manifest_sha256" in value else set()):
            if value[key] is not None and not isinstance(value[key], str):
                raise EvidenceError(f"identity.{key}: must be a string")
        return cls(
            source_commit=value["source_commit"],
            binary_sha256=value["binary_sha256"],
            profile_sha256=value["profile_sha256"],
            generated_at=value["generated_at"],
            input_manifest_sha256=value.get("input_manifest_sha256"),
        )

    def matches(self, expected: "EvidenceIdentity") -> bool:
        return (
            self.source_commit == expected.source_commit
            and self.binary_sha256 == expected.binary_sha256
            and self.profile_sha256 == expected.profile_sha256
            and self.input_manifest_sha256 == expected.input_manifest_sha256
            and self.generated_at == expected.generated_at
        )


@dataclass(frozen=True)
class EvidenceRecord:
    claim: str
    executed: bool = False
    heuristic: bool = False
    content_validated: bool = False
    identity: EvidenceIdentity | None = None
    details: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.claim, str) or not self.claim.strip() or len(self.claim) > 160:
            raise EvidenceError("claim: must be a non-empty string of at most 160 characters")
        if self.claim != self.claim.strip():
            raise EvidenceError("claim: must not have leading or trailing whitespace")
        if any(type(value) is not bool for value in (self.executed, self.heuristic, self.content_validated)):
            raise EvidenceError("executed, heuristic, and content_validated must be booleans")
        if not isinstance(self.details, str) or len(self.details) > 2048:
            raise EvidenceError("details: must be a string of at most 2048 characters")
        if self.content_validated and not self.executed:
            raise EvidenceError("content_validated requires executed evidence")
        if self.identity is not None and not self.executed:
            raise EvidenceError("identity-bound evidence must be executed")
        if self.heuristic and (self.executed or self.content_validated or self.identity is not None):
            raise EvidenceError("heuristic evidence cannot simultaneously claim execution or identity")

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "EvidenceRecord":
        _reject_unknown(value, _ALLOWED_RECORD_KEYS, "record")
        if "claim" not in value:
            raise EvidenceError("record: missing required field: claim")
        identity_value = value.get("identity")
        if identity_value is not None and not isinstance(identity_value, Mapping):
            raise EvidenceError("record.identity: must be an object or null")
        return cls(
            claim=value["claim"],
            executed=value.get("executed", False),
            heuristic=value.get("heuristic", False),
            content_validated=value.get("content_validated", False),
            identity=EvidenceIdentity.from_mapping(identity_value) if identity_value is not None else None,
            details=value.get("details", ""),
        )


def grade(record: EvidenceRecord, expected: EvidenceIdentity | None = None) -> EvidenceGrade:
    """Derive the strongest justified grade; identity mismatch is explicitly STALE."""

    if record.heuristic:
        return EvidenceGrade.HEURISTIC
    if not record.executed:
        return EvidenceGrade.UNKNOWN
    if record.identity is None:
        return EvidenceGrade.EXECUTED
    if expected is None:
        return EvidenceGrade.UNKNOWN
    if not record.identity.matches(expected):
        return EvidenceGrade.STALE
    if record.content_validated:
        return EvidenceGrade.CONTENT_VALIDATED
    return EvidenceGrade.FRESHNESS_BOUND


def satisfies(
    record: EvidenceRecord,
    expected: EvidenceIdentity,
    required: EvidenceGrade = EvidenceGrade.CONTENT_VALIDATED,
) -> bool:
    """Return whether evidence meets an explicit minimum grade."""

    if required < EvidenceGrade.EXECUTED:
        raise EvidenceError("completion requirements must be EXECUTED or stronger")
    return grade(record, expected) >= required


def completion_units(
    records: Iterable[EvidenceRecord],
    expected: EvidenceIdentity,
    required: EvidenceGrade = EvidenceGrade.CONTENT_VALIDATED,
) -> int:
    """Count only records meeting the explicit grade; stale/proxy evidence earns zero."""

    credited_claims = {
        record.claim for record in records
        if satisfies(record, expected, required)
    }
    return len(credited_claims)


def milestones_in_order(observed: Sequence[str], required: Sequence[str]) -> bool:
    """Require the milestone sequence as an ordered subsequence, not set membership."""

    if len(set(required)) != len(required):
        raise EvidenceError("required milestones must be unique")
    index = 0
    for milestone in observed:
        if index < len(required) and milestone == required[index]:
            index += 1
    return index == len(required)


def absence_observed(
    record: EvidenceRecord,
    expected: EvidenceIdentity,
    *,
    route_exercised: bool,
    required_grade: EvidenceGrade = EvidenceGrade.FRESHNESS_BOUND,
) -> bool:
    """Permit absence claims only when the relevant route ran on bound evidence."""

    if type(route_exercised) is not bool:
        raise EvidenceError("route_exercised must be a boolean")
    return route_exercised and satisfies(record, expected, required_grade)


def _reject_unknown(value: Mapping[str, Any], allowed: set[str], path: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise EvidenceError(f"{path}: unknown field(s): {', '.join(unknown)}")


def _require_match(value: str, pattern: re.Pattern[str], path: str, description: str) -> None:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise EvidenceError(f"{path}: must contain {description}")


def _parse_utc_timestamp(value: str) -> datetime:
    if not isinstance(value, str) or not _UTC_RE.fullmatch(value):
        raise EvidenceError("generated_at: must be an RFC 3339 UTC timestamp ending in Z")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise EvidenceError("generated_at: invalid RFC 3339 timestamp") from exc
    if parsed.tzinfo != timezone.utc:
        raise EvidenceError("generated_at: must be UTC")
    return parsed
