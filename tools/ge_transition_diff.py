#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Offline diff for the narrow GE transition trace (issue #69).

Takes the JSONL written with ``SR_GE_TRANSITION_TRACE=<path>`` (one record per
weighted GE_PRIM draw, see the ``SR_GE_TRANSITION_TRACE`` block in ``src/rt/ge.c``)
and a frame range, then prints a ``LAST_GOOD -> FIRST_BAD -> FIRST_RECOVERED``
diff per draw identity (which fields changed).

Usage:
    python tools/ge_transition_diff.py trace.jsonl --frames GOOD:BAD [--recovered F] [--end F]

``--frames`` takes ``GOOD:BAD`` (or ``GOOD,BAD``). Records are matched across
frames by ``draw_id`` (FNV-1a over VTYPE + vertex base + prim + count); repeated
identities within one frame pair up by occurrence order. Every field except the
positional ``frame``/``draw`` counters is compared, so list/command-address
relocations show up alongside matrix, VTYPE, and target changes. Matrix arrays
are summarized (changed count, max abs delta, first indices) rather than dumped
whole. ``--recovered`` pins the recovery frame; otherwise the first frame after
BAD whose compared fields all equal GOOD is reported (``--end`` caps the scan).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

#: Positional counters excluded from comparison and recovery matching.
POSITIONAL_FIELDS = frozenset({"frame", "draw"})


def parse_frames(spec: str) -> tuple[int, int]:
    for sep in (":", ","):
        if sep in spec:
            first, _, second = spec.partition(sep)
            try:
                return int(first), int(second)
            except ValueError:
                break
    raise ValueError(f"expected GOOD:BAD frame pair, got {spec!r}")


def load_records(path: Path) -> list[dict]:
    records = []
    with path.open(encoding="utf-8") as fp:
        for lineno, line in enumerate(fp, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if not isinstance(record, dict):
                raise ValueError(f"{path}:{lineno}: expected a JSON object per line")
            for key in ("frame", "draw_id", "prim", "count"):
                if key not in record:
                    raise ValueError(f"{path}:{lineno}: record is missing {key!r}")
            records.append(record)
    return records


def group_by_frame(records: list[dict]) -> dict[int, dict[str, list[dict]]]:
    frames: dict[int, dict[str, list[dict]]] = {}
    for record in records:
        frames.setdefault(int(record["frame"]), {}).setdefault(str(record["draw_id"]), []).append(record)
    return frames


def compare_values(good, bad) -> tuple[bool, str]:
    """Return (changed, detail) for one compared field."""
    if isinstance(good, list) and isinstance(bad, list):
        if len(good) != len(bad):
            return True, f"len {len(good)} -> {len(bad)}"
        changed = [(i, g, b) for i, (g, b) in enumerate(zip(good, bad, strict=False)) if g != b]
        if not changed:
            return False, ""
        deltas = [abs(b - g) for _, g, b in changed
                  if isinstance(g, (int, float)) and isinstance(b, (int, float))]
        detail = f"{len(changed)}/{len(good)} changed"
        if deltas:
            detail += f", max|d|={max(deltas):.9g}"
        shown = ", ".join(f"[{i}]: {g} -> {b}" for i, g, b in changed[:4])
        detail += f" ({shown}{'...' if len(changed) > 4 else ''})"
        return True, detail
    if good != bad:
        return True, f"{good} -> {bad}"
    return False, ""


def diff_records(good: dict, bad: dict) -> list[str]:
    changes = []
    for key in sorted(set(good) | set(bad)):
        if key in POSITIONAL_FIELDS:
            continue
        if key not in good:
            changes.append(f"{key}: <absent> -> {bad[key]}")
        elif key not in bad:
            changes.append(f"{key}: {good[key]} -> <absent>")
        else:
            changed, detail = compare_values(good[key], bad[key])
            if changed:
                changes.append(f"{key}: {detail}")
    return changes


def records_equal(good: dict, bad: dict) -> bool:
    keys = (set(good) | set(bad)) - POSITIONAL_FIELDS
    return all(good.get(key) == bad.get(key) for key in keys)


def find_recovery(frames: dict[int, dict[str, list[dict]]], draw_id: str, occurrence: int,
                  good: dict, bad_frame: int, end: int | None) -> tuple[int | None, dict | None]:
    for frame in sorted(f for f in frames if f > bad_frame and (end is None or f <= end)):
        candidates = frames[frame].get(draw_id, [])
        if occurrence < len(candidates) and records_equal(good, candidates[occurrence]):
            return frame, candidates[occurrence]
    return None, None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="JSONL written with SR_GE_TRANSITION_TRACE=<path>")
    parser.add_argument("--frames", required=True, help="LAST_GOOD:FIRST_BAD frame pair (GOOD:BAD)")
    parser.add_argument("--recovered", type=int, default=None,
                        help="pin the FIRST_RECOVERED frame instead of auto-scanning")
    parser.add_argument("--end", type=int, default=None,
                        help="cap the auto recovery scan at this frame (inclusive)")
    args = parser.parse_args(argv)

    try:
        good_frame, bad_frame = parse_frames(args.frames)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if good_frame == bad_frame:
        print("error: GOOD and BAD frames must differ", file=sys.stderr)
        return 2
    try:
        records = load_records(args.trace)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    frames = group_by_frame(records)
    for wanted, name in ((good_frame, "LAST_GOOD"), (bad_frame, "FIRST_BAD")):
        if wanted not in frames:
            available = sorted(frames)
            span = f"{available[0]}..{available[-1]}" if available else "<empty trace>"
            print(f"error: {name} frame={wanted} not in trace (available: {span})", file=sys.stderr)
            return 2
    if args.recovered is not None and args.recovered not in frames:
        print(f"error: --recovered frame={args.recovered} not in trace", file=sys.stderr)
        return 2

    good_ids = frames[good_frame]
    bad_ids = frames[bad_frame]
    lines = [f"LAST_GOOD frame={good_frame}", f"FIRST_BAD frame={bad_frame}"]
    for draw_id in sorted(set(good_ids) | set(bad_ids)):
        good_list = good_ids.get(draw_id, [])
        bad_list = bad_ids.get(draw_id, [])
        pairs = min(len(good_list), len(bad_list))
        for occurrence in range(pairs):
            good, bad = good_list[occurrence], bad_list[occurrence]
            lines.append(f"draw_id={draw_id} occurrence={occurrence} "
                         f"prim={good.get('prim')} count={good.get('count')} "
                         f"vtype={good.get('vtype')}")
            lines.append(f"  LAST_GOOD frame={good_frame} draw={good.get('draw')} "
                         f"list={good.get('list')} cmd={good.get('cmd')}")
            lines.append(f"  FIRST_BAD frame={bad_frame} draw={bad.get('draw')} "
                         f"list={bad.get('list')} cmd={bad.get('cmd')}")
            if args.recovered is not None:
                recovered_list = frames[args.recovered].get(draw_id, [])
                if occurrence < len(recovered_list):
                    recovered, recovered_frame = recovered_list[occurrence], args.recovered
                else:
                    recovered, recovered_frame = None, None
            else:
                recovered_frame, recovered = find_recovery(
                    frames, draw_id, occurrence, good, bad_frame, args.end)
            if recovered is None:
                bound = f" in ({bad_frame}, {args.end}]" if args.end is not None else \
                    f" after {bad_frame}"
                lines.append(f"  FIRST_RECOVERED none{bound}")
            else:
                lines.append(f"  FIRST_RECOVERED frame={recovered_frame} draw={recovered.get('draw')} "
                             f"list={recovered.get('list')} cmd={recovered.get('cmd')}")
            good_bad = diff_records(good, bad)
            if good_bad:
                lines.append("  good->bad changed:")
                lines.extend(f"    {change}" for change in good_bad)
            else:
                lines.append("  good->bad changed: <none>")
            if recovered is not None:
                bad_rec = diff_records(bad, recovered)
                if bad_rec:
                    lines.append("  bad->recovered changed:")
                    lines.extend(f"    {change}" for change in bad_rec)
                else:
                    lines.append("  bad->recovered changed: <none>")
        for occurrence in range(pairs, len(good_list)):
            good = good_list[occurrence]
            lines.append(f"draw_id={draw_id} occurrence={occurrence} "
                         f"prim={good.get('prim')} count={good.get('count')} "
                         f"MISSING in FIRST_BAD frame={bad_frame}")
        for occurrence in range(pairs, len(bad_list)):
            bad = bad_list[occurrence]
            lines.append(f"draw_id={draw_id} occurrence={occurrence} "
                         f"prim={bad.get('prim')} count={bad.get('count')} "
                         f"NEW in FIRST_BAD frame={bad_frame}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
