# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

# Extract the sequence of HLE imports (by NID) a trace executes, mapping each import-stub jr
# line (pc in .sceStub.text) to its NID via the import map. With two traces it reports how far
# the recompiled run's import sequence agrees with the oracle's -- the functional-equivalence
# metric for HLE, which tolerates UID/address value differences the trace-diff cannot.
#
# Usage: nidseq.py <imports.toml> <trace> [<oracle-trace>]

import sys

import tomllib


def load_imports(path):
    m = {}
    stubs = []
    for e in tomllib.load(open(path, "rb"))["import"]:
        m[e["stub"]] = (e["lib"], e["nid"])
        stubs.append(e["stub"])
    # Derive .sceStub.text range from the import table itself so the tool is
    # game-agnostic.  The original code hard-coded an ACX-specific address pair
    # (S0=0x08a246ac, S1=0x08a24dd4+8) which produced silent zero output for HST.
    # O(n) scan over stubs; acceptable for <10k entries, acceptable.
    if stubs:
        s0, s1 = min(stubs), max(stubs) + 8
    else:
        s0, s1 = 0, 0
    return m, s0, s1


def nid_seq(trace, imp, s0, s1, limit=100000):
    seq = []
    with open(trace) as fh:
        for line in fh:
            if line[0] == "#":
                continue
            p = line.split()
            if len(p) < 3:
                continue
            pc = int(p[1][3:], 16)
            if s0 <= pc < s1 and p[2] == "op=0x03e00008":  # the stub's jr $ra line
                seq.append((pc, imp.get(pc, ("?", 0))))
                if len(seq) >= limit:
                    break
    return seq


def main(argv):
    imp, s0, s1 = load_imports(argv[1])
    print(f"stub range: 0x{s0:08x}..0x{s1:08x} ({len(imp)} imports)")
    mine = nid_seq(argv[2], imp, s0, s1)
    print(f"{argv[2]}: {len(mine)} imports")
    for i, (pc, (lib, nid)) in enumerate(mine[:40]):
        print(f"  {i:3} {lib}.0x{nid:08x}")
    if len(argv) > 3:
        orac = nid_seq(argv[3], imp, s0, s1, limit=len(mine) + 5)
        n = min(len(mine), len(orac))
        agree = 0
        for i in range(n):
            if mine[i][0] != orac[i][0]:
                print(f"DIVERGE at import {i}: recomp={imp.get(mine[i][0])} oracle={imp.get(orac[i][0])}")
                break
            agree += 1
        else:
            print(f"import sequence agrees for all {n} compared")
        print(f"AGREE {agree} imports")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
