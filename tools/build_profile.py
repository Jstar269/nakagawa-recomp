#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Hash and record compiler profiles used by the native build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def compiler_identity(command: str) -> dict[str, str]:
    """Return stable identity data for a compiler command."""
    words = shlex.split(command, posix=True)
    if not words:
        raise ValueError("compiler command is empty")
    executable = shutil.which(words[0]) or words[0]
    try:
        proc = subprocess.run(
            [executable, *words[1:], "--version"],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
        version = (proc.stdout + proc.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        version = f"unavailable: {type(exc).__name__}: {exc}"
    return {
        "command": command,
        "executable": str(Path(executable).resolve()) if Path(executable).exists() else executable,
        "version": version,
    }


def profile_payload(compiler: str, entries: list[str]) -> dict:
    return {
        "compiler": compiler_identity(compiler),
        "entries": entries,
    }


def profile_hash(payload: dict) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()[:20]


def record_profile(path: Path, section: str, payload: dict) -> None:
    document: dict = {"schema_version": 1, "sections": {}}
    if path.is_file():
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("schema_version") != 1 or not isinstance(document.get("sections"), dict):
            raise ValueError(f"unsupported build-profile manifest: {path}")
    document["sections"][section] = {
        "profile_hash": profile_hash(payload),
        **payload,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    os.replace(temporary, path)


def activate_stamp(
    path: Path,
    stale_glob: str,
    digest: str,
    invalidate: list[Path] | None = None,
    invalidate_globs: list[str] | None = None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    expected_line = f"# build profile {digest}\n"
    if path.is_file() and path.read_text(encoding="ascii") == expected_line:
        # GNU Make executes recipes that remake included makefiles even under -n,
        # and the manager uses -Bnwk to refresh compile_commands.json. A forced
        # same-profile recipe must therefore be a true no-op, not an invalidation.
        for stale in path.parent.glob(stale_glob):
            if stale != path:
                stale.unlink(missing_ok=True)
        return
    for target in invalidate or []:
        target.unlink(missing_ok=True)
    for pattern in invalidate_globs or []:
        pattern_path = Path(pattern)
        for target in pattern_path.parent.glob(pattern_path.name):
            target.unlink(missing_ok=True)
    for stale in path.parent.glob(stale_glob):
        if stale != path:
            stale.unlink(missing_ok=True)
    # A comment-only file is safe to use as a GNU Make included makefile. When a
    # profile changes, Make creates it, restarts parsing, and sees invalidated
    # objects as absent before deciding which targets are current.
    path.write_text(expected_line, encoding="ascii")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    for action in ("hash", "record"):
        command = subparsers.add_parser(action)
        command.add_argument("--compiler", required=True)
        command.add_argument("--entry", action="append", default=[])
        if action == "record":
            command.add_argument("--output", type=Path, required=True)
            command.add_argument("--section", required=True)
            command.add_argument("--stamp", type=Path)
            command.add_argument("--stale-glob")
            command.add_argument("--invalidate", type=Path, action="append", default=[])
            command.add_argument("--invalidate-glob", action="append", default=[])
    stamp = subparsers.add_parser("stamp")
    stamp.add_argument("--output", type=Path, required=True)
    stamp.add_argument("--stale-glob", required=True)
    stamp.add_argument("--value", required=True)
    # Same contract as `record --invalidate`: a stamp whose flavour changed must be able
    # to DELETE what that flavour produced. Deletion, not a newer mtime, is what makes a
    # dependent target unambiguously out of date -- see the note on the -include of the
    # profile stamps in the Makefile.
    stamp.add_argument("--invalidate", type=Path, action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.action == "stamp":
        activate_stamp(args.output, args.stale_glob, args.value, invalidate=args.invalidate)
        return 0
    payload = profile_payload(args.compiler, args.entry)
    digest = profile_hash(payload)
    if args.action == "hash":
        print(digest)
    else:
        record_profile(args.output, args.section, payload)
        if bool(args.stamp) != bool(args.stale_glob):
            raise ValueError("--stamp and --stale-glob must be supplied together")
        if args.stamp:
            activate_stamp(
                args.stamp,
                args.stale_glob,
                digest,
                invalidate=args.invalidate,
                invalidate_globs=args.invalidate_glob,
            )
        print(f"{args.section} profile: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
