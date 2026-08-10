#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify HQ-1 (#296) measured VFPU register sets against the production decode.

Raw-first: parse the hardware lanes payloads and independently derive the
destination register permutation per (width, field) case, then compare with
tools/codegen.py's vreg_indices (the PPSSPP-cited decode that IND-2 targets).
No expected values from PPSSPP are seeded into the measurement itself.

The probe zeroes all 128 regs, loads pattern values 0x3f800001..4 (unique per
lane) at phys regs {0,1,2,3} (S000..S003), executes one raw vmov.<w> with dst
field E and src field 0x00, then reads all 128 regs.  A physical register p is
the destination of lane i if p holds pattern value i+1 and p is not the source
register for that lane (phys i).  When a lane's value disappears entirely its
source register was overwritten and is itself a destination (identity/rotation
cases resolve through the values that DID move).
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PAT = [0x3F800001, 0x3F800002, 0x3F800003, 0x3F800004]


def vreg_indices(reg: int, size: int) -> list[int]:
    # Mirrors tools/codegen.py exactly (the decode under test).
    mtx = (reg >> 2) & 7
    col = reg & 3
    transpose = (reg >> 5) & 1
    if size == 1:
        transpose = 0
        row = (reg >> 5) & 3
        length = 1
    elif size == 2:
        row = (reg >> 5) & 2
        length = 2
    elif size == 3:
        row = (reg >> 6) & 1
        length = 3
    else:
        row = (reg >> 5) & 2
        length = 4
    out = []
    for i in range(length):
        if transpose:
            out.append(mtx * 16 + ((row + i) & 3) * 4 + col)
        else:
            out.append(mtx * 16 + col * 4 + ((row + i) & 3))
    return out


def scalar_map_field_to_phys(field: int) -> int:
    """Independent Stage-A formula confirmed by the 128-lane measurement."""
    row = (field >> 5) & 3
    mtx = (field >> 2) & 7
    col = field & 3
    return mtx * 16 + col * 4 + row


def parse_lanes(lanes: str) -> dict[int, int]:
    """phys -> raw word from the probe's lanes= payload."""
    out = {}
    for tok in lanes.split(","):
        tok = tok.strip()
        if not tok:
            continue
        p, _, v = tok.partition(":")
        out[int(p, 16)] = int(v, 16)
    return out


def derive_perm(width: str, lanes: dict[int, int]) -> list[int]:
    """Lane i's destination phys, derived only from measured lanes."""
    n = {"s": 1, "p": 2, "t": 3, "q": 4}[width]
    perm = []
    for i in range(n):
        val = PAT[i]
        holders = sorted(p for p, v in lanes.items() if v == val)
        non_src = [p for p in holders if p != i]
        if non_src:
            perm.append(non_src[0])
        elif i in holders:
            perm.append(i)  # identity copy: dst == src
        else:
            perm.append(-1)  # value overwritten: dst took its place
    return perm


def main() -> int:
    results_dir = Path(__file__).resolve().parent.parent.parent / "oracle" / "hardware-results"
    runs = [results_dir / "vfpu-addr-run1.json", results_dir / "vfpu-addr-run2.json"]
    payloads = []
    for run in runs:
        d = json.loads(run.read_text(encoding="utf-8"))
        payloads.append(d["runs"][0]["raw"])

    width_cases = {}
    for raw in payloads:
        for line in raw.splitlines():
            if not line.startswith("NAKAGAWA_PSP_TEST"):
                continue
            m = re.search(r"case_id=([sptq]):0x([0-9a-f]+)", line)
            if not m:
                continue
            width, field = m.group(1), int(m.group(2), 16)
            lm = re.search(r"lanes=(\S+)", line)
            key = (width, field)
            lanes = parse_lanes(lm.group(1))
            if key not in width_cases:
                width_cases[key] = lanes
            elif width_cases[key] != lanes:
                print(f"MISMATCH between runs for {key}")
                return 1

    failures = 0
    print(f"{'case':8} {'measured':22} {'codegen':22} {'match'}")
    for (width, field), lanes in sorted(width_cases.items()):
        measured = derive_perm(width, lanes)
        predicted = vreg_indices(field, {"s": 1, "p": 2, "t": 3, "q": 4}[width])
        ok = measured == predicted
        if not ok:
            failures += 1
        print(f"{width}:0x{field:02x}  "
              f"[{','.join(f'{p:02x}' if p >= 0 else '--' for p in measured)}] "
              f"[{','.join(f'{p:02x}' for p in predicted)}]  {'OK' if ok else '*** MISMATCH ***'}")

    # Stage A scalar map: verify all 128 field->phys pairs against the formula.
    scalar_line = next(l for l in payloads[0].splitlines() if "case_id=S:map" in l)
    pairs = re.search(r"map=(\S+)", scalar_line).group(1).split(",")
    bad = 0
    for tok in pairs:
        f, ph = (int(x, 16) for x in tok.split(":"))
        if scalar_map_field_to_phys(f) != ph:
            bad += 1
            print(f"SCALAR MISMATCH field={f:02x} measured={ph:02x} formula={scalar_map_field_to_phys(f):02x}")
    print(f"\nStage A: {len(pairs)}/128 single-encoding pairs checked, {bad} formula mismatches")
    print(f"Stage B: {len(width_cases)} wide cases, {failures} decode mismatches")
    return 1 if (failures or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
