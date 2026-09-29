#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import argparse
import json
import re
import sys
from itertools import zip_longest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = ROOT / "assets" / "flight_recorder_schema.json"
CLASS_ORDER = ("hle", "unsupported", "sched", "prx", "media", "fault", "fatal")

# Verdict tokens of the comparison contract. MATCH is printed only when the two
# bundles are comparable and every compared field agrees; the full contract is
# documented on diff_bundles.
MATCH = "MATCH"
DIVERGENCE = "DIVERGENCE"
INCOMPARABLE = "INCOMPARABLE"

# Exit codes: 0 MATCH, 1 DIVERGENCE, 2 error/malformed bundle, 3 INCOMPARABLE.
EXIT_MATCH = 0
EXIT_DIVERGENCE = 1
EXIT_ERROR = 2
EXIT_INCOMPARABLE = 3


class FlightDiffError(ValueError):
    pass


class BundleIncomparable(FlightDiffError):
    """Two individually valid bundles are not evidence of comparable executions.

    Raised only for pairs whose recorder configuration, retained-history
    completeness, or terminal state would let "same retained events" stand in
    for "same execution" -- the failure mode this tool must never report as
    MATCH. ``reasons`` lists every violated comparability condition.
    """

    def __init__(self, reasons: list[str]) -> None:
        super().__init__("; ".join(reasons))
        self.reasons = list(reasons)


def _fail(path: str, message: str) -> None:
    raise FlightDiffError(f"{path}: {message}")


def _validate_schema(value: Any, schema: dict[str, Any], path: str = "$") -> None:
    if "const" in schema and value != schema["const"]:
        _fail(path, f"expected {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        _fail(path, f"expected one of {schema['enum']!r}")

    expected = schema.get("type")
    if expected is not None:
        expected_types = expected if isinstance(expected, list) else [expected]
        valid = False
        for type_name in expected_types:
            if type_name == "object" and isinstance(value, dict):
                valid = True
            elif type_name == "array" and isinstance(value, list):
                valid = True
            elif type_name == "string" and isinstance(value, str):
                valid = True
            elif type_name == "boolean" and isinstance(value, bool):
                valid = True
            elif type_name == "integer" and isinstance(value, int) and not isinstance(value, bool):
                valid = True
            elif type_name == "number" and isinstance(value, (int, float)) and not isinstance(value, bool):
                valid = True
            elif type_name == "null" and value is None:
                valid = True
        if not valid:
            _fail(path, f"expected {expected!r}")

    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            _fail(path, "string is too short")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            _fail(path, "string is too long")
        pattern = schema.get("pattern")
        if pattern is not None and re.search(pattern, value) is None:
            _fail(path, "string does not match its schema pattern")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            _fail(path, "number is below its minimum")
        if "maximum" in schema and value > schema["maximum"]:
            _fail(path, "number is above its maximum")
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            _fail(path, "array is too short")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            _fail(path, "array is too long")
        if schema.get("uniqueItems"):
            serialized = [json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value]
            if len(serialized) != len(set(serialized)):
                _fail(path, "array items are not unique")
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                _validate_schema(item, item_schema, f"{path}[{index}]")
    if isinstance(value, dict):
        for key in schema.get("required", ()):
            if key not in value:
                _fail(path, f"missing required property {key!r}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            for key in value:
                if key not in properties:
                    _fail(path, f"property {key!r} is not allowed")
        for key, child_schema in properties.items():
            if key in value:
                _validate_schema(value[key], child_schema, f"{path}.{key}")
    if "allOf" in schema:
        for child_schema in schema["allOf"]:
            _validate_schema(value, child_schema, path)
    if "not" in schema:
        try:
            _validate_schema(value, schema["not"], path)
        except FlightDiffError:
            pass
        else:
            _fail(path, "value matches a forbidden schema")
    if "if" in schema:
        try:
            _validate_schema(value, schema["if"], path)
        except FlightDiffError:
            branch = schema.get("else")
        else:
            branch = schema.get("then")
        if branch is not None:
            _validate_schema(value, branch, path)


def _forbid_embedded_text(value: Any, path: str = "$") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            lowered = key.casefold()
            if any(token in lowered for token in ("path", "payload", "memory", "guest", "text", "string")):
                _fail(path, f"source-unsafe property {key!r}")
            _forbid_embedded_text(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _forbid_embedded_text(child, f"{path}[{index}]")
    elif isinstance(value, str):
        if "C:\\" in value or "c:\\" in value or ".." in value or "/" in value or "\\" in value:
            _fail(path, "path-like or guest-derived text is not allowed")


def validate_bundle(bundle: Any, schema: dict[str, Any] | None = None) -> None:
    if schema is None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    # The schema also forbids an identity-less v3 build block; this check runs first so
    # the operator gets the specific reason rather than a generic schema mismatch.
    build = bundle.get("build") if isinstance(bundle, dict) else None
    version = bundle.get("schema_version") if isinstance(bundle, dict) else None
    if (isinstance(version, int) and not isinstance(version, bool) and version >= 3
            and isinstance(build, dict)
            and build.get("source_date_epoch") is None and build.get("build_id") is None):
        _fail("$.build", "bundle must record a reproducible build identity "
                         "(source_date_epoch or build_id); a clock stamp is never recorded")
    _validate_schema(bundle, schema)
    _forbid_embedded_text(bundle)
    version = bundle["schema_version"]
    if any(event["schema_version"] != version for event in bundle["events"]):
        _fail("$.events", "event schema versions must match the bundle")
    recorder = bundle["recorder"]
    events = bundle["events"]
    if recorder["recorded"] != len(events) + recorder["dropped"]:
        _fail("$.recorder", "recorded must equal retained plus dropped events")
    if len(events) > recorder["limit"]:
        _fail("$.events", "retained events exceed the configured limit")
    # The recorder assigns sequences 1..recorded and its ring retains the last
    # min(recorded, limit) of them; "dropped" is exactly the ring overflow. A
    # bundle that deviates from that window does not describe a capture this
    # tool can reason about, so it is malformed rather than merely divergent.
    if recorder["dropped"] != max(0, recorder["recorded"] - recorder["limit"]):
        _fail("$.recorder", "dropped must equal recorded minus min(recorded, limit)")
    sequences = [event["sequence"] for event in events]
    if sequences != sorted(set(sequences)):
        _fail("$.events", "sequence numbers must be strictly increasing")
    if sequences != list(range(recorder["dropped"] + 1, recorder["recorded"] + 1)):
        _fail("$.events", "retained sequences must be exactly dropped+1..recorded")
    enabled = set(recorder["enabled_classes"])
    if any(event["class"] not in enabled for event in events):
        _fail("$.events", "event class is not enabled")
    if bundle["terminal"]["sequence"] and bundle["terminal"]["sequence"] not in sequences:
        _fail("$.terminal", "terminal sequence is not retained")
    if recorder["triggers"]["fired"] and bundle["terminal"]["reason"] == "exit":
        _fail("$.terminal", "a triggered bundle cannot have an exit terminal reason")
    if not recorder["triggers"]["fired"] and bundle["terminal"]["reason"] != "exit":
        _fail("$.terminal", "an untriggered bundle must terminate at exit")


def load_bundle(path: Path) -> dict[str, Any]:
    try:
        bundle = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FlightDiffError(f"{path}: cannot read JSON: {exc}") from exc
    validate_bundle(bundle)
    return bundle


def _event_equal(left: dict[str, Any], right: dict[str, Any]) -> bool:
    return left == right


def _first_sequence_divergence(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> tuple[str, dict[str, Any] | None, dict[str, Any] | None] | None:
    left = {event["sequence"]: event for event in baseline["events"]}
    right = {event["sequence"]: event for event in candidate["events"]}
    for sequence in sorted(set(left) | set(right)):
        first = left.get(sequence)
        second = right.get(sequence)
        if first is None or second is None or not _event_equal(first, second):
            return f"sequence {sequence}", first, second
    return None


def _first_class_divergence(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> tuple[str, dict[str, Any] | None, dict[str, Any] | None] | None:
    candidates: list[tuple[int, str, dict[str, Any] | None, dict[str, Any] | None]] = []
    for event_class in CLASS_ORDER:
        left = [event for event in baseline["events"] if event["class"] == event_class]
        right = [event for event in candidate["events"] if event["class"] == event_class]
        for occurrence, (first, second) in enumerate(zip_longest(left, right)):
            if first is None or second is None or not _event_equal(first, second):
                order = max(
                    first["sequence"] if first is not None else 0,
                    second["sequence"] if second is not None else 0,
                )
                candidates.append((order, f"class {event_class} occurrence {occurrence}", first, second))
                break
    if not candidates:
        return None
    _, label, first, second = min(candidates, key=lambda item: (item[0], item[1]))
    return label, first, second


def _format_event(event: dict[str, Any] | None) -> str:
    return "<absent>" if event is None else json.dumps(event, sort_keys=True, separators=(",", ":"))


def _trigger_policy(recorder: dict[str, Any]) -> dict[str, Any]:
    """The armed trigger modes, as opposed to the ``fired`` outcome field."""
    return {key: value for key, value in recorder["triggers"].items() if key != "fired"}


def _comparability_reasons(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    """Every condition under which the pair must not be trusted as comparable."""
    reasons: list[str] = []
    if baseline["schema_version"] != candidate["schema_version"]:
        reasons.append(
            "bundle schema versions differ: "
            f"{baseline['schema_version']} vs {candidate['schema_version']}"
        )
    if baseline["runtime"] != candidate["runtime"]:
        reasons.append(f"runtime blocks differ: {baseline['runtime']} vs {candidate['runtime']}")
    if set(baseline["recorder"]["enabled_classes"]) != set(
        candidate["recorder"]["enabled_classes"]
    ):
        reasons.append(
            "enabled event classes differ: "
            f"{baseline['recorder']['enabled_classes']} vs "
            f"{candidate['recorder']['enabled_classes']}; a class one side "
            "could not record is silence, not agreement"
        )
    if _trigger_policy(baseline["recorder"]) != _trigger_policy(candidate["recorder"]):
        reasons.append(
            "trigger policies differ: "
            f"{_trigger_policy(baseline['recorder'])} vs "
            f"{_trigger_policy(candidate['recorder'])}; different capture-stop "
            "semantics do not bound the same retained window"
        )
    for name, bundle in (("baseline", baseline), ("candidate", candidate)):
        dropped = bundle["recorder"]["dropped"]
        if dropped:
            reasons.append(
                f"{name} dropped {dropped} events; the retained window slid, so "
                "an earlier divergence inside the dropped events is invisible"
            )
    return reasons


_TERMINAL_FIELDS = ("reason", "sequence", "kind", "arg0")


def _terminal_divergence(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> tuple[str, dict[str, Any] | None, dict[str, Any] | None] | None:
    """First differing terminal field; the runs' outcomes are evidence too."""
    left = baseline["terminal"]
    right = candidate["terminal"]
    for field in _TERMINAL_FIELDS:
        if left[field] != right[field]:
            return f"terminal {field}", left, right
    return None


def diff_bundles(
    baseline: dict[str, Any], candidate: dict[str, Any], align: str
) -> tuple[str, dict[str, Any] | None, dict[str, Any] | None] | None:
    """Compare two validated bundles under the comparability contract.

    Contract, in the order the checks run:

    1. Each bundle must pass ``validate_bundle`` (schema, sanitizer, and the
       recorder truncation invariants). A violation is an error (exit 2).
    2. The pair is comparable only when ``schema_version``, the ``runtime``
       block, the enabled event classes, and the armed trigger policy agree,
       and neither side dropped any event. Otherwise ``BundleIncomparable`` is
       raised (exit 3): "same retained events" is not evidence that the
       executions matched. A "running" terminal is compared like any other
       terminal value; it cannot hide events before the cut.
    3. Build identity (``build_id``, ``source_date_epoch``, ``compiler``,
       ``pointer_bits``) and the recorder limit never block a comparison:
       differing identities are the cross-build use case, and with zero drops
       on both sides the retained window is the complete execution regardless
       of each limit. The CLI reports identities on stdout.
    4. Events are compared first (sequence or class alignment), then the
       terminal reason/sequence/kind/arg0. Any difference is DIVERGENCE
       (exit 1); only full agreement prints MATCH (exit 0).
    """
    validate_bundle(baseline)
    validate_bundle(candidate)
    reasons = _comparability_reasons(baseline, candidate)
    if reasons:
        raise BundleIncomparable(reasons)
    if align == "sequence":
        divergence = _first_sequence_divergence(baseline, candidate)
    else:
        divergence = _first_class_divergence(baseline, candidate)
    if divergence is None:
        return _terminal_divergence(baseline, candidate)
    return divergence


def _identity_fields(bundle: dict[str, Any]) -> str:
    """One comparable identity line; identities are reported, never compared."""
    build = bundle["build"]
    fields = [f"compiler={build['compiler']}", f"pointer_bits={build['pointer_bits']}"]
    for key in ("build_id", "source_date_epoch", "compiled_date", "compiled_time"):
        if key in build:
            fields.append(f"{key}={build[key]}")
    return " ".join(fields)


def _enabled_classes(bundle: dict[str, Any]) -> str:
    """The recorder's class coverage, which is what bounds the verdict.

    Comparability already requires both sides to agree on this set, so one
    line describes a MATCH or DIVERGENCE; printing it is what keeps a MATCH
    honest about the channel it was actually able to observe.
    """
    classes = bundle["recorder"]["enabled_classes"]
    return ", ".join(classes) if classes else "<none>"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare two source-safe flight-recorder bundles")
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--align", choices=("sequence", "class"), default="sequence")
    args = parser.parse_args(argv)
    try:
        baseline = load_bundle(args.baseline)
        candidate = load_bundle(args.candidate)
        divergence = diff_bundles(baseline, candidate, args.align)
    except BundleIncomparable as exc:
        print(f"{INCOMPARABLE}: {exc.reasons[0]}")
        for reason in exc.reasons[1:]:
            print(f"  {reason}")
        return EXIT_INCOMPARABLE
    except FlightDiffError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    print(f"identity baseline: {_identity_fields(baseline)}")
    print(f"identity candidate: {_identity_fields(candidate)}")
    print(f"enabled classes: {_enabled_classes(baseline)}")
    if divergence is None:
        print(f"{MATCH}: {len(baseline['events'])} events aligned by {args.align}; "
              "agrees on these enabled classes and the terminal outcome")
        return EXIT_MATCH
    label, first, second = divergence
    print(f"{DIVERGENCE}: {label}")
    print(f"  baseline: {_format_event(first)}")
    print(f"  candidate: {_format_event(second)}")
    return EXIT_DIVERGENCE


if __name__ == "__main__":
    raise SystemExit(main())
