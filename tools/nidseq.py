# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Extract the sequence of HLE imports (by NID) a trace executes, mapping each import-stub jr
# line (pc in .sceStub.text) to its NID via the import map. With two traces it verifies the
# recompiled run's import sequence against the oracle's -- the functional-equivalence
# metric for HLE, which tolerates UID/address value differences the trace-diff cannot.
#
# Modes (issue #381):
#   informational (one trace):  nidseq.py <imports.toml> <trace>
#       Prints the import sequence and exits 0. No equivalence claim is made.
#   verification (two traces):  nidseq.py <imports.toml> <trace> <oracle-trace> [--allow-prefix]
#       Default is exact sequence equality: exit 0 requires at least one import compared,
#       equal sequence lengths, and every import in agreement. Any of the following is a
#       nonzero failure: divergence, zero imports compared, either sequence empty, recomp
#       sequence a strict prefix of the oracle, oracle shorter than the recomp sequence,
#       an empty/absent import table, or malformed trace/import data.
#       --allow-prefix explicitly accepts a strict-prefix relation (documented prefix
#       analysis mode); it still requires at least one import compared.
#
# Usage: nidseq.py <imports.toml> <trace> [<oracle-trace>] [--allow-prefix]

import sys

import tomllib


DEFAULT_IMPORT_SCAN_LIMIT = 100000  # runaway bound for informational extraction only


class NidseqError(Exception):
    pass


def load_imports(path):
    try:
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
    except OSError as e:
        raise NidseqError(f"cannot read import map {path}: {e}") from None
    except tomllib.TOMLDecodeError as e:
        raise NidseqError(f"malformed import map {path}: {e}") from None
    table = data.get("import")
    if not isinstance(table, list):
        raise NidseqError(f"import map {path}: missing or non-list top-level 'import' table")
    m = {}
    stubs = []
    for i, e in enumerate(table):
        if not isinstance(e, dict) or not {"stub", "lib", "nid"} <= set(e):
            raise NidseqError(f"import map {path}: entry {i} lacks stub/lib/nid fields")
        stub, nid = e["stub"], e["nid"]
        if type(stub) is not int or not 0 <= stub < 2**32:
            raise NidseqError(f"import map {path}: entry {i} has a non-u32 stub value {stub!r}")
        if type(nid) is not int or not 0 <= nid < 2**32:
            raise NidseqError(f"import map {path}: entry {i} has a non-u32 nid value {nid!r}")
        if stub in m:
            raise NidseqError(f"import map {path}: duplicate stub address 0x{stub:08x}")
        m[stub] = (str(e["lib"]), nid)
        stubs.append(stub)
    if not stubs:
        raise NidseqError(f"import map {path}: empty import table")
    # Derive .sceStub.text range from the import table itself so the tool is
    # game-agnostic.  The original code hard-coded an ACX-specific address pair
    # (S0=0x08a246ac, S1=0x08a24dd4+8) which produced silent zero output for HST.
    # O(n) scan over stubs; acceptable for <10k entries, acceptable.
    s0, s1 = min(stubs), max(stubs) + 8
    return m, s0, s1


def nid_seq(trace, imp, s0, s1, limit=None):
    """Return the ordered [(stub_pc, (lib, nid)), ...] import-stub jr sequence.

    Every non-comment line must be a structurally valid trace record; blank, short,
    or malformed lines are hard errors so truncation/corruption can never silently
    shrink the observed import sequence (issue #381).
    """
    seq = []
    with open(trace) as fh:
        for lineno, line in enumerate(fh, 1):
            if line[:1] == "#":
                continue
            p = line.split()
            if len(p) < 3 or not p[1].startswith("pc=0x") or not p[2].startswith("op=0x"):
                raise NidseqError(
                    f"{trace}:{lineno}: malformed trace record (expected "
                    f"'<step> pc=0x... op=0x... ...'): {line!r}"
                )
            try:
                pc = int(p[1][3:], 16)
            except ValueError:
                raise NidseqError(f"{trace}:{lineno}: malformed pc field {p[1]!r}: {line!r}") from None
            if s0 <= pc < s1 and p[2] == "op=0x03e00008":  # the stub's jr $ra line
                seq.append((pc, imp.get(pc, ("?", 0))))
                if limit is not None and len(seq) >= limit:
                    break
    return seq


def verify_sequences(mine, orac, imp, allow_prefix):
    """Exact-equality verification by default; --allow-prefix permits strict prefixes.

    Returns the process exit code. Fail-closed: zero compared imports, length mismatch,
    or divergence is nonzero (issue #381).
    """
    n = min(len(mine), len(orac))
    if n == 0:
        print(
            f"FAIL: zero imports compared (recomp={len(mine)}, oracle={len(orac)}); "
            "no equivalence evidence",
            file=sys.stderr,
        )
        return 1
    for i in range(n):
        if mine[i][0] != orac[i][0]:
            rl, rn = imp.get(mine[i][0], ("?", 0))
            ol, on = imp.get(orac[i][0], ("?", 0))
            print(
                f"DIVERGE at import {i}: recomp={rl}.0x{rn:08x} " f"oracle={ol}.0x{on:08x}"
            )
            print(f"FAILED: import sequences diverge at index {i} of {n} compared")
            return 1
    if len(mine) == len(orac):
        print(f"VERIFIED: import sequences identical ({len(mine)} imports)")
        return 0
    if allow_prefix:
        if len(mine) < len(orac):
            print(
                f"PREFIX-MATCH (--allow-prefix): recomp sequence is a strict prefix of the "
                f"oracle sequence ({len(mine)} of {len(orac)} imports agree)"
            )
        else:
            print(
                f"PREFIX-MATCH (--allow-prefix): oracle sequence is a strict prefix of the "
                f"recomp sequence ({len(orac)} of {len(mine)} imports agree)"
            )
        return 0
    if len(mine) < len(orac):
        print(
            f"FAILED: recomp import sequence is a strict prefix of the oracle sequence "
            f"({len(mine)} of {len(orac)} imports; oracle continued beyond recomp coverage). "
            f"Use --allow-prefix to request documented prefix analysis.",
            file=sys.stderr,
        )
    else:
        print(
            f"FAILED: oracle import sequence is shorter than the recomp sequence "
            f"({len(orac)} of {len(mine)} imports; oracle ended before recomp coverage)",
            file=sys.stderr,
        )
    return 1


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    unknown = [o for o in opts if o != "--allow-prefix"]
    if unknown or not 2 <= len(args) <= 3:
        print(
            "usage: nidseq.py <imports.toml> <trace> [<oracle-trace>] [--allow-prefix]\n"
            "One trace: informational extraction (no equivalence claim, exit 0).\n"
            "Two traces: verification; exit 0 requires exact import-sequence equality with\n"
            "at least one import compared. --allow-prefix explicitly permits strict-prefix\n"
            "agreement instead of exact equality.",
            file=sys.stderr,
        )
        return 2
    allow_prefix = "--allow-prefix" in opts

    try:
        imp, s0, s1 = load_imports(args[0])
    except NidseqError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    print(f"stub range: 0x{s0:08x}..0x{s1:08x} ({len(imp)} imports)")

    informational = len(args) == 2
    try:
        mine = nid_seq(args[1], imp, s0, s1, limit=DEFAULT_IMPORT_SCAN_LIMIT if informational else None)
    except OSError as e:
        print(f"ERROR: cannot read trace {args[1]}: {e}", file=sys.stderr)
        return 2
    except NidseqError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print(f"{args[1]}: {len(mine)} imports")
    for i, (_pc, (lib, nid)) in enumerate(mine[:40]):
        print(f"  {i:3} {lib}.0x{nid:08x}")

    if informational:
        print("informational extraction mode: no oracle trace supplied; no equivalence claim made")
        return 0

    try:
        orac = nid_seq(args[2], imp, s0, s1)
    except OSError as e:
        print(f"ERROR: cannot read oracle trace {args[2]}: {e}", file=sys.stderr)
        return 2
    except NidseqError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print(f"{args[2]}: {len(orac)} imports (oracle)")
    return verify_sequences(mine, orac, imp, allow_prefix)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
