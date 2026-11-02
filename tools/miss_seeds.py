#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Dispatch-miss seeds: close the loop from a run back into analysis (#1848).

When a run reaches guest code the analyzer did not attribute to a function, the dispatcher
reaches it only as a miss and the run falls to the interpreter floor (docs/DEBUGGING.md,
"Static coverage" and "Execution share"). The runtime writes the distinct miss targets of a run
to the file named by ``SR_MISS_SEEDS``; this tool merges those files into one per-title seed list;
``tools/analyze.py`` and ``tools/codegen.py`` take the list as extra function entries
(``--miss-seeds=FILE``) on the next build.

Seeding only adds analysis input. A miss stays a visible, fail-closed event at run time
(AGENTS.md Section 6), and a seed the analysis cannot use is reported and ignored, never forced.
A seed list is derived from a title's code and stays private (AGENTS.md Section 5): keep it with
the title's other private build inputs.

File format (ASCII, one record per line, ``#`` starts a comment)::

    # nakagawa-miss-seeds-v1
    0x08804068 run=smoke-1

The runtime writes the address alone; ``merge`` adds ``run=<label>``, the run that first found the
seed. Addresses are guest addresses of the image the run executed.

    miss_seeds.py merge --out SEEDS --run LABEL [--elf ELF [--base HEX]] MISS_FILE...
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HEADER = "# nakagawa-miss-seeds-v1"
_RECORD = re.compile(r"^(0x[0-9a-fA-F]{1,8})(?:[ \t]+run=([A-Za-z0-9_.-]+))?[ \t]*$")
_OVERFLOW = re.compile(r"\bentry_points_overflow=(\d+)")
UNKNOWN_RUN = "unknown"


class SeedFileError(ValueError):
    """A seed file that is not in the documented format."""


class SeedFile:
    """The seeds of one file: address to the run that found it, and any reported overflow."""

    def __init__(self, seeds=None, overflow=0):
        self.seeds = dict(seeds or {})
        self.overflow = overflow


def parse_seed_text(text: str, source: str = "<seed file>") -> SeedFile:
    parsed = SeedFile()
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            found = _OVERFLOW.search(stripped)
            if found:
                parsed.overflow += int(found.group(1))
            continue
        match = _RECORD.match(stripped)
        if match is None:
            raise SeedFileError(f"{source}:{number}: not a seed record: {stripped!r}")
        address = int(match.group(1), 16)
        parsed.seeds.setdefault(address, match.group(2) or UNKNOWN_RUN)
    return parsed


def read_seed_file(path) -> SeedFile:
    try:
        text = Path(path).read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise SeedFileError(f"{path}: cannot read the seed file: {exc}") from exc
    return parse_seed_text(text, str(path))


def format_seed_file(seeds: dict[int, str]) -> str:
    lines = [HEADER]
    lines.extend(f"0x{address:08x} run={run}" for address, run in sorted(seeds.items()))
    return "\n".join(lines) + "\n"


def known_entries(elf, extra_spans=None):
    """The function entries the analysis finds without seeds. A target that is already an entry
    gains nothing from a seed, so ``merge`` drops it. A target inside a function's body is kept:
    a run entered the body mid-way (a computed jump into a switch arm, say), the dispatcher has
    no body registered at that address, and a seed gives it one."""
    import analyze

    functions, _ranges = analyze.analyze(elf, extra_spans=extra_spans)
    return set(functions)


def report_outcome(outcome, prefix="MISS_SEEDS_APPLIED"):
    """Print what analysis did with the seeds: one summary line, then one line per ignored seed.

    ``outcome`` maps each seed address to its ``analyze`` status (``applied``, ``known`` or the
    reason it was ignored). The summary goes to stdout, the ignored seeds to stderr.
    """
    import analyze

    counts = {"applied": 0, "known": 0, "ignored": 0}
    for address, status in sorted(outcome.items()):
        if status in (analyze.SEED_APPLIED, analyze.SEED_KNOWN):
            counts[status] += 1
        else:
            counts["ignored"] += 1
            sys.stderr.write(f"{prefix}: ignored 0x{address:08x}: {status}\n")
    print(f"{prefix} seeds={len(outcome)} applied={counts['applied']} known={counts['known']} "
          f"ignored={counts['ignored']}")


def merge(existing: SeedFile, miss_files, run: str, known=frozenset()):
    """Merge the miss files into ``existing``; returns (seeds, added, dropped, overflow)."""
    seeds = dict(existing.seeds)
    overflow = existing.overflow
    added, dropped = [], []
    for miss in miss_files:
        overflow += miss.overflow
        for address in sorted(miss.seeds):
            if address in known:
                dropped.append(address)
            elif address not in seeds:
                seeds[address] = run
                added.append(address)
    return seeds, added, dropped, overflow


def _build_parser():
    parser = argparse.ArgumentParser(prog="miss_seeds.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    merge_parser = sub.add_parser("merge", help="merge run miss files into a per-title seed list")
    merge_parser.add_argument("miss_files", nargs="+", metavar="MISS_FILE",
                              help="a file written by a run with SR_MISS_SEEDS set")
    merge_parser.add_argument("--out", required=True, metavar="SEEDS",
                              help="the seed list to create or extend")
    merge_parser.add_argument("--run", required=True, metavar="LABEL",
                              help="a label for these runs, recorded with each new seed")
    merge_parser.add_argument("--elf", metavar="ELF", default=None,
                              help="the image the runs executed: drop targets that are already function entries")
    merge_parser.add_argument("--base", metavar="HEX", type=lambda text: int(text, 16),
                              default=None, help="load base of a relocatable image, in hex")
    return parser


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.run):
        sys.stderr.write("miss_seeds: --run takes letters, digits, '.', '_' and '-' only\n")
        return 2
    out_path = Path(args.out)
    try:
        existing = read_seed_file(out_path) if out_path.exists() else SeedFile()
        miss_files = [read_seed_file(path) for path in args.miss_files]
    except SeedFileError as exc:
        sys.stderr.write(f"miss_seeds: {exc}\n")
        return 1
    known: frozenset[int] = frozenset()
    if args.elf is not None:
        import analyze

        elf = analyze.Elf(args.elf, base=args.base)
        known = frozenset(known_entries(elf, analyze.resolve_extra_spans(None)))
    seeds, added, dropped, overflow = merge(existing, miss_files, args.run, known)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(format_seed_file(seeds), encoding="ascii", newline="\n")
    print(f"MISS_SEEDS merged added={len(added)} dropped_known={len(dropped)} "
          f"total={len(seeds)} out={out_path.name}")
    if overflow:
        sys.stderr.write(
            f"miss_seeds: a run reported {overflow} interpreter entries that did not fit its "
            "entry-point set; the miss list is incomplete, so run again after rebuilding\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
