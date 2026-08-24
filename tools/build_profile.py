#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Hash and record the build identities the native build depends on.

Two kinds of identity live here, both answering "has an input to this
target changed?": compiler/flag profiles, and the operator-supplied guest
input pathnames described below.

Fail-closed transport and resolution of operator-supplied build input paths.

Guest-image pathnames (GAME_ELF, GAME_PSP_HEADER, GAME_EXTRA_ELFS) come from
operator/build configuration.  They are legal filenames, and on Windows a legal
filename may contain characters that a command interpreter treats as syntax:
`&` and `^` and `|` split or escape under cmd.exe, `%VAR%` expands under
cmd.exe, and `$VAR`, backtick, `'`, `"` expand under sh.  Interpolating such a
pathname into Make recipe command text therefore both truncates the path the
tool receives and hands the interpreter tokens taken from pathname data.

The build transports these values through the *process environment* instead, so
no pathname byte ever reaches a command interpreter as syntax.  This module is
the single resolution contract shared by every tool that consumes them, so the
five consumers do not drift into five slightly different argument contracts.

Two Make-level caveats this module cannot repair, and which callers must not
paper over (see tools/test_build_truth.py::GuestInputTransportTests):

* GNU Make expands `$` in a command-line variable value before this module
  ever sees it: `GAME_ELF=a$b.elf` reaches the environment as `a.elf`.
* mingw32 GNU Make transports its command line through the ANSI code page, so
  a pathname outside that code page arrives transliterated (CJK becomes `?`).

Both corrupt the value inside Make, upstream of the environment.  The
resolution below fails closed on the resulting nonexistent path rather than
silently opening a different file.

The `stamp` subcommand exists so Make can keep a real dependency edge on an
input whose *name* it cannot safely parse.  Make's prerequisite list is
whitespace-delimited and glob-expanded, so a pathname containing a space, or
one containing `[`/`]`/`*`/`?`, cannot be carried there faithfully.  The stamp
is a fixed, metacharacter-free path under the build directory whose *contents*
record the input's identity, so freshness is preserved for every pathname shape
instead of the dependency being dropped.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
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


# GAME_EXTRA_ELFS is a list.  Make joins list elements with spaces, which is
# also a legal filename character, so the environment form uses newline as the
# separator: a newline cannot occur in a Windows or POSIX path component.
LIST_SEPARATOR = "\n"


class BuildInputError(Exception):
    """An operator-supplied build input is missing, ambiguous, or unusable."""


def _flag(env_var: str, flag: str | None) -> str:
    """The command-line switch that requests ``env_var`` from the environment."""
    if flag:
        return flag
    return "--env-" + env_var.lower().replace("_", "-")


def resolve_path(
    env_var: str,
    *,
    cli_value: str | None = None,
    cli_label: str = "positional argument",
    use_env: bool = False,
    must_exist: bool = True,
    flag: str | None = None,
) -> str:
    """Return the single agreed value for ``env_var``, or raise.

    ``use_env`` is the caller's explicit request to read the environment (the
    ``--env-elf`` flag).  Supplying both an environment request and a
    command-line value is a configuration conflict: this raises rather than
    picking a winner, because the two sources disagreeing means the caller does
    not know which file it is building from.
    """
    env_value = os.environ.get(env_var)

    if use_env and cli_value is not None:
        raise BuildInputError(
            f"conflicting sources for {env_var}: both {_flag(env_var, flag)}"
            f" and a {cli_label} were supplied. Pass exactly one."
        )

    if use_env:
        if env_value is None:
            raise BuildInputError(
                f"{_flag(env_var, flag)} was requested but {env_var}"
                f" is not set in the environment."
            )
        value = env_value
    elif cli_value is not None:
        value = cli_value
    else:
        raise BuildInputError(
            f"no {env_var} supplied: pass it as a {cli_label} or request the"
            f" environment with {_flag(env_var, flag)}."
        )

    if not value or not value.strip():
        raise BuildInputError(f"{env_var} is empty or whitespace-only.")

    # A leading/trailing space is legal in neither of the shapes this build
    # supports and is far more likely to be a quoting accident than intent.
    value = value.strip()

    if must_exist:
        path = Path(value)
        if not path.exists():
            raise BuildInputError(f"{env_var} does not exist: {value}")
        if path.is_dir():
            raise BuildInputError(f"{env_var} is a directory, not a file: {value}")
        if not path.is_file():
            raise BuildInputError(f"{env_var} is not a regular file: {value}")

    return value


def resolve_list(env_var: str, *, use_env: bool = False, cli_values: list[str] | None = None,
                 flag: str | None = None) -> list[str]:
    """Return the list form of ``env_var`` (newline-separated), or raise on conflict."""
    if use_env and cli_values:
        raise BuildInputError(
            f"conflicting sources for {env_var}: both {_flag(env_var, flag)}"
            f" and explicit command-line entries were supplied. Pass exactly one."
        )
    if not use_env:
        return list(cli_values or [])
    raw = os.environ.get(env_var)
    if raw is None:
        raise BuildInputError(
            f"{_flag(env_var, flag)} was requested but {env_var}"
            f" is not set in the environment."
        )
    return [item for item in (line.strip() for line in raw.split(LIST_SEPARATOR)) if item]


def identity_of(path: str) -> str:
    """Return a stable identity line for one input file.

    Both mtime and content hash are recorded.  mtime keeps Make's own
    "touch rebuilds" semantics, which the raw-prerequisite form had and which
    the dependency edge must not lose; the hash additionally distinguishes two
    different files that happen to share a timestamp.
    """
    p = Path(path)
    stat = p.stat()
    digest = hashlib.sha256(p.read_bytes()).hexdigest()
    return f"{stat.st_size} {stat.st_mtime_ns} {digest}"


def stamp_inputs(args: argparse.Namespace) -> int:
    lines: list[str] = []
    for env_var in args.env:
        optional = env_var in args.optional_env
        try:
            if env_var in args.list_env:
                if optional and not os.environ.get(env_var, "").strip():
                    lines.append(f"{env_var} <unset>")
                    continue
                entries = resolve_list(env_var, use_env=True)
                for entry in entries:
                    # Extra-ELF entries are "path@base"; the identity covers the
                    # file, and the base belongs to the codegen profile hash.
                    file_part = entry.rsplit("@", 1)[0] if "@" in entry else entry
                    if not Path(file_part).is_file():
                        raise BuildInputError(f"{env_var} entry does not exist: {file_part}")
                    lines.append(f"{env_var} {entry} {identity_of(file_part)}")
                if not entries:
                    lines.append(f"{env_var} <empty>")
            else:
                if optional and not os.environ.get(env_var, "").strip():
                    lines.append(f"{env_var} <unset>")
                    continue
                value = resolve_path(env_var, use_env=True)
                lines.append(f"{env_var} {value} {identity_of(value)}")
        except (BuildInputError, OSError) as exc:
            sys.stderr.write(f"build_profile: {exc}\n")
            return 2

    payload = "\n".join(lines) + "\n"
    out = Path(args.out)
    # Rewrite only on change: an unchanged stamp must not bump its mtime, or
    # every dependent would rebuild on every invocation.
    try:
        if out.exists() and out.read_text(encoding="utf-8") == payload:
            return 0
    except OSError:
        pass
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(payload, encoding="utf-8", newline="\n")
    return 0


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

    # Guest-input identity. Kept separate from `stamp` because it derives its value
    # from the environment rather than being handed one.
    inputs = subparsers.add_parser("stamp-inputs")
    inputs.add_argument("--env", action="append", default=[], metavar="VAR",
                        help="environment variable naming a REQUIRED input path (repeatable)")
    inputs.add_argument("--optional-env", action="append", default=[], metavar="VAR",
                        help="like --env, but an unset or empty VAR is recorded instead of failing")
    inputs.add_argument("--list-env", action="append", default=[], metavar="VAR",
                        help="treat VAR as a newline-separated list of path@base entries")
    inputs.add_argument("--out", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.action == "stamp-inputs":
        for var in args.optional_env + args.list_env:
            if var not in args.env:
                args.env.append(var)
        return stamp_inputs(args)
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
