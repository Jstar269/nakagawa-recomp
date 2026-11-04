#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The root Makefile as one text, with its ``mk/*.mk`` fragments inlined.

The root ``Makefile`` includes its fragments with a literal ``include mk/<name>.mk`` line at the point where the
text used to be, so GNU Make evaluates the files in the same order as before the split (#1531). Tests and tools
that read the build text for a variable, a target or a recipe line read it through :func:`read_makefile` and see
the same single text, in the same order, wherever the line lives now.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

_INCLUDE_RE = re.compile(r"^(?P<indent>[ \t]*)-?include[ \t]+(?P<path>mk/[A-Za-z0-9_./-]+\.mk)[ \t]*$")


def _inline(root: Path, path: Path, seen: tuple[Path, ...], encoding: str, errors: str) -> str:
    if path in seen:
        raise ValueError(f"include cycle through {path}")
    text = path.read_text(encoding=encoding, errors=errors)
    out: list[str] = []
    for line in text.split("\n"):
        match = _INCLUDE_RE.match(line)
        if match is not None and (root / match.group("path")).is_file():
            out.append(_inline(root, root / match.group("path"), seen + (path,), encoding, errors).rstrip("\n"))
        else:
            out.append(line)
    return "\n".join(out)


def read_makefile(root: Path | None = None, encoding: str = "utf-8", errors: str = "strict") -> str:
    """The root ``Makefile`` of ``root`` (the repository by default) with every ``include mk/*.mk`` inlined."""
    base = ROOT if root is None else Path(root)
    return _inline(base, base / "Makefile", (), encoding, errors)


def makefile_paths(root: Path | None = None) -> list[Path]:
    """The root ``Makefile`` and every ``mk/*.mk`` fragment, in a stable order."""
    base = ROOT if root is None else Path(root)
    return [base / "Makefile", *sorted((base / "mk").glob("*.mk"))]
