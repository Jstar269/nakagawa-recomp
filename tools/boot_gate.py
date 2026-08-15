#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize machine-readable native boot milestones from a runtime log.

Exit 0 means the required boot phases occurred in causal order, no disqualifying
fault was observed, and frame content was validated as nonblank. Exit 1 means
the run started but did not meet that evidence contract. Exit 2 means the log is
absent/invalid. ``--allow-present-only`` explicitly lowers only the frame-content
requirement for liveness diagnostics; the JSON output still labels that evidence
as ``present-submitted`` rather than visual success.

A ``phase=stalled`` event carrying ``observation=no_new_flip`` is reported as a
neutral observation (``observations`` JSON list), not a disqualifying reason: the
no-frame watchdog fires on a stretch of vblanks with no new presented frame, which
a legitimately static scene (e.g. a save-confirmation modal waiting for input) also
produces. Legacy bare ``phase=stalled`` events remain disqualifying.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

from evidence_model import milestones_in_order


EVENT_RE = re.compile(r"\bBOOT_EVENT\s+(.+)$")
PAIR_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\S+)')
EXPECTED = ("image_loaded", "runtime_registered", "window_ready", "guest_start", "display_flip", "first_frame")
FAULT_MARKERS = ("ERROR:", "MEM OOR", "Unhandled")


def _parse_nonnegative_int(value: str | None) -> tuple[int, bool]:
    if value is None:
        return 0, True
    try:
        parsed = int(value, 0)
    except (TypeError, ValueError):
        return 0, False
    return (parsed, parsed >= 0) if parsed >= 0 else (0, False)


def parse_log(path: str, *, allow_present_only: bool = False) -> dict:
    if type(allow_present_only) is not bool:
        raise TypeError("allow_present_only must be a boolean")

    events: list[dict[str, str]] = []
    malformed_paths = 0
    faults: list[str] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, 1):
            match = EVENT_RE.search(line)
            if match:
                event = {key: value.strip('"') for key, value in PAIR_RE.findall(match.group(1))}
                event["line"] = str(line_number)
                events.append(event)
            if "Open(host0:" in line and ("Open(host0:)" in line or "�" in line or "\x03" in line):
                malformed_paths += 1
            if any(marker in line for marker in FAULT_MARKERS):
                faults.append(line.strip())

    phases = [event.get("phase", "unknown") for event in events]
    reached = {phase: phase in phases for phase in EXPECTED}
    sequence_ok = milestones_in_order(phases, EXPECTED)

    first_frame = next((event for event in events if event.get("phase") == "first_frame"), None)
    frame_source = first_frame.get("source", "cpu") if first_frame else None
    nonzero, nonzero_valid = _parse_nonnegative_int(first_frame.get("nonzero_pixels") if first_frame else None)
    if first_frame and not nonzero_valid:
        faults.append(
            f"BOOT_GATE: invalid nonzero_pixels={first_frame.get('nonzero_pixels')!r} "
            f"at line {first_frame.get('line', '?')}"
        )

    if first_frame is None:
        frame_evidence = "none"
    elif nonzero > 0:
        frame_evidence = "content-validated"
    elif frame_source == "gpu":
        frame_evidence = "present-submitted"
    else:
        frame_evidence = "blank-or-unvalidated"

    frame_ok = frame_evidence == "content-validated" or (
        allow_present_only and frame_evidence == "present-submitted"
    )
    stalled = "stalled" in phases

    # A phase=stalled event from the current runtime carries
    # observation=no_new_flip: the no-frame watchdog fires on a stretch of
    # vblanks with no new presented frame, which a legitimately static scene
    # (e.g. a save-confirmation modal waiting for input) also produces. That is
    # a neutral NO-NEW-FLIP observation, not a hang/stall verdict, so it is
    # reported as an observation instead of a disqualifying reason. Only the
    # legacy bare phase=stalled event (old runtime logs) keeps its
    # disqualifying meaning.
    no_frame_observation = any(
        event.get("phase") == "stalled" and event.get("observation") == "no_new_flip"
        for event in events
    )
    observations: list[str] = []
    if no_frame_observation:
        observations.append("no-frame-observation")

    reasons: list[str] = []
    if not sequence_ok:
        reasons.append("milestones-incomplete-or-out-of-order")
    if stalled and not no_frame_observation:
        reasons.append("stalled")
    if faults:
        reasons.append("disqualifying-fault")
    if malformed_paths:
        reasons.append("malformed-host-path")
    if not frame_ok:
        reasons.append("frame-content-unvalidated")

    ok = not reasons
    last_phase = phases[-1] if phases else "not-started"
    return {
        "ok": ok,
        "stalled": stalled,
        "observations": observations,
        "lastPhase": last_phase,
        "reached": reached,
        "sequenceOk": sequence_ok,
        "requiredSequence": list(EXPECTED),
        "frameSource": frame_source,
        "frameEvidence": frame_evidence,
        "requiredFrameEvidence": "present-submitted" if allow_present_only else "content-validated",
        "nonzeroPixels": nonzero,
        "malformedHostPaths": malformed_paths,
        "faultCount": len(faults),
        "faults": faults[-20:],
        "disqualifyingReasons": reasons,
        "events": events,
        "logPath": os.path.abspath(path),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", default=os.path.join("logs", "stderr_run.log"))
    parser.add_argument("--json", action="store_true", help="emit one JSON object")
    parser.add_argument(
        "--allow-present-only",
        action="store_true",
        help="accept a causally ordered fault-free GPU present as liveness evidence, not visual success",
    )
    args = parser.parse_args(argv)
    if not os.path.isfile(args.log):
        result = {"ok": False, "error": "log-not-found", "logPath": os.path.abspath(args.log)}
        print(json.dumps(result) if args.json else f"BOOT GATE: log not found: {result['logPath']}")
        return 2
    result = parse_log(args.log, allow_present_only=args.allow_present_only)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        mark = "PASS" if result["ok"] else "FAIL"
        print(
            f"BOOT GATE: {mark} "
            f"(last={result['lastPhase']}, frame={result['frameEvidence']}, "
            f"nonzero={result['nonzeroPixels']}, malformed_paths={result['malformedHostPaths']}, "
            f"faults={result['faultCount']})"
        )
        for phase, reached in result["reached"].items():
            print(f"  {'[x]' if reached else '[ ]'} {phase}")
        for reason in result["disqualifyingReasons"]:
            print(f"  [!] {reason}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
