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
CLASS_ORDER = ("hle", "unsupported", "sched", "prx", "fault", "fatal")


class FlightDiffError(ValueError):
    pass


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
    _validate_schema(bundle, schema)
    _forbid_embedded_text(bundle)
    version = bundle["schema_version"]
    if any(event["schema_version"] != version for event in bundle["events"]):
        _fail("$.events", "event schema versions must match the bundle")
    build = bundle["build"]
    if (version >= 3 and build.get("source_date_epoch") is None
            and build.get("build_id") is None):
        _fail("$.build", "bundle must record a reproducible build identity "
                         "(source_date_epoch or build_id); a clock stamp is never recorded")
    recorder = bundle["recorder"]
    events = bundle["events"]
    if recorder["recorded"] != len(events) + recorder["dropped"]:
        _fail("$.recorder", "recorded must equal retained plus dropped events")
    if len(events) > recorder["limit"]:
        _fail("$.events", "retained events exceed the configured limit")
    sequences = [event["sequence"] for event in events]
    if sequences != sorted(set(sequences)):
        _fail("$.events", "sequence numbers must be strictly increasing")
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


def diff_bundles(
    baseline: dict[str, Any], candidate: dict[str, Any], align: str
) -> tuple[str, dict[str, Any] | None, dict[str, Any] | None] | None:
    validate_bundle(baseline)
    validate_bundle(candidate)
    if align == "sequence":
        return _first_sequence_divergence(baseline, candidate)
    return _first_class_divergence(baseline, candidate)


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
    except FlightDiffError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if divergence is None:
        print(f"MATCH: {len(baseline['events'])} events aligned by {args.align}")
        return 0
    label, first, second = divergence
    print(f"DIVERGENCE: {label}")
    print(f"  baseline: {_format_event(first)}")
    print(f"  candidate: {_format_event(second)}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
