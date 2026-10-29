#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Keep game addresses out of the core runtime.

The runtime under ``src/rt/`` is meant to be title-independent: a fix for one game goes
in the generic code path or in that game's title config, never as a hard-coded guest
address in the core.  This gate fails on any hexadecimal literal in ``src/rt/`` C code
whose value lies in PSP user RAM (``0x08800000``-``0x09ffffff``, the user partition a game
loads into), outside the places where such a value is expected:

* title-config files (``TITLE_CONFIG_FILES``), which is where per-title data belongs;
* native self-tests (``*_selftest.c`` / ``*_selftest.h``), whose guest addresses are
  synthetic fixtures;
* registered NIDs: a NID is a 32-bit hash, not an address, and some hashes fall in the
  range.  A literal whose value is a NID in ``src/rt/nid_names.h`` (generated from the HLE
  registrations by ``tools/regen_all.py``) is skipped, and that table itself is exempt;
* the reviewed ``ALLOWLIST`` below: one entry per (file, value) with the exact number of
  occurrences and the reason it is not a game address.

Comments and string literals are not code and are not scanned: measured addresses quoted
in a comment (``cpu_lle.c`` cites hardware runs) change no behaviour.  An allowlist entry
that no longer matches its file is itself a failure, so the list cannot go stale.

CLI: ``python tools/purity_gate.py`` prints findings and exits 1 when there are any.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCAN_DIR = "src/rt"
SCAN_SUFFIXES = (".c", ".h", ".inc")
USER_RAM_FIRST = 0x08800000
USER_RAM_LAST = 0x09FFFFFF

TITLE_CONFIG_FILES = ("src/rt/title_config.c", "src/rt/title_config.h")
SELFTEST_PATTERNS = ("src/rt/*_selftest.c", "src/rt/*_selftest.h")
NID_TABLE = "src/rt/nid_names.h"


@dataclass(frozen=True)
class Allowed:
    path: str
    value: int
    count: int
    reason: str


# Every literal in src/rt/ production code that is in user RAM today, reviewed 2026-10-28.
# None names a game: each is part of the generic PSP memory map, a debug option, or a
# test-build-only hook.  Add an entry only with a reason a reviewer can check.
ALLOWLIST: tuple[Allowed, ...] = (
    Allowed("src/rt/driver.c", 0x09F00000, 2,
            "generic boot default for the main thread's $sp near the top of the user "
            "partition, used when the loaded module supplies no stack (direct boot and "
            "LoadExec restart); not title-specific"),
    Allowed("src/rt/driver.c", 0x08800000, 1,
            "start of the PSP user partition: the loaded-image floor for a LoadExec "
            "replacement module before its own loader supplies the end"),
    Allowed("src/rt/driver.c", 0x09000000, 1,
            "SR_PPM_DUMP debug option only: dumps a 64x64 block at a fixed address; never "
            "runs in a normal session (debt: read the display framebuffer address instead)"),
    Allowed("src/rt/sched.c", 0x08800000, 1,
            "SR_STACK_ARENA_FLOOR: start of the PSP user partition, the generic floor of the "
            "thread-stack arena"),
    Allowed("src/rt/hle.c", 0x08FFDF00, 1,
            "HLE_TEST_SCHED_CALL_ENTRY: synthetic guest entry inside #ifdef "
            "SR_HLE_THREAD_SELFTEST, compiled only into the thread self-test"),
)

_HEX = re.compile(r"\b0[xX]([0-9a-fA-F]+)[uUlL]*\b")
_NID_ROW = re.compile(r"\{\s*0[xX]([0-9a-fA-F]{1,8})[uU]?\s*,")


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    value: int
    text: str

    def render(self) -> str:
        return (f"{self.path}:{self.line}: PSP user-RAM address literal 0x{self.value:08x} in "
                f"the core runtime: {self.text.strip()}")


def strip_comments_and_strings(source: str) -> str:
    """Blank out comments and string/char literals, keeping every newline in place."""
    out: list[str] = []
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = source.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif ch == "/" and nxt == "*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in source[i:end]))
            i = end
        elif ch in "\"'":
            j = i + 1
            while j < n and source[j] != ch and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def user_ram_literals(source: str) -> list[tuple[int, int, str]]:
    """(line, value, original line) for each user-RAM hex literal in code."""
    code_lines = strip_comments_and_strings(source).split("\n")
    raw_lines = source.split("\n")
    found = []
    for number, line in enumerate(code_lines, start=1):
        for match in _HEX.finditer(line):
            value = int(match.group(1), 16)
            if USER_RAM_FIRST <= value <= USER_RAM_LAST:
                found.append((number, value, raw_lines[number - 1]))
    return found


def registered_nids(root: Path) -> frozenset[int]:
    table = root / NID_TABLE
    if not table.is_file():
        return frozenset()
    return frozenset(int(m.group(1), 16) for m in _NID_ROW.finditer(table.read_text(encoding="utf-8")))


def _exempt(path: str) -> bool:
    if path in TITLE_CONFIG_FILES or path == NID_TABLE:
        return True
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in SELFTEST_PATTERNS)


def scan(root: Path, allowlist: tuple[Allowed, ...] = ALLOWLIST) -> list[str]:
    """Return one message per violation (unlisted literal, count drift, stale entry)."""
    nids = registered_nids(root)
    scan_root = root / SCAN_DIR
    files = sorted(p for p in scan_root.rglob("*") if p.is_file() and p.suffix in SCAN_SUFFIXES)
    findings: list[Finding] = []
    for file in files:
        rel = file.relative_to(root).as_posix()
        if _exempt(rel):
            continue
        source = file.read_text(encoding="utf-8", errors="replace")
        for line, value, text in user_ram_literals(source):
            if value in nids:
                continue
            findings.append(Finding(rel, line, value, text))

    allowed = {(entry.path, entry.value): entry for entry in allowlist}
    counts = Counter((f.path, f.value) for f in findings)
    messages = []
    for finding in findings:
        if (finding.path, finding.value) not in allowed:
            messages.append(finding.render())
    for key, entry in sorted(allowed.items()):
        seen = counts.get(key, 0)
        if seen != entry.count:
            messages.append(
                f"{entry.path}: allowlist entry 0x{entry.value:08x} expects {entry.count} "
                f"occurrence(s), found {seen}; review the change and update the entry in "
                f"tools/purity_gate.py")
    return messages


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    args = parser.parse_args(argv)
    messages = scan(args.root)
    for message in messages:
        print(message)
    if messages:
        print(f"purity gate: FAIL ({len(messages)} finding(s)). A game address belongs in the "
              f"title config, not in {SCAN_DIR}/; see tools/purity_gate.py.")
        return 1
    print(f"purity gate: PASS ({len(ALLOWLIST)} reviewed allowlist entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
