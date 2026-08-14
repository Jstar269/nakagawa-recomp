#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# frame_capture_check.py - temporal acceptance check for present-truthful frame captures.
#
# Runs over an archived VisualOracle output directory (or any directory holding
# build/snapshots-style captures) plus the run's stderr log and reports, in a JSON
# manifest plus a one-line summary:
#
#   - frame accounting: every "FBSNAP ... (result=1)" line must have its file present;
#     result=-1 lines are capture failures; SKIPPED lines are dropped presents.
#   - black-frame detection (mean luminance below a threshold);
#   - stale-frame detection (consecutive frames with identical content hashes);
#   - frame-number gaps in the rotating sequence (expected when the 30 Hz output cap
#     drops a present; the cadence SR_FBSNAP=N guarantees a minimum step);
#   - present-gap classification: PRESENT_GAP lines (guest running, no host present)
#     vs WATCHDOG lines (no guest flip at all) vs nothing (process deadlock is then
#     detected by the manager's backstop, not by this script).
#
# Only hashes and metrics are written to the manifest -- pixels never leave the machine.
# Synthetic/source-owned frames are handled identically, so CI can hash those.
#
# Exit codes: 0 = check ran and found no hard inconsistency; 1 = a capture reported
# successful is missing, or the check itself failed; 2 = usage error.

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

BLACK_MEAN_THRESHOLD = 4.0  # out of 255; menus/scene borders are far above this

# "FBSNAP f=<vcount> swapchain capture -> <path> (result=<n>)"
RE_RESULT = re.compile(
    r"FBSNAP f=(\d+) swapchain capture -> (\S+) \(result=(-?\d+)\)"
)
# "FBSNAP f=<vcount> swapchain capture -> SKIPPED (no present serviced this frame)"
RE_SKIPPED = re.compile(r"FBSNAP f=(\d+) swapchain capture -> SKIPPED")
RE_PRESENT_GAP = re.compile(r"PRESENT_GAP: vcount=(\d+) last_host_present=(\d+) gap=(\d+)")
RE_WATCHDOG = re.compile(r"WATCHDOG: no frame presented for (\d+) vblanks")
RE_LEGACY = re.compile(r"FBSNAP f=(\d+) -> (\S+)")


def parse_ppm(path):
    """Return (width, height, payload_bytes) for a P6 PPM, or None on malformed input."""
    data = Path(path).read_bytes()
    if data[:2] != b"P6":
        return None
    # Header: "P6" <ws> W <ws> H <ws> maxval <single whitespace> payload
    pos = 2
    tokens = []
    while len(tokens) < 3 and pos < len(data):
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        if pos == start:
            break
        try:
            tokens.append(int(data[start:pos]))
        except ValueError:
            return None
    if len(tokens) != 3:
        return None
    w, h, maxval = tokens
    if maxval != 255:
        return None
    # The PPM spec puts exactly ONE whitespace char between maxval and the raster; pixel
    # bytes may themselves be 0x0A/0x09/0x20, so a run-skip here would eat the payload.
    if pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1
    body = w * h * 3
    if len(data) - pos < body:
        return None
    return w, h, data[pos : pos + body]


def frame_number(name):
    """Frame number for frame_%04u.ppm (rotating) and frame_v<vcount>.ppm (windows)."""
    m = re.fullmatch(r"frame_v(\d+)\.ppm", name)
    if m:
        return ("v", int(m.group(1)))
    m = re.fullmatch(r"frame_(\d+)\.ppm", name)
    if m:
        return ("n", int(m.group(1)))
    return None


def analyze_frames(directory):
    """Analyze every capture in `directory`; returns (frames, errors) where frames is a
    list of dicts sorted by frame number."""
    frames = []
    for p in sorted(Path(directory).iterdir()):
        if not p.is_file():
            continue
        num = frame_number(p.name)
        if num is None:
            continue
        parsed = parse_ppm(p)
        if parsed is None:
            frames.append(
                {
                    "file": p.name,
                    "frame": num[1],
                    "style": num[0],
                    "error": "malformed ppm",
                }
            )
            continue
        w, h, payload = parsed
        mean = sum(payload) / (len(payload) or 1)
        frames.append(
            {
                "file": p.name,
                "frame": num[1],
                "style": num[0],
                "width": w,
                "height": h,
                "mean_luma": round(mean, 3),
                "black": mean < BLACK_MEAN_THRESHOLD,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        )
    frames.sort(key=lambda f: (f["style"], f["frame"]))
    return frames


def classify_log(log_path):
    """Parse the runtime stderr for capture results, skips, and present-gap lines."""
    report = {
        "capture_results": [],   # {vcount, path, result}
        "skipped": [],
        "present_gaps": [],      # {vcount, last_present, gap}
        "watchdogs": [],         # {vblanks}
        "legacy": [],            # {vcount, path}
    }
    if log_path is None or not Path(log_path).exists():
        return report
    for line in Path(log_path).read_text(errors="replace").splitlines():
        m = RE_RESULT.search(line)
        if m:
            report["capture_results"].append(
                {
                    "vcount": int(m.group(1)),
                    "path": m.group(2),
                    "result": int(m.group(3)),
                }
            )
            continue
        m = RE_SKIPPED.search(line)
        if m:
            report["skipped"].append(int(m.group(1)))
            continue
        m = RE_PRESENT_GAP.search(line)
        if m:
            report["present_gaps"].append(
                {
                    "vcount": int(m.group(1)),
                    "last_present": int(m.group(2)),
                    "gap": int(m.group(3)),
                }
            )
            continue
        m = RE_WATCHDOG.search(line)
        if m:
            report["watchdogs"].append({"vblanks": int(m.group(1))})
            continue
        m = RE_LEGACY.search(line)
        if m:
            report["legacy"].append({"vcount": int(m.group(1)), "path": m.group(2)})
    return report


def present_gap_classification(report):
    """Distinguish 'guest running but no present' from process deadlock."""
    if report["present_gaps"] and not report["watchdogs"]:
        return "guest-running-no-present"
    if report["present_gaps"] and report["watchdogs"]:
        return "both-gap-and-stall"
    if report["watchdogs"]:
        return "guest-stalled-no-flip"
    return "no-gap-recorded"


def run_check(directory, log_path, out_path):
    frames = analyze_frames(directory)
    report = classify_log(log_path)

    by_name = {f["file"] for f in frames if "error" not in f}
    missing_for_success = []
    capture_failures = []
    for r in report["capture_results"]:
        if r["result"] == -1:
            capture_failures.append(r)
        elif r["result"] == 1:
            if Path(r["path"]).name not in by_name:
                missing_for_success.append(r)

    black = [f["file"] for f in frames if f.get("black")]
    duplicates = []
    prev = None
    for f in frames:
        if ("sha256" in f and prev is not None and "sha256" in prev
                and f["sha256"] == prev["sha256"]):
            duplicates.append((prev["file"], f["file"]))
        prev = f

    # Frame-number gaps in the rotating sequence (style "n"): the cadence guarantees a
    # minimum step, so a gap larger than 1 is only surprising when every armed frame was
    # presented; report them, do not fail on them.
    n_frames = [f for f in frames if f["style"] == "n" and "error" not in f]
    gaps = []
    for a, b in zip(n_frames, n_frames[1:]):
        if b["frame"] > a["frame"] + 1:
            gaps.append({"from": a["frame"], "to": b["frame"], "step": b["frame"] - a["frame"]})

    hard_errors = missing_for_success + [f for f in frames if "error" in f]
    gap_class = present_gap_classification(report)
    verdict = "clean"
    if hard_errors:
        verdict = "hard-error"
    elif capture_failures or black or gap_class.startswith("guest") or report["watchdogs"]:
        # Gaps larger than the cadence are normal when the 30 Hz output cap drops a
        # present; they are reported in the JSON but do not alone warn.
        verdict = "warn"

    result = {
        "total_frames": len(frames),
        "black_frames": black,
        "duplicate_consecutive": [{"first": a, "second": b} for a, b in duplicates],
        "frame_number_gaps": gaps,
        "capture_failures": capture_failures,
        "skipped_presents": len(report["skipped"]),
        "present_gaps": report["present_gaps"],
        "present_gap_classification": gap_class,
        "watchdog_events": len(report["watchdogs"]),
        "missing_for_success": missing_for_success,
        "legacy_snaps": len(report["legacy"]),
        "verdict": verdict,
        "frames": frames,
    }
    if out_path:
        Path(out_path).write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", required=True, help="directory holding frame_*.ppm captures")
    ap.add_argument("--log", help="runtime stderr log to classify (optional)")
    ap.add_argument("--out", help="JSON manifest path (optional)")
    args = ap.parse_args(argv)

    if not Path(args.dir).is_dir():
        print("frame_capture_check: --dir is not a directory", file=sys.stderr)
        return 2
    result = run_check(args.dir, args.log, args.out)

    summary = (
        f"frames={result['total_frames']} black={len(result['black_frames'])} "
        f"duplicates={len(result['duplicate_consecutive'])} "
        f"capture_failures={len(result['capture_failures'])} "
        f"missing={len(result['missing_for_success'])} "
        f"skipped={result['skipped_presents']} gaps={len(result['frame_number_gaps'])} "
        f"present={result['present_gap_classification']} verdict={result['verdict']}"
    )
    print(summary)
    return 1 if result["verdict"] == "hard-error" else 0


if __name__ == "__main__":
    sys.exit(main())
