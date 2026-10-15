# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

"""
Usage: python tools/ppmdiff.py [--threshold N] [--watch] [dirA] [dirB]
Compare frames in dirA and dirB.
If --watch is specified or SR_FBSNAP=1 is set in the environment,
automatically watch the directories and generate a live visual_regression_report.json.
Otherwise, perform a single comparison run and output the JSON report.
Exit code in one-shot mode:
  0 = all frames passed with complete coverage
  1 = any failure (missing dir, empty dirs, coverage gap, comparison failure)
"""
import os
import sys
import json
import time
import argparse


class CoverageError(Exception):
    """Raised when framebuffer comparison coverage is incomplete."""


def read_ppm(path):
    """Read a P6 PPM file.  Returns (pixels_bytes, width, height).

    Raises ``CoverageError`` on any I/O, format, or truncation problem so that
    callers can never silently lose coverage.
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise CoverageError(f"cannot read {path}: {exc}") from exc

    parts = data.split(b"\n", 3)
    if len(parts) < 4:
        raise CoverageError(f"malformed PPM header in {path}: too few header lines")
    if parts[0] != b"P6":
        raise CoverageError(f"not a P6 PPM file: {path}")

    try:
        w, h = map(int, parts[1].split())
    except (ValueError, IndexError) as exc:
        raise CoverageError(f"bad dimensions in {path}: {exc}") from exc

    if w <= 0 or h <= 0:
        raise CoverageError(f"non-positive dimensions {w}x{h} in {path}")

    maxval = parts[2].strip()
    if maxval != b"255":
        raise CoverageError(f"unsupported maxval {maxval!r} in {path}")

    expected = w * h * 3
    payload = parts[3]
    if len(payload) < expected:
        raise CoverageError(
            f"truncated payload in {path}: expected {expected} bytes, got {len(payload)}"
        )

    return payload[:expected], w, h


def run_diff(da, db, threshold):
    """Compare PPM frames in *da* vs *db*.

    Returns ``(report_dict, ok)`` where *ok* is ``True`` only when every
    coverage requirement is met and zero frames failed.
    """
    # --- coverage gate: both dirs must exist ---
    if not os.path.isdir(da):
        raise CoverageError(f"directory A does not exist: {da}")
    if not os.path.isdir(db):
        raise CoverageError(f"directory B does not exist: {db}")

    filesA = {n for n in os.listdir(da) if os.path.isfile(os.path.join(da, n))}
    filesB = {n for n in os.listdir(db) if os.path.isfile(os.path.join(db, n))}

    # --- coverage gate: at least one frame ---
    if not filesA and not filesB:
        raise CoverageError("both directories are empty — zero coverage")

    # --- coverage gate: sets must match exactly ---
    a_only = sorted(filesA - filesB)
    b_only = sorted(filesB - filesA)
    coverage_ok = True
    coverage_errors = []

    if a_only:
        coverage_ok = False
        coverage_errors.append(f"A-only frames: {a_only}")
    if b_only:
        coverage_ok = False
        coverage_errors.append(f"B-only frames: {b_only}")

    matched = sorted(
        filesA & filesB,
        key=lambda n: int("".join(c for c in n if c.isdigit()) or 0),
    )

    if not matched and (filesA or filesB):
        raise CoverageError(
            f"disjoint frame sets — zero comparisons. {'; '.join(coverage_errors)}"
        )

    frames_report = []
    passed = 0
    failed = 0

    for n in matched:
        pathA = os.path.join(da, n)
        pathB = os.path.join(db, n)

        # read_ppm raises CoverageError on any problem
        a, wa, ha = read_ppm(pathA)
        b, wb, hb = read_ppm(pathB)

        if wa != wb or ha != hb:
            raise CoverageError(
                f"dimension mismatch for {n}: A={wa}x{ha}, B={wb}x{hb}"
            )

        npx = wa * ha
        diff = 0
        big = 0
        maxd = 0

        for i in range(0, npx * 3, 3):
            d = max(
                abs(a[i] - b[i]),
                abs(a[i + 1] - b[i + 1]),
                abs(a[i + 2] - b[i + 2]),
            )
            if d:
                diff += 1
                if d > threshold:
                    big += 1
                if d > maxd:
                    maxd = d

        status = "pass" if big == 0 else "fail"
        if status == "pass":
            passed += 1
        else:
            failed += 1

        frames_report.append({
            "filename": n,
            "width": wa,
            "height": ha,
            "total_pixels": npx,
            "diff_pixels": diff,
            "diff_pct": round(100.0 * diff / npx, 4) if npx else 0.0,
            "big_diff_pixels": big,
            "big_diff_pct": round(100.0 * big / npx, 4) if npx else 0.0,
            "max_delta": maxd,
            "status": status,
        })

    # Build unmatched frame entries (always "fail")
    for n in a_only:
        frames_report.append({
            "filename": n,
            "status": "fail",
            "reason": "A-only (missing from B)",
        })
        failed += 1

    for n in b_only:
        frames_report.append({
            "filename": n,
            "status": "fail",
            "reason": "B-only (missing from A)",
        })
        failed += 1

    total = passed + failed
    pass_rate = round(100.0 * passed / total, 2) if total else 0.0

    ok = coverage_ok and failed == 0

    report = {
        "timestamp": time.time(),
        "threshold": threshold,
        "summary": {
            "total_frames": total,
            "passed_frames": passed,
            "failed_frames": failed,
            "pass_rate": pass_rate,
            "coverage_ok": coverage_ok,
        },
        "frames": frames_report,
    }

    if coverage_errors:
        report["coverage_errors"] = coverage_errors

    return report, ok


def main():
    ap = argparse.ArgumentParser(description="PPM framebuffer diff reporter")
    ap.add_argument("--threshold", type=int, default=3,
                    help="per-channel delta considered 'big' (default: 3, was 8)")
    ap.add_argument("--watch", action="store_true",
                    help="automatically watch directories and generate live reports")
    ap.add_argument("dirA", nargs="?", default="build/snapshots")
    ap.add_argument("dirB", nargs="?", default="build/golden")
    args = ap.parse_args()

    da = args.dirA
    db = args.dirB
    threshold = args.threshold
    watch_mode = args.watch or os.environ.get("SR_FBSNAP") == "1"

    report_path = "visual_regression_report.json"

    if not watch_mode:
        try:
            report, ok = run_diff(da, db, threshold)
        except CoverageError as exc:
            print(f"COVERAGE FAILURE: {exc}", file=sys.stderr)
            sys.exit(1)

        with open(report_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Report written to {report_path}")
        for fr in report["frames"]:
            if "reason" in fr:
                print(f"{fr['filename']}: {fr['status']} — {fr['reason']}")
            else:
                print(f"{fr['filename']}: {fr['total_pixels']}px diff={fr['diff_pixels']} "
                      f"({fr['diff_pct']}%) big(>{threshold})={fr['big_diff_pixels']} "
                      f"({fr['big_diff_pct']}%) maxdelta={fr['max_delta']}")

        if not ok:
            sys.exit(1)
        return

    # --- watch mode (unchanged) ---
    print(f"Watching {da} and {db} for changes...")
    last_mtimes = {}

    while True:
        try:
            if not os.path.exists(da) or not os.path.exists(db):
                time.sleep(1.0)
                continue

            current_mtimes = {}
            for d in (da, db):
                for n in os.listdir(d):
                    p = os.path.join(d, n)
                    if os.path.isfile(p):
                        try:
                            current_mtimes[p] = os.path.getmtime(p)
                        except OSError:
                            pass  # Ignore file being deleted or locked

            if current_mtimes != last_mtimes:
                print("Change detected. Regenerating report...")
                try:
                    report, _ = run_diff(da, db, threshold)
                except CoverageError as exc:
                    print(f"Coverage problem: {exc}", file=sys.stderr)
                    last_mtimes = current_mtimes
                    continue
                with open(report_path, "w") as f:
                    json.dump(report, f, indent=2)
                last_mtimes = current_mtimes

            time.sleep(0.5)
        except KeyboardInterrupt:
            print("\nExiting watch mode.")
            break
        except Exception as e:
            print(f"Error in watch loop: {e}", file=sys.stderr)
            time.sleep(2.0)


if __name__ == "__main__":
    main()
