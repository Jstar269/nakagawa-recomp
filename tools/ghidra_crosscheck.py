# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Cross-check Ghidra's function inventory against tools/analyze.py discovery.

Ghidra (with ghidra-allegrex) is an independent, mature disassembler; treating
its function list as a second opinion surfaces analyze.py misdiscoveries of the
0x000e1724 class (functions suppressed by the _is_trailing_epilogue heuristic,
which at runtime become NONPLT_MISS dispatch faults). See ISSUES.md 2026-07-17.

Inputs:
  third_party/ghidra/exports/functions.csv  (python tools/ghidra_headless.py export-functions)
  place_game_here/EBOOT.elf                 (same ELF the pipeline consumes)

Address spaces: the recomp pipeline sees the ELF at base 0; Ghidra loads it at
its image base (recorded in the CSV header). Everything below is normalized to
the pipeline's base-0 view.

This is a triage aid, not a CI gate: the two tools legitimately disagree on
some boundaries (jump-table landing pads, data-in-text). --strict exits 1 when
Ghidra-only entries exist, for use once the list is triaged to zero.

Usage:  python tools/ghidra_crosscheck.py [--csv PATH] [--elf PATH] [--strict] [--json OUT]
"""

from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import analyze  # noqa: E402  (repo tool, needs ROOT/tools on sys.path)

DEFAULT_CSV = os.path.join(ROOT, "third_party", "ghidra", "exports", "functions.csv")
DEFAULT_ELF = os.path.join(ROOT, "place_game_here", "EBOOT.elf")


def load_ghidra_csv(path):
    """Return (image_base, exec_blocks, funcs) from ExportFunctionsCSV output.

    funcs: list of dicts {entry, size, name, thunk} in Ghidra's address space.
    exec_blocks: [(start, end_inclusive)] of executable memory blocks.
    """
    image_base = None
    exec_blocks = []
    funcs = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("# imageBase="):
                image_base = int(line.split("=", 1)[1], 16)
            elif line.startswith("# block "):
                kv = dict(tok.split("=", 1) for tok in line[8:].split() if "=" in tok)
                if kv.get("exec") == "1":
                    exec_blocks.append((int(kv["start"], 16), int(kv["end"], 16)))
            elif line.startswith("#") or line.startswith("entry,"):
                continue
            else:
                entry_s, size_s, name, thunk_s = line.split(",", 3)
                funcs.append({
                    "entry": int(entry_s, 16),
                    "size": int(size_s),
                    "name": name,
                    "thunk": thunk_s.strip() == "1",
                })
    if image_base is None:
        raise ValueError(f"{path}: missing '# imageBase=' header line")
    return image_base, exec_blocks, funcs


def main(argv):
    assert __doc__ is not None
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--csv", default=DEFAULT_CSV)
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 if any Ghidra-only entries remain")
    ap.add_argument("--json", metavar="OUT", help="write machine-readable report")
    ns = ap.parse_args(argv[1:])

    image_base, exec_blocks, gfuncs = load_ghidra_csv(ns.csv)
    elf = analyze.Elf(ns.elf)
    starts, ranges = analyze.analyze(elf, extra_spans=analyze.analyzer_span_from_env())

    # Ghidra entries, normalized to base 0, restricted to real code:
    # executable blocks, non-thunk (thunks model imports/PLT stubs).
    ghidra_entries = {}
    for f in gfuncs:
        if f["thunk"]:
            continue
        if exec_blocks and not any(lo <= f["entry"] <= hi for lo, hi in exec_blocks):
            continue
        ghidra_entries[(f["entry"] - image_base) & 0xFFFFFFFF] = f

    # Compare only inside the ranges analyze.py actually scans -- outside them
    # the pipeline never emits code, so a disagreement there is moot.
    in_scan = lambda a: analyze.in_ranges(a, ranges)  # noqa: E731
    g_in = {a for a in ghidra_entries if in_scan(a)}
    a_in = {a for a in starts if in_scan(a)}

    ghidra_only = sorted(g_in - a_in)
    analyze_only = sorted(a_in - g_in)
    both = g_in & a_in

    print(f"image base            0x{image_base:08x}")
    print(f"ghidra functions      {len(g_in)} (non-thunk, exec, in scan ranges)")
    print(f"analyze.py entries    {len(a_in)}")
    print(f"agreed                {len(both)}")
    print(f"ghidra-only (analyze.py misses -- NONPLT_MISS candidates): {len(ghidra_only)}")
    for a in ghidra_only[:40]:
        gf = ghidra_entries[a]
        print(f"  0x{a:08x}  size={gf['size']:<6} {gf['name']}")
    if len(ghidra_only) > 40:
        print(f"  ... and {len(ghidra_only) - 40} more (use --json for the full list)")
    print(f"analyze-only (usually benign: jump-table landings etc.): {len(analyze_only)}")
    for a in analyze_only[:10]:
        print(f"  0x{a:08x}")
    if len(analyze_only) > 10:
        print(f"  ... and {len(analyze_only) - 10} more")

    if ns.json:
        with open(ns.json, "w", encoding="utf-8") as fh:
            json.dump({
                "image_base": image_base,
                "ghidra_total": len(g_in),
                "analyze_total": len(a_in),
                "agreed": len(both),
                "ghidra_only": [
                    {"addr": "0x%08x" % a,
                     "size": ghidra_entries[a]["size"],
                     "name": ghidra_entries[a]["name"]}
                    for a in ghidra_only
                ],
                "analyze_only": ["0x%08x" % a for a in analyze_only],
            }, fh, indent=2)
        print(f"wrote {ns.json}")

    return 1 if (ns.strict and ghidra_only) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
