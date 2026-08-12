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
"""
import os
import sys
import json
import time
import argparse


def read_ppm(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
        # P6\n<w> <h>\n255\n
        parts = data.split(b"\n", 3)
        if parts[0] != b"P6":
            return None, 0, 0
        w, h = map(int, parts[1].split())
        return parts[3][: w * h * 3], w, h
    except Exception:
        return None, 0, 0


def run_diff(da, db, threshold):
    if not os.path.exists(da) or not os.path.exists(db):
        return {
            "timestamp": time.time(),
            "threshold": threshold,
            "summary": {
                "total_frames": 0,
                "passed_frames": 0,
                "failed_frames": 0,
                "pass_rate": 0.0
            },
            "frames": []
        }

    filesA = set(os.listdir(da))
    filesB = set(os.listdir(db))
    # Match files with identical names
    names = sorted(filesA & filesB, key=lambda n: int("".join(c for c in n if c.isdigit()) or 0))

    frames_report = []
    passed = 0
    failed = 0

    for n in names:
        pathA = os.path.join(da, n)
        pathB = os.path.join(db, n)

        a, w, h = read_ppm(pathA)
        b, _, _ = read_ppm(pathB)

        if a is None or b is None or len(a) != len(b):
            frames_report.append({
                "filename": n,
                "width": w or 480,
                "height": h or 272,
                "total_pixels": w * h if (w and h) else 130560,
                "diff_pixels": 0,
                "diff_pct": 0.0,
                "big_diff_pixels": 0,
                "big_diff_pct": 0.0,
                "max_delta": 255,
                "status": "fail"
            })
            failed += 1
            continue

        npx = w * h
        diff = 0
        big = 0
        maxd = 0

        # Loop through pixels to find color channel differences
        for i in range(0, npx * 3, 3):
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
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
            "width": w,
            "height": h,
            "total_pixels": npx,
            "diff_pixels": diff,
            "diff_pct": round(100.0 * diff / npx, 4) if npx else 0.0,
            "big_diff_pixels": big,
            "big_diff_pct": round(100.0 * big / npx, 4) if npx else 0.0,
            "max_delta": maxd,
            "status": status
        })

    total = passed + failed
    pass_rate = round(100.0 * passed / total, 2) if total else 0.0

    return {
        "timestamp": time.time(),
        "threshold": threshold,
        "summary": {
            "total_frames": total,
            "passed_frames": passed,
            "failed_frames": failed,
            "pass_rate": pass_rate
        },
        "frames": frames_report
    }


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
        report = run_diff(da, db, threshold)
        with open(report_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Report written to {report_path}")
        for f in report["frames"]:
            print(f"{f['filename']}: {f['total_pixels']}px diff={f['diff_pixels']} "
                  f"({f['diff_pct']}%) big(>{threshold})={f['big_diff_pixels']} "
                  f"({f['big_diff_pct']}%) maxdelta={f['max_delta']}")
        return

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
                report = run_diff(da, db, threshold)
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
