#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCHEMA = "nakagawa-perf-v1"
VOLATILE_PATHS = frozenset({"$.wall_ns", "$.interval_count"})
RANKING_PATHS = (
    "guest.aot_ns",
    "guest.interpreter_ns",
    "vfpu.ns",
    "scheduler.runnable_ns",
    "scheduler.blocked_ns",
    "scheduler.idle_ns",
    "ge.cpu_ns",
    "vulkan.submit_ns",
    "vulkan.wait_ns",
    "vulkan.readback_ns",
    "vulkan.pipeline_creation_ns",
    "textures.decode_ns",
    "storage.iso_read_ns",
    "storage.vfs_read_ns",
    "media.h264_decode_ns",
    "media.atrac_decode_ns",
    "audio.mix_ns",
    "audio.output_ns",
)
REQUIRED_TOP_LEVEL = (
    "schema",
    "build",
    "wall_ns",
    "interval_count",
    "vblanks",
    "guest",
    "transitions",
    "scheduler",
    "vfpu",
    "ge",
    "vulkan",
    "textures",
    "storage",
    "media",
    "audio",
    "boundaries",
)


class PerfSummaryError(ValueError):
    pass


def _fail(path: str, message: str) -> None:
    raise PerfSummaryError(f"{path}: {message}")


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _require_number(value: Any, path: str) -> None:
    if not _is_number(value) or value < 0:
        _fail(path, "expected a non-negative number")


def _require_integer(value: Any, path: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        _fail(path, "expected a non-negative integer")


def _require_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(path, "expected an object")
    return value


def _require_keys(value: dict[str, Any], keys: tuple[str, ...], path: str) -> None:
    for key in keys:
        if key not in value:
            _fail(f"{path}.{key}", "missing required property")


def validate_summary(summary: Any) -> None:
    root = _require_object(summary, "$")
    if root.get("schema") != SCHEMA:
        _fail("$.schema", f"expected {SCHEMA!r}")
    _require_keys(root, REQUIRED_TOP_LEVEL, "$")
    _require_object(root["build"], "$.build")
    _require_object(root["vblanks"], "$.vblanks")
    _require_object(root["guest"], "$.guest")
    _require_object(root["transitions"], "$.transitions")
    _require_object(root["scheduler"], "$.scheduler")
    _require_object(root["vfpu"], "$.vfpu")
    _require_object(root["ge"], "$.ge")
    _require_object(root["vulkan"], "$.vulkan")
    _require_object(root["textures"], "$.textures")
    _require_object(root["storage"], "$.storage")
    _require_object(root["media"], "$.media")
    _require_object(root["audio"], "$.audio")
    if not isinstance(root["boundaries"], list):
        _fail("$.boundaries", "expected an array")
    _require_integer(root["wall_ns"], "$.wall_ns")
    _require_integer(root["interval_count"], "$.interval_count")
    _require_keys(root["guest"], ("ns", "aot_ns", "aot_calls", "aot_instruction_count",
                                  "interpreter_ns", "interpreter_calls", "interpreter_instructions"), "$.guest")
    aot_instructions = root["guest"]["aot_instruction_count"]
    if aot_instructions is not None:
        _require_integer(aot_instructions, "$.guest.aot_instruction_count")
    for path in RANKING_PATHS:
        value = root
        for part in path.split("."):
            value = value.get(part) if isinstance(value, dict) else None
        _require_number(value, f"$.{path}")
    _require_keys(root["transitions"], ("aot_to_interpreter_count", "interpreter_to_aot_count", "top_pcs"), "$.transitions")
    top_pcs = _require_object(root["transitions"]["top_pcs"], "$.transitions.top_pcs")
    _require_keys(top_pcs, ("aot_to_interpreter", "interpreter_to_aot"), "$.transitions.top_pcs")
    for direction in ("aot_to_interpreter", "interpreter_to_aot"):
        entries = top_pcs[direction]
        if not isinstance(entries, list) or len(entries) > 16:
            _fail(f"$.transitions.top_pcs.{direction}", "expected at most 16 entries")
        seen: set[tuple[str, str]] = set()
        for index, entry in enumerate(entries):
            item = _require_object(entry, f"$.transitions.top_pcs.{direction}[{index}]")
            _require_keys(item, ("pc", "reason", "count"), f"$.transitions.top_pcs.{direction}[{index}]")
            if (not isinstance(item["pc"], str) or len(item["pc"]) <= 2
                    or not item["pc"].startswith("0x")
                    or any(character not in "0123456789abcdefABCDEF" for character in item["pc"][2:])):
                _fail(f"$.transitions.top_pcs.{direction}[{index}].pc", "expected a hexadecimal string")
            if not isinstance(item["reason"], str) or not item["reason"]:
                _fail(f"$.transitions.top_pcs.{direction}[{index}].reason", "expected a non-empty string")
            _require_integer(item["count"], f"$.transitions.top_pcs.{direction}[{index}].count")
            key = (item["pc"], item["reason"])
            if key in seen:
                _fail(f"$.transitions.top_pcs.{direction}[{index}]", "duplicate PC/reason")
            seen.add(key)


def load_summary(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PerfSummaryError(f"{path}: cannot read JSON: {exc}") from exc
    validate_summary(value)
    return value


def _value_at(summary: dict[str, Any], path: str) -> Any:
    value: Any = summary
    for part in path.split("."):
        value = value[part]
    return value


def _close(before: Any, after: Any, tolerance: float) -> bool:
    if not (_is_number(before) and _is_number(after)):
        return before == after
    delta = abs(float(after) - float(before))
    return delta <= tolerance * max(1.0, abs(float(before)))


def _change(path: str, before: Any, after: Any) -> dict[str, Any]:
    result: dict[str, Any] = {"path": path, "before": before, "after": after}
    if _is_number(before) and _is_number(after):
        result["delta"] = after - before
    return result


def _transition_map(entries: list[dict[str, Any]]) -> dict[tuple[str, str], int]:
    return {(entry["pc"], entry["reason"]): int(entry["count"]) for entry in entries}


def _compare_transitions(before: dict[str, Any], after: dict[str, Any], tolerance: float,
                         changes: list[dict[str, Any]]) -> None:
    left = before["transitions"]["top_pcs"]
    right = after["transitions"]["top_pcs"]
    for direction in ("aot_to_interpreter", "interpreter_to_aot"):
        before_map = _transition_map(left[direction])
        after_map = _transition_map(right[direction])
        for key in sorted(set(before_map) | set(after_map)):
            old = before_map.get(key)
            new = after_map.get(key)
            if old is None or new is None or not _close(old, new, tolerance):
                changes.append(_change(
                    f"$.transitions.top_pcs.{direction}[{key[0]}:{key[1]}]",
                    old,
                    new,
                ))


def _compare_values(before: Any, after: Any, path: str, tolerance: float,
                    changes: list[dict[str, Any]]) -> None:
    if path in VOLATILE_PATHS:
        return
    if isinstance(before, dict) and isinstance(after, dict):
        for key in sorted(set(before) | set(after)):
            child = f"{path}.{key}"
            if key not in before or key not in after:
                changes.append(_change(child, before.get(key), after.get(key)))
            else:
                _compare_values(before[key], after[key], child, tolerance, changes)
        return
    if isinstance(before, list) and isinstance(after, list):
        if path == "$.transitions.top_pcs.aot_to_interpreter" or path == "$.transitions.top_pcs.interpreter_to_aot":
            return
        if len(before) != len(after):
            changes.append(_change(f"{path}.length", len(before), len(after)))
        for index, (old, new) in enumerate(zip(before, after, strict=False)):
            _compare_values(old, new, f"{path}[{index}]", tolerance, changes)
        return
    if not _close(before, after, tolerance):
        changes.append(_change(path, before, after))


def _ranking(summary: dict[str, Any]) -> list[dict[str, Any]]:
    values = []
    for path in RANKING_PATHS:
        value = _value_at(summary, path)
        if value:
            values.append({"metric": path, "ns": value})
    return sorted(values, key=lambda item: (-item["ns"], item["metric"]))


def diff_summaries(before: dict[str, Any], after: dict[str, Any], tolerance: float = 0.0) -> dict[str, Any]:
    if tolerance < 0.0:
        raise PerfSummaryError("tolerance must be non-negative")
    validate_summary(before)
    validate_summary(after)
    changes: list[dict[str, Any]] = []
    _compare_values(before, after, "$", tolerance, changes)
    _compare_transitions(before, after, tolerance, changes)
    changes = [change for change in changes if change["path"] not in {
        "$.transitions.top_pcs.aot_to_interpreter",
        "$.transitions.top_pcs.interpreter_to_aot",
    }]
    return {
        "schema": "nakagawa-perf-diff-v1",
        "changed": bool(changes),
        "tolerance": tolerance,
        "changes": changes,
        "ranking": _ranking(after),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare two Nakagawa SR_PERF JSON summaries")
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--tolerance", type=float, default=0.0)
    args = parser.parse_args(argv)
    try:
        result = diff_summaries(load_summary(args.baseline), load_summary(args.candidate), args.tolerance)
    except PerfSummaryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 1 if result["changed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
