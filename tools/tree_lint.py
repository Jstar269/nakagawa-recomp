#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Report tracked files that no other tracked file names.

Dead-file detection for the public tree.  A tracked file is *referenced* when a
path-shaped token in another tracked text file spells its path, its basename, or its
importable module name.  The generated inventories -- the publication policy, the
provenance ledger, the export control and the inherited-file notice manifest -- list
every tracked path by construction, so they are not read as sources of references:
without that exclusion every file looks used and the report is always empty.

``tools/tree_lint_allow.json`` is excluded for the same reason and by the same rule:
it records a reviewed decision *about* a path rather than using it.  The caller knows
where it found the allowlist and passes it in, because the path is a CLI argument.  A
committed allowlist is otherwise its own worst reference -- every entry it carries
would be reported as referenced, and the reason recorded for it would become a no-op.

Four spellings count, because the tree uses all four.  ``tools/codegen.py`` is the
repository-relative path, which is how a Makefile and a policy name an exact file.
``codegen.py`` is the basename, which is how a tool is invoked and how a ``#include``
names a vendored header.  ``tools.codegen`` is the module name, which is how Python
spells an import (``from tools import codegen``).  A *relative* reference names only
a trailing part of the path -- a Markdown link ``provenance/HST_PUBLIC_CENSUS.md``,
an ``#include "libavutil/version.h"`` -- and an import may drop a leading component,
because both ``tools/`` and the repository root are import roots
(``from psp_oracle.parse_golden import ...``).  Matching only whole paths and
basenames would call every imported and every relatively-linked file dead.

A basename or stem shared by several tracked files -- ``Makefile``, ``main.c``,
``__init__.py``, ``version.h`` -- is a *false friend*: the token says one of them is
used and nothing about which.  Such a match is reported as its own class, so the file
is neither declared dead nor declared live.  Every other file is one of two things: an
unambiguous match makes it referenced, and no match at all makes it unreferenced.
Collapsing those three states into one would either hide the fixtures no build
touches or invent references nobody wrote.

The match is textual, so a token can in principle keep a file alive by accident.  That
is what ``tools/tree_lint_allow.json`` is for: it records, with a one-line reason,
every file consumed by name rather than referenced from the tree -- dotfiles and
configuration read by an external tool, the legal records, the vendored include
graph, the unittest discovery glob.  Allowlisted files are still printed, so the
allowlist stays visible instead of silently becoming the answer.

The reference index is built in a single pass over the tracked tree: each file is
read once, split into path-shaped tokens, and every token is looked up in one table.
Nothing here calls ``git grep`` per candidate, so the cost is linear in the size of
the tree rather than quadratic in the number of candidates.

Exit status is 0 for the reporting default, whatever the tree contains; 1 only
under ``--check`` when at least one tracked file lacks an unambiguous reference and is
not allowlisted; and 2 when the tree or the allowlist cannot be read or validated
(a tracked tree that cannot be read is an error, never an empty report).

The Makefile target runs the reporting form so a local inspection never fails
solely because it found candidates.  The always-on CI hygiene job runs
``--check``: each candidate must be removed or receive a reviewed, checkable allowlist
reason before the change can land.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Collection
from fnmatch import fnmatchcase
from itertools import groupby
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nk_core.git_isolation import run_git  # noqa: E402

DEFAULT_ALLOWLIST = Path(__file__).resolve().parent / "tree_lint_allow.json"

#: Generated inventories that name every tracked path by construction.  They are
#: excluded as *sources* of references, so a path they list is not thereby
#: considered used.  They are still reported on their own merits.
GENERATED_INVENTORIES = frozenset({
    "assets/public_provenance_ledger.json",
    "assets/public_source_profile.json",
    "PUBLIC_EXPORT.json",
    "docs/provenance/MODIFIED_FILE_NOTICES.json",
})

#: A path-shaped token over the character subset this tree's paths use (letters,
#: digits, ``_ . + -`` and ``/``).  Git permits more (``@``, ``#``, ``~``, spaces, ...):
#: a path with such a character would be split into fragments and reported
#: unreferenced, a false positive, until this class is widened.  The repetition is
#: greedy, so ``tools/codegen.py`` arrives as one token, not three.
TOKEN_RE = re.compile(r"[A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+)*")

#: Outcome classes.  ``REFERENCED`` is the only one that means a file is in use.
REFERENCED = "referenced"
AMBIGUOUS = "ambiguous-basename"
UNREFERENCED = "unreferenced"


class TreeLintError(RuntimeError):
    """A tracked tree that cannot be read is reported, never guessed around."""


def tracked_files(root: Path) -> list[str]:
    """Every path the index knows about, in git's own order."""
    result = run_git(["ls-files"], cwd=root, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise TreeLintError(f"git ls-files failed in {root}: {result.stderr.strip() or result.returncode}")
    return [line for line in result.stdout.splitlines() if line.strip()]


def _is_scannable(blob: bytes) -> bool:
    """True when a blob can carry a textual path reference.

    A NUL byte is the same signal every text/binary sniffing tool uses, and the
    tracked binary payloads here are VFPU lookup tables and pinned prebuilt
    fixtures -- none of which names a source path.
    """
    return b"\0" not in blob


def _spelling_keys(path: str) -> set[str]:
    """Every spelling by which another file can name ``path``.

    A tracked file is named four ways in this tree, and each has its own spelling.
    Its repository-relative path is what a Makefile and a policy use.  Any trailing
    suffix of that path is what a *relative* reference uses -- a Markdown link
    ``provenance/HST_PUBLIC_CENSUS.md`` and a ``#include "libavutil/version.h"`` both
    name a file without its repository prefix.  Its module name is what Python uses,
    spelled with dots (``from tools import codegen``) and, because ``tools/`` and the
    repository root are both import roots, with any leading component dropped
    (``from psp_oracle.parse_golden import ...``); a relative import
    (``from .types import ...``) names the bare stem.  A bare stem is the loosest key
    and errs toward calling a file referenced, which is the right direction for a
    report whose output is a list of candidates for a human to judge.
    """
    pure = PurePosixPath(path)
    parts = pure.parts
    keys = {path, pure.name, pure.stem}
    for cut in range(len(parts)):
        suffix = "/".join(parts[cut:])
        dotted = suffix.removesuffix(pure.suffix) if pure.suffix else suffix
        keys.add(suffix)
        keys.add(dotted)
        keys.add(dotted.replace("/", "."))
    return keys


def _tree_relative(path: Path, root: Path) -> str | None:
    """``path`` as a tracked-tree-relative POSIX path, or None when it is outside.

    The allowlist is chosen with ``--allowlist`` and may live anywhere, including a
    scratch directory outside the tree, so this is a question rather than a lookup.
    """
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def build_reference_index(
    root: Path,
    tracked: list[str],
    *,
    ignored_sources: Collection[str] = frozenset(),
) -> dict[str, dict[str, object]]:
    """Classify every tracked path from the files that name it.

    One pass over the tree: each token is looked up once per key table, so the cost
    is linear in the size of the tree rather than quadratic in the number of
    candidates.  Only the paths that are *not* referenced come back, each already
    classified and carrying the files that wrote the token that came closest.

    ``ignored_sources`` names tracked paths whose text is not read as a source of
    references: the generated inventories, and the allowlist the caller loaded.  They
    are still judged on their own merit, exactly like the inventories.
    """
    keys: dict[str, list[str]] = {}
    for path in tracked:
        for key in _spelling_keys(path):
            owners = keys.setdefault(key, [])
            if path not in owners:
                owners.append(path)
    contested = {key for key, owners in keys.items() if len(owners) > 1}

    index: dict[str, dict[str, object]] = {path: {"status": UNREFERENCED, "referenced_by": set()} for path in tracked}
    for source in tracked:
        if source in GENERATED_INVENTORIES or source in ignored_sources:
            continue
        source_path = root / source
        # A tracked symlink's target may be outside the checkout. Its link text
        # is not a source file and must never be read by this repository-wide scan.
        if source_path.is_symlink():
            continue
        blob = source_path.read_bytes()
        if not _is_scannable(blob):
            continue
        for raw_token in TOKEN_RE.findall(blob.decode("utf-8", errors="replace")):
            token = raw_token.rstrip(".")
            if not token:
                continue
            owners = keys.get(token)
            if not owners:
                continue
            if token in contested:
                # A shared spelling says one of these files is named, not which.
                for target in owners:
                    if target != source and index[target]["status"] == UNREFERENCED:
                        index[target]["referenced_by"].add(source)
                continue
            for target in owners:
                if target != source and index[target]["status"] == UNREFERENCED:
                    index[target]["status"] = REFERENCED
                    index[target]["referenced_by"].add(source)

    return {
        path: {
            "status": AMBIGUOUS if entry["referenced_by"] else UNREFERENCED,
            "referenced_by": sorted(entry["referenced_by"]),
        }
        for path, entry in index.items() if entry["status"] != REFERENCED
    }



def load_allowlist(path: Path) -> dict[str, str]:
    """Read the reviewed allowlist as ``pattern -> reason``.

    A pattern may be an exact path or contain ``*`` wildcards, because whole
    families of files are consumed by a glob rather than by name -- the unittest
    discovery contract is the reason every ``tools/test_*.py`` module is live.
    A malformed allowlist is an error rather than an empty set: silently allowing
    nothing would report the whole tree, and silently allowing everything would
    hide every dead file.
    """
    if path.is_symlink():
        raise TreeLintError(f"{path.name}: allowlist must not be a symlink")
    if not path.is_file():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    if (
        not isinstance(document, dict)
        or type(document.get("schema_version")) is not int
        or document.get("schema_version") != 1
        or set(document) - {"schema_version", "description", "entries"}
        or ("description" in document and not isinstance(document["description"], str))
    ):
        raise TreeLintError(f"{path.name}: unsupported allowlist document")
    entries = document.get("entries")
    if not isinstance(entries, list):
        raise TreeLintError(f"{path.name}: 'entries' must be a list")
    allowed: dict[str, str] = {}
    for entry in entries:
        if (
            not isinstance(entry, dict)
            or set(entry) != {"path", "reason"}
            or not isinstance(entry.get("path"), str)
            or not isinstance(entry.get("reason"), str)
        ):
            raise TreeLintError(f"{path.name}: every entry needs a string 'path' and a string 'reason'")
        pattern = entry["path"]
        reason = entry["reason"]
        if (
            not pattern
            or pattern == "*"
            or pattern.startswith("/")
            or "\\" in pattern
            or ":" in pattern
            or any(character in pattern for character in "?[]")
            or any(ord(character) < 32 or ord(character) == 127 for character in pattern)
            or any(part in {"", ".", ".."} for part in pattern.split("/"))
        ):
            raise TreeLintError(f"{path.name}: allowlist path must be a relative repository path")
        if not reason.strip() or "\n" in reason or "\r" in reason:
            raise TreeLintError(f"{path.name}: allowlist reason must be a non-empty single line")
        if pattern in allowed:
            raise TreeLintError(f"{path.name}: duplicate allowlist path {pattern!r}")
        allowed[pattern] = reason
    return allowed


def _pattern_matches(path: str, pattern: str) -> bool:
    """Match segment by segment, case-sensitively, so ``*`` never crosses ``/``.

    ``fnmatch`` translates ``*`` to ``.*`` (crossing directory separators) and
    normalises case on Windows, which would let ``tools/*/__init__.py`` excuse
    ``tools/a/b/__init__.py`` or ``Makefile`` excuse ``makefile``.
    """
    if "*" not in pattern:
        return path == pattern
    path_parts = path.split("/")
    pattern_parts = pattern.split("/")
    return len(path_parts) == len(pattern_parts) and all(
        fnmatchcase(part, expected) for part, expected in zip(path_parts, pattern_parts, strict=True)
    )


def _allowing_pattern(path: str, allowed: dict[str, str]) -> str | None:
    for pattern in allowed:
        if _pattern_matches(path, pattern):
            return pattern
    return None


def unreferenced(
    root: Path,
    tracked: list[str],
    allowed: dict[str, str],
    *,
    ignored_sources: Collection[str] = frozenset(),
) -> list[dict[str, object]]:
    """Every tracked path nothing names unambiguously, allowlisted ones included."""
    findings = []
    for path, entry in build_reference_index(root, tracked, ignored_sources=ignored_sources).items():
        pattern = _allowing_pattern(path, allowed)
        findings.append({
            **entry,
            "path": path,
            "allowlisted": pattern is not None,
            "pattern": pattern,
            "reason": allowed.get(pattern) if pattern is not None else None,
        })
    return findings


def report_text(findings: list[dict[str, object]], tracked: list[str], check: bool) -> str:
    live = [entry for entry in findings if not entry["allowlisted"]]
    dead = [entry for entry in live if entry["status"] == UNREFERENCED]
    ambiguous = [entry for entry in live if entry["status"] == AMBIGUOUS]
    excused = [entry for entry in findings if entry["allowlisted"]]
    lines = [
        f"tree-lint: {len(tracked)} tracked file(s); {len(tracked) - len(findings)} referenced; "
        f"{len(dead)} unreferenced; {len(ambiguous)} ambiguous-basename; {len(excused)} allowlisted"
    ]
    for entry in dead:
        lines.append(f"  unreferenced: {entry['path']}")
    for entry in ambiguous:
        named = ", ".join(str(source) for source in entry["referenced_by"][:3])
        lines.append(f"  ambiguous-basename: {entry['path']} (its basename is also written in {named})")
    grouped = groupby(sorted(excused, key=lambda entry: str(entry["pattern"])), key=lambda entry: str(entry["pattern"]))
    for pattern, group in grouped:
        entries = list(group)
        paths = [str(entry["path"]) for entry in entries]
        shown = ", ".join(paths[:4]) + (f", and {len(paths) - 4} more" if len(paths) > 4 else "")
        lines.append(f"  allowlisted by {pattern}: {len(paths)} file(s) -- {entries[0]['reason']}")
        lines.append(f"    {shown}")
    if not live:
        lines.append("  no un-allowlisted file lacks an unambiguous reference")
    if check and live:
        lines.append("tree-lint: FAIL -- --check requires every file without an unambiguous reference to be allowlisted or removed")
    elif check:
        lines.append("tree-lint: OK (--check)")
    else:
        lines.append("tree-lint: OK (reporting only)")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT, help="tracked tree to inspect (default: this repository)")
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST, help="reviewed allowlist (default: tools/tree_lint_allow.json)")
    parser.add_argument("--json", action="store_true", help="emit the findings as JSON instead of text")
    parser.add_argument("--check", action="store_true", help="exit 1 when a tracked file lacks an unambiguous reference and is not allowlisted")
    args = parser.parse_args(argv)

    try:
        tracked = tracked_files(args.root)
        allowed = load_allowlist(args.allowlist)
        # A tracked allowlist is a decision record, not a user of the tree: exclude
        # it as a source so its own entries keep printing with their reasons.
        allowlist_source = _tree_relative(args.allowlist, args.root)
        ignored = frozenset({allowlist_source}) if allowlist_source else frozenset()
        findings = unreferenced(args.root, tracked, allowed, ignored_sources=ignored)
    except (OSError, TreeLintError, ValueError) as error:
        print(f"tree-lint: ERROR -- {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps({
            "tracked": len(tracked),
            "unreferenced": [
                {"path": entry["path"], "status": entry["status"]} for entry in findings if not entry["allowlisted"]
            ],
            "allowlisted_unreferenced": [
                {"path": entry["path"], "status": entry["status"], "pattern": entry["pattern"], "reason": entry["reason"]}
                for entry in findings if entry["allowlisted"]
            ],
        }, indent=2))
    else:
        print(report_text(findings, tracked, args.check))

    if args.check and any(not entry["allowlisted"] for entry in findings):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
