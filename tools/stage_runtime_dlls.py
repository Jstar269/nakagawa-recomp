#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Stage the host runtime DLL closure beside a built player or title package.

The player resolves its SDL3_ttf shared library at run time with
``SDL_LoadObject``. On Windows the loader only searches the executable's own
directory and ``PATH``, so a player started from Explorer or a plain ``cmd.exe``
(with no MSYS2 toolchain on ``PATH``) silently lost its readable UI font unless
the library and its whole dependency closure sat beside the executable (#421).

This module is the one mechanical staging step both packaging routes share:

* ``tools/nk_cli.py build-package`` stages the closure into each built package;
* the Makefile's ``player`` target stages it beside ``build/nakagawa_player.exe``.

The closure is determined by walking each PE binary's import table
(``objdump -p`` with a pure-Python PE fallback in ``tools/package_notices.py``),
recursively, stopping at Windows system DLLs. It is never hand-guessed. A
non-system import that cannot be resolved to a file is a named, fail-closed
error -- a half-staged closure would merely move the silent fallback.

Root DLLs follow the ``SDL3_DLL`` override pattern from #296: an environment
override names the exact file, otherwise the toolchain bin directory
(``C:/msys64/ucrt64/bin`` by default) is searched.

Licence texts are staged beside the DLLs through
``tools/package_notices.py`` (``THIRD_PARTY_NOTICES/``), which fails closed when
any staged binary has no licence record in ``assets/third_party_components.json``.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path
from typing import Callable, Mapping, Sequence

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import package_notices  # noqa: E402

#: Files staged beside the player and into built packages, as
#: ``(file name, environment override)`` pairs. The overrides follow the
#: ``SDL3_DLL`` pattern: when set, they name the exact DLL file to stage.
RUNTIME_ROOTS: tuple[tuple[str, str], ...] = (
    ("SDL3.dll", "SDL3_DLL"),
    ("SDL3_ttf.dll", "SDL3_TTF_DLL"),
)

#: Default toolchain bin directory used when no override resolves.
DEFAULT_TOOLCHAIN_BIN = Path("C:/msys64/ucrt64/bin")


class StageError(RuntimeError):
    """A runtime DLL that must be staged is missing or has no licence record."""


def default_search_dirs() -> list[Path]:
    """Directories searched for runtime DLLs: the toolchain bin directory."""
    dirs: list[Path] = []
    toolchain_root = package_notices.resolve_toolchain_root()
    if toolchain_root is not None:
        dirs.append(toolchain_root / "bin")
    if DEFAULT_TOOLCHAIN_BIN not in dirs and DEFAULT_TOOLCHAIN_BIN.is_dir():
        dirs.append(DEFAULT_TOOLCHAIN_BIN)
    return dirs


def resolve_root_dll(
    name: str,
    override: str,
    search_dirs: Sequence[Path],
) -> Path:
    """Resolve one root DLL: explicit override first, then the search dirs."""
    if override:
        path = Path(override)
        if path.is_file():
            return path
        raise StageError(
            f"{name} override {override} does not name an existing file; "
            f"fix the override or unset it"
        )
    for directory in search_dirs:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    searched = ", ".join(str(d) for d in search_dirs) or "(no search directories)"
    raise StageError(
        f"{name} could not be resolved; set the override environment variable or "
        f"install it under {searched}"
    )


def _find_in_dirs(name: str, search_dirs: Sequence[Path]) -> Path | None:
    for directory in search_dirs:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    lowered = name.lower()
    for directory in search_dirs:
        if not directory.is_dir():
            continue
        try:
            for entry in directory.iterdir():
                if entry.name.lower() == lowered and entry.is_file():
                    return entry
        except OSError:
            continue
    return None


def resolve_dll_closure(
    root: Path,
    *,
    search_dirs: Sequence[Path],
    imports_of: Callable[[Path], list[str]] = package_notices.get_binary_imports,
    is_system: Callable[[str], bool] = package_notices.is_system_dll,
) -> list[Path]:
    """Mechanically resolve the DLL dependency closure of ``root``.

    Walks each binary's PE import table recursively and stops at Windows system
    DLLs (``package_notices.is_system_dll``). Returns ``[root, *dependencies]``
    with dependencies sorted by name. Raises :class:`StageError` when any
    non-system import has no file in ``search_dirs``: an incomplete closure
    cannot be shipped, and the error names every unresolved import and its
    importer.
    """
    resolved: dict[str, Path] = {root.name.lower(): root}
    pending: list[Path] = [root]
    unresolved: dict[str, str] = {}
    while pending:
        current = pending.pop(0)
        for imported in imports_of(current):
            key = imported.lower()
            if is_system(imported) or key in resolved or key in unresolved:
                continue
            found = _find_in_dirs(imported, search_dirs)
            if found is None:
                unresolved[key] = current.name
                continue
            resolved[key] = found
            pending.append(found)
    if unresolved:
        detail = ", ".join(
            f"{name} (imported by {importer})"
            for name, importer in sorted(unresolved.items())
        )
        searched = ", ".join(str(d) for d in search_dirs) or "(no search directories)"
        raise StageError(
            f"runtime DLL closure is incomplete: {detail}; searched {searched}"
        )
    ordered = [resolved.pop(root.name.lower())]
    return ordered + [resolved[key] for key in sorted(resolved)]


def stage_runtime_dlls(
    target_dir: Path,
    *,
    roots: Sequence[str] | None = None,
    search_dirs: Sequence[Path] | None = None,
    env: Mapping[str, str] | None = None,
    imports_of: Callable[[Path], list[str]] = package_notices.get_binary_imports,
    is_system: Callable[[str], bool] = package_notices.is_system_dll,
    notices: bool = True,
    repo_root: Path = ROOT,
    toolchain_root: Path | None = None,
) -> list[str]:
    """Stage the runtime DLL roots, their resolved closure, and (optionally)
    the generated licence notices into ``target_dir``.

    ``roots`` defaults to :data:`RUNTIME_ROOTS`; ``env`` defaults to the process
    environment and supplies each root's override. A root already present in
    ``target_dir`` is kept (a package may pre-bundle it) but still contributes
    its closure. Returns the sorted names of every file staged.
    """
    target_dir.mkdir(parents=True, exist_ok=True)
    names = list(roots) if roots is not None else [name for name, _ in RUNTIME_ROOTS]
    overrides = dict(RUNTIME_ROOTS)
    environ = os.environ if env is None else env
    dirs = list(search_dirs) if search_dirs is not None else default_search_dirs()

    staged: set[str] = set()
    for name in names:
        existing = target_dir / name
        if existing.is_file():
            root_dll = existing
        else:
            root_dll = resolve_root_dll(name, environ.get(overrides.get(name, ""), ""), dirs)
        closure = resolve_dll_closure(
            root_dll, search_dirs=dirs, imports_of=imports_of, is_system=is_system
        )
        for source in closure:
            destination = target_dir / source.name
            if not destination.is_file():
                shutil.copyfile(source, destination)
            staged.add(source.name)

    if notices:
        package_notices.generate_package_notices(
            target_dir, repo_root=repo_root, toolchain_root=toolchain_root
        )
    return sorted(staged)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Stage the SDL3/SDL3_ttf runtime DLL closure and their licence "
            "notices beside a built player or package (#421)."
        )
    )
    parser.add_argument(
        "--target",
        required=True,
        type=Path,
        help="directory receiving the DLLs (the directory of build/nakagawa_player.exe)",
    )
    parser.add_argument(
        "--no-notices",
        action="store_true",
        help="copy only the DLLs; the caller generates THIRD_PARTY_NOTICES itself",
    )
    args = parser.parse_args(argv)

    if os.name != "nt":
        # POSIX hosts resolve shared libraries through the system loader; there
        # is no staged-DLL layout to create. Nothing is silently skipped.
        print(
            "stage_runtime_dlls: non-Windows host; shared libraries resolve "
            "through the system loader, nothing to stage"
        )
        return 0

    try:
        staged = stage_runtime_dlls(args.target, notices=not args.no_notices)
    except (StageError, package_notices.PackageRouteError) as exc:
        print(f"stage_runtime_dlls: FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        f"stage_runtime_dlls: staged {len(staged)} runtime DLL(s) into "
        f"{args.target}: {', '.join(staged)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
