# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run a soak or title-route command on a disposable copy of the Memory Stick and settings.

A long unattended run writes saves and settings. This wrapper hashes the real Memory Stick
tree (and any settings files), copies them into a throw-away folder, runs the given command
with ``SR_MEMSTICK`` (and optional settings variables) pointed at the copy, then hashes the
originals again. It prints ``ORIGINALS_UNCHANGED`` or fails naming the first path that
differs. It is a wrapper only: it never stops the guest from writing, and a write to the copy
is expected and kept for inspection.

    python tools/isolated_run.py --memstick MS --work-dir PRIVATE_DIR
        [--settings FILE --settings-env NAME] -- <command> [args]

``--work-dir`` must be an existing folder outside the originals; keep it under the private
per-title folder, because the copy can hold save data (AGENTS.md section 5). Exit status is
the command's own, unless an original changed (status 3) or the setup failed (status 2).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def snapshot(root: Path) -> dict[str, str]:
    """Map relative path -> sha256 (or ``<dir>``) for a file or tree; absent -> empty."""
    result: dict[str, str] = {}
    if root.is_file():
        result["."] = hashlib.sha256(root.read_bytes()).hexdigest()
        return result
    if not root.is_dir():
        return result
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.is_dir():
            result[rel] = "<dir>"
        else:
            result[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def first_difference(before: dict[str, str], after: dict[str, str]) -> str | None:
    for key in sorted(set(before) | set(after)):
        if before.get(key) != after.get(key):
            return key
    return None


def _overlaps(a: Path, b: Path) -> bool:
    return a == b or a in b.parents or b in a.parents


def run(args: argparse.Namespace) -> int:
    memstick = Path(args.memstick).resolve()
    work_dir = Path(args.work_dir).resolve()
    settings = [Path(p).resolve() for p in args.settings]
    if not memstick.is_dir():
        print(f"ISOLATED_RUN: memstick is not a directory: {memstick}", file=sys.stderr)
        return 2
    if not work_dir.is_dir():
        print(f"ISOLATED_RUN: work dir must exist: {work_dir}", file=sys.stderr)
        return 2
    for original in [memstick, *settings]:
        if _overlaps(work_dir, original):
            print(f"ISOLATED_RUN: work dir overlaps an original: {original}", file=sys.stderr)
            return 2
    if len(args.settings_env) != len(settings):
        print("ISOLATED_RUN: give one --settings-env per --settings", file=sys.stderr)
        return 2
    if not args.command:
        print("ISOLATED_RUN: no command after --", file=sys.stderr)
        return 2

    copy_root = Path(tempfile.mkdtemp(prefix="isolated-", dir=work_dir))
    before = {"memstick": snapshot(memstick)}
    ms_copy = copy_root / "memstick"
    shutil.copytree(memstick, ms_copy)
    env = dict(os.environ)
    env["SR_MEMSTICK"] = str(ms_copy)
    originals = {"memstick": memstick}
    for index, (path, name) in enumerate(zip(settings, args.settings_env, strict=True)):
        label = f"settings{index}"
        before[label] = snapshot(path)
        originals[label] = path
        target = copy_root / label / path.name
        target.parent.mkdir()
        if path.is_dir():
            shutil.copytree(path, target)
        elif path.exists():
            shutil.copy2(path, target)
        env[name] = str(target)
    print(f"ISOLATED_RUN: copy={copy_root}")
    status = subprocess.run(args.command, env=env, check=False).returncode

    for label, original in originals.items():
        diff = first_difference(before[label], snapshot(original))
        if diff is not None:
            print(f"ORIGINAL_CHANGED: {label} {original} first differing path: {diff}")
            return 3
    print(f"ORIGINALS_UNCHANGED: command_status={status}")
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--memstick", required=True, help="the real Memory Stick folder")
    parser.add_argument("--work-dir", required=True, help="existing private folder for the copy")
    parser.add_argument("--settings", action="append", default=[],
                        help="a settings file or folder to copy (repeatable)")
    parser.add_argument("--settings-env", action="append", default=[],
                        help="variable that receives the copy's path, one per --settings")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
