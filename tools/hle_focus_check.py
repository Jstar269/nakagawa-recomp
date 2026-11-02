#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Prove that a focused HLE self-test executable links only the HLE files it names (issue #307).

A focused build (``make psmf-media-focused-selftest``) links one subsystem's HLE files plus a
small stub file instead of every file in ``tools/hle_sources.py``. This check reads symbol
tables, not source text: the linked executable's, and those of the object files compiled from
the excluded HLE files. It fails when

* an excluded HLE file is missing its object (nothing to compare against: fail closed),
* the executable defines a symbol that an excluded HLE file defines and no included file or
  stub defines, which means the excluded file, or a copy of its code, was linked, or
* a ``--require`` symbol is absent from the executable, which would make the first test
  pass on an executable that does not hold the subsystem at all.

CLI: ``python tools/hle_focus_check.py --exe E --objdir D --include FILE... [--stub OBJ...]
[--require SYMBOL...]``. Exit 0 when it holds, 1 when it does not.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402


class FocusError(Exception):
    """The check could not be made, which is a failure, not a pass."""


def defined_symbols(path: Path, nm: str) -> set[str]:
    """External symbols defined by an object or executable, from ``nm -g --defined-only``."""
    try:
        result = subprocess.run([nm, "-g", "--defined-only", str(path)], capture_output=True,
                                text=True, check=False)
    except OSError as error:
        raise FocusError(f"cannot run {nm}: {error}") from error
    if result.returncode != 0:
        raise FocusError(f"{nm} failed on {path}: {result.stderr.strip()}")
    names: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and not fields[2].startswith(".refptr."):   # MinGW import pseudo-symbols
            names.add(fields[2])
    return names


def stem(source: str) -> str:
    return Path(source).stem


def check(exe: Path, objdir: Path, included: list[str], stubs: list[Path], required: list[str],
          nm: str, sources: list[str] | None = None) -> list[str]:
    """Return the problems found; an empty list means the executable is focused."""
    if sources is None:
        sources = list(hle_sources.RUNTIME_HLE_SOURCES)
    names = {stem(source): source for source in sources}
    unknown = sorted(stem(source) for source in included if stem(source) not in names)
    if unknown:
        raise FocusError(f"not HLE files listed by tools/hle_sources.py: {', '.join(unknown)}")
    if not included:
        raise FocusError("no included HLE file named")
    excluded = sorted(set(names) - {stem(source) for source in included})
    allowed: set[str] = set()
    for source in included:
        allowed |= defined_symbols(objdir / f"{stem(source)}.o", nm)
    for stub in stubs:
        allowed |= defined_symbols(stub, nm)
    linked = defined_symbols(exe, nm)
    problems: list[str] = []
    for name in excluded:
        obj = objdir / f"{name}.o"
        if not obj.is_file():
            raise FocusError(f"no object for the excluded file {names[name]} at {obj}")
        leaked = sorted((defined_symbols(obj, nm) - allowed) & linked)
        if leaked:
            problems.append(f"{names[name]} is linked: {', '.join(leaked[:6])}"
                            + (f" and {len(leaked) - 6} more" if len(leaked) > 6 else ""))
    for symbol in required:
        if symbol not in linked:
            problems.append(f"required symbol {symbol} is not defined in {exe.name}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--objdir", type=Path, required=True)
    parser.add_argument("--include", nargs="+", required=True)
    parser.add_argument("--stub", nargs="*", type=Path, default=[])
    parser.add_argument("--require", nargs="*", default=[])
    parser.add_argument("--nm", default=shutil.which("nm") or "nm")
    args = parser.parse_args(argv)
    try:
        problems = check(args.exe, args.objdir, args.include, args.stub, args.require, args.nm)
    except FocusError as error:
        print(f"hle_focus_check: INCOMPLETE: {error}", file=sys.stderr)
        return 1
    if problems:
        for problem in problems:
            print(f"hle_focus_check: FAIL: {problem}", file=sys.stderr)
        return 1
    total = len(hle_sources.RUNTIME_HLE_SOURCES)
    print(f"hle_focus_check: {args.exe.name} links {len(args.include)} of {total} HLE files; "
          f"the other {total - len(args.include)} are absent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
