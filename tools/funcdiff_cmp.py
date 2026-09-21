# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-09-21.
# See NOTICE.md for upstream lineage and modification provenance.

# Compare a funcdiff output trace (steps renumbered from 0) against the oracle slice that
# starts at <entry-step>. Step numbers are ignored; pc/op and the set of register/memory
# writes must match. Reports the first divergence, or confirms N matching steps.
#
# Fail-closed contract (issue #381): exit 0 requires that at least one valid recomp step
# was compared AND that the oracle supplied the complete required slice from <entry-step>
# AND that every compared step matched. The oracle may legitimately continue beyond the
# requested recomp-length slice; oracle coverage must be >= recomp coverage from
# <entry-step> (not global equal file length). Empty or malformed input that would shrink
# the required coverage fails explicitly instead of reporting a zero-coverage MATCH.
#
# Usage: funcdiff_cmp.py <oracle-trace> <my-trace> <entry-step>
import sys


def norm(line, lineno, path):
    """Normalize one trace line to (pc, op, frozenset(writes)); raise on malformed input.

    Lines: "<step> pc=0x... op=0x... [rN=0x...|hi=0x...|lo=0x...|m<size>[0x...]=0x...]*"
    (the funcdiff/recomp.c emit format). Comment lines (leading '#') are skipped by the
    callers; blank lines and structurally malformed records are hard errors so that
    corruption can never silently shrink the required comparison coverage.
    """
    p = line.split()
    if len(p) < 3:
        raise TraceFormatError(f"{path}:{lineno}: malformed trace record (need >=3 fields): {line!r}")
    pc, op = p[1], p[2]
    if not pc.startswith("pc=0x") or not op.startswith("op=0x"):
        raise TraceFormatError(
            f"{path}:{lineno}: malformed trace record (expected 'pc=0x... op=0x...' after the step number): {line!r}"
        )
    writes = frozenset(p[3:])
    for w in writes:
        if "=" not in w:
            raise TraceFormatError(f"{path}:{lineno}: malformed write token {w!r}: {line!r}")
    return pc, op, writes


class TraceFormatError(Exception):
    pass


def load_my_trace(path):
    my = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            if line[:1] == "#":
                continue
            my.append(norm(line, lineno, path))
    return my


def load_oracle_slice(path, entry, need):
    """Return exactly the <need> oracle records starting at <entry-step>, or raise.

    The oracle file may contain further steps after the requested slice; only coverage
    from <entry-step> onward is required.
    """
    cur = []
    started = False
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            if line[:1] == "#":
                continue
            p = line.split()
            if len(p) < 3:
                raise TraceFormatError(
                    f"{path}:{lineno}: malformed oracle record (need >=3 fields): {line!r}"
                )
            try:
                st = int(p[0])
            except ValueError:
                raise TraceFormatError(
                    f"{path}:{lineno}: malformed oracle step number {p[0]!r}: {line!r}"
                ) from None
            if st < entry:
                continue
            started = True
            cur.append(norm(line, lineno, path))
            if len(cur) >= need:
                break
    if not started:
        raise TraceFormatError(f"{path}: oracle has no steps at or after entry-step {entry}")
    if len(cur) < need:
        raise TraceFormatError(
            f"{path}: oracle truncated before recomp trace ends: need {need} steps from entry-step {entry}, found {len(cur)}"
        )
    return cur


def main(argv):
    if len(argv) != 4:
        print(
            "usage: funcdiff_cmp.py <oracle-trace> <my-trace> <entry-step>\n"
            "Exit 0 only when >=1 step is compared, the oracle covers every recomp step from\n"
            "<entry-step> onward, and all steps match; any other outcome is nonzero.",
            file=sys.stderr,
        )
        return 2
    try:
        entry = int(argv[3])
        if entry < 0:
            raise ValueError
    except ValueError:
        print(f"ERROR: entry-step must be a nonnegative integer: {argv[3]!r}", file=sys.stderr)
        return 2
    oracle_path, mine_path = argv[1], argv[2]

    try:
        my = load_my_trace(mine_path)
    except OSError as e:
        print(f"ERROR: cannot read recomp trace: {e}", file=sys.stderr)
        return 2
    except TraceFormatError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    if not my:
        print("FAIL: empty recomp trace: zero steps to compare", file=sys.stderr)
        return 1

    try:
        cur = load_oracle_slice(oracle_path, entry, len(my))
    except OSError as e:
        print(f"ERROR: cannot read oracle trace: {e}", file=sys.stderr)
        return 2
    except TraceFormatError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    for i in range(len(my)):
        if my[i] != cur[i]:
            print(f"DIVERGENCE at my step {i} (oracle step {entry+i}):")
            print(f"  oracle: pc={cur[i][0]} op={cur[i][1]} writes={sorted(cur[i][2])}")
            print(f"  recomp: pc={my[i][0]} op={my[i][1]} writes={sorted(my[i][2])}")
            return 1

    print(f"MATCH: {len(my)} steps identical (recomp ran {len(my)} steps from oracle step {entry})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
