# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Make a ``compile_commands.json`` relocatable (issue #368).

A compilation database written by a tool carries the absolute checkout path in
``directory``, ``file`` and every argument. This tool rewrites every path under
the checkout root to a root-relative form and keeps the single absolute field
(``directory``) pointing at the checkout it is run for, so moving or cloning a
workspace needs only ``--root`` to be repointed, with no other path left over.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def _norm(text: str) -> str:
    return text.replace("\\", "/")


def _rel(value: str, old_root: str) -> str:
    """Return ``value`` with a leading ``old_root`` made root-relative."""
    norm = _norm(value)
    if norm == old_root:
        return "."
    prefix = old_root + "/"
    if norm.startswith(prefix):
        return norm[len(prefix):]
    # Flag forms such as -I/abs/path or -o/abs/path.
    match = re.match(r"^(-[A-Za-z])(.+)$", norm)
    if match and (match.group(2) == old_root or match.group(2).startswith(prefix)):
        return match.group(1) + _rel(match.group(2), old_root)
    return value


def relocate(entries: list[dict], old_root: str, new_root: str) -> list[dict]:
    """Relativise paths under ``old_root`` and set ``directory`` to ``new_root``."""
    old = _norm(old_root).rstrip("/")
    out = []
    for entry in entries:
        item = dict(entry)
        item["directory"] = _norm(new_root).rstrip("/")
        if "file" in item:
            item["file"] = _rel(item["file"], old)
        if "output" in item:
            item["output"] = _rel(item["output"], old)
        if "arguments" in item:
            item["arguments"] = [_rel(a, old) for a in item["arguments"]]
        if "command" in item:
            item["command"] = " ".join(_rel(a, old) for a in item["command"].split(" "))
        out.append(item)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path)
    parser.add_argument("--old-root", help="checkout root recorded in the database "
                        "(default: the database's first directory)")
    parser.add_argument("--root", type=Path, default=Path.cwd(),
                        help="checkout root to record (default: current directory)")
    args = parser.parse_args(argv)
    entries = json.loads(args.database.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        print("compdb_relocate: database is not a JSON list", file=sys.stderr)
        return 1
    old_root = args.old_root or (entries[0].get("directory") if entries else "") or ""
    result = relocate(entries, old_root, args.root.resolve().as_posix())
    args.database.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
