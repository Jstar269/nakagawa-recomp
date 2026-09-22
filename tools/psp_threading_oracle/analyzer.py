# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""CLI analyzer for threading oracle runs. Produces human-reviewable table."""

from __future__ import annotations

import argparse
import pathlib
import sys

try:
    from .parser import analyze_runs, human_table
except ImportError:
    # Direct invocation
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    from psp_threading_oracle.parser import analyze_runs, human_table

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Analyze PSP threading oracle captures")
    ap.add_argument("inputs", nargs="+", type=pathlib.Path, help="one or more captured result files (one per launch)")
    ap.add_argument("--out", type=pathlib.Path, help="write table to file")
    ap.add_argument("--provenance", type=str, help="label provenance: synthetic vs hardware (auto per-stream)")
    args = ap.parse_args(argv)
    texts: list[str] = []
    for p in args.inputs:
        texts.append(p.read_text(encoding="utf-8"))
    # By default without explicit evidence context, parsing is UNVERIFIED_CAPTURE.
    # Callers with trusted hardware workflow should construct HardwareCaptureContext
    # out-of-band and call analyze_runs(texts, evidence_contexts=...) directly.
    analysis = analyze_runs(texts)
    table = human_table(analysis)
    # Annotate provenance per input (unverified by default; never self-promotes to HARDWARE)
    try:
        from .parser import evidence_label, parse_threading_output
    except ImportError:
        from psp_threading_oracle.parser import evidence_label, parse_threading_output  # type: ignore
    for idx, text in enumerate(texts):
        try:
            parsed = parse_threading_output(text)
            # No out-of-band context supplied via CLI -> remains UNVERIFIED or SYNTHETIC
            label = evidence_label(parsed, evidence_context=None, raw_text=text)
            table += f"\n# Input {idx}: {label} ({args.inputs[idx]})"
        except Exception as exc:  # noqa: BLE001
            table += f"\n# Input {idx}: MALFORMED ({exc})"
    if args.out:
        args.out.write_text(table, encoding="utf-8")
    else:
        sys.stdout.write(table)
    # Exit 2 if missing cases or unstable? For now 0 always; harness decides.
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
