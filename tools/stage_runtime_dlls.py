#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Stage the host runtime DLL closure beside a built player or title package.

The player resolves its SDL3_ttf shared library at run time with
``SDL_LoadObject``. On Windows the loader only searches the executable's own
directory and ``PATH``, so a player started from Explorer or a plain ``cmd.exe``
(with no MSYS2 toolchain on ``PATH``) silently lost its readable UI font unless
the library and its whole dependency closure sat beside the executable.

This module is the one mechanical staging step both packaging routes share:

* ``tools/nk_cli.py build-package`` stages the closure into each built package;
* the Makefile's ``player`` target stages it beside ``build/nakagawa_player.exe``
  and copies the project's UI font assets beside the player.

The closure is determined by walking each PE binary's import table
(``objdump -p`` with a pure-Python PE fallback in ``tools/package_notices.py``),
recursively, stopping at Windows system DLLs. It is never hand-guessed. A
non-system import that cannot be resolved to a file is a named, fail-closed
error -- a half-staged closure would merely move the silent fallback.

Root DLLs follow the ``SDL3_DLL`` override pattern: an environment
override names the exact file, otherwise the search directories are consulted --
an explicit ``MSYS_PATH`` bin directory first, then the discovered toolchain
root (``MINGW_PREFIX``, a ``gcc`` on ``PATH``, or the installer's pinned
toolchain), then ``C:/msys64/ucrt64/bin`` as the last resort. Resolution
failures name both the per-file override and those environment knobs (#368).

Licence texts are staged beside the DLLs through
``tools/package_notices.py`` (``THIRD_PARTY_NOTICES/``), which fails closed when
any staged binary has no licence record in ``assets/third_party_components.json``.

The runtime manifest (#324, ``runtime-manifest.json`` beside the executables)
records the identity of every executable and DLL in the folder: its size, its
SHA-256 and the non-system DLLs it imports. Writing it fails closed when an
import resolves to no file in the folder, and ``--verify-runtime-manifest``
re-checks a staged or extracted folder against it, so a release cannot ship a
changed, missing, extra or unlisted runtime library unnoticed.
"""

from __future__ import annotations

import argparse
import hashlib
from collections import deque
import json
import os
import re
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

#: Environment variable naming the toolchain bin directory explicitly, the same
#: knob ``nk.ps1`` and ``tools/nk_doctor.py`` honour. ``MINGW_PREFIX`` (naming
#: the prefix rather than ``bin/``) is consulted through
#: ``package_notices.resolve_toolchain_root``.
TOOLCHAIN_BIN_ENV = "MSYS_PATH"


class StageError(RuntimeError):
    """A runtime DLL that must be staged is missing or has no licence record."""


class StagingCollisionError(StageError):
    """A runtime DLL's staging name is already a directory.

    Unlike a missing optional font closure, this damages the staging target, so
    the package build must refuse rather than fall back to the bitmap font.
    """


def default_search_dirs(env: Mapping[str, str] | None = None) -> list[Path]:
    """Directories searched for runtime DLLs: the toolchain bin directory.

    An explicit ``MSYS_PATH`` entry wins over discovery, so a non-default
    toolchain root stages without per-file overrides being set by hand.
    """
    environ = os.environ if env is None else env
    dirs: list[Path] = []
    explicit = environ.get(TOOLCHAIN_BIN_ENV, "").strip()
    if explicit:
        dirs.append(Path(explicit))
    toolchain_root = package_notices.resolve_toolchain_root()
    if toolchain_root is not None:
        discovered = toolchain_root / "bin"
        if discovered not in dirs:
            dirs.append(discovered)
    if DEFAULT_TOOLCHAIN_BIN not in dirs and DEFAULT_TOOLCHAIN_BIN.is_dir():
        dirs.append(DEFAULT_TOOLCHAIN_BIN)
    return dirs


def resolve_root_dll(
    name: str,
    override: str,
    search_dirs: Sequence[Path],
    *,
    override_env: str | None = None,
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
    override_hint = (
        f"set {override_env} to the exact file"
        if override_env
        else "use the per-file override"
    )
    raise StageError(
        f"{name} could not be resolved; {override_hint}, or set {TOOLCHAIN_BIN_ENV} "
        f"(a bin directory) or MINGW_PREFIX (a toolchain prefix) so its bin/ "
        f"directory is searched, or install it under {searched}"
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
    pending: deque[Path] = deque([root])
    unresolved: dict[str, str] = {}
    while pending:
        current = pending.popleft()
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


def stage_ui_font_assets(target_dir: Path, *, repo_root: Path = ROOT) -> list[str]:
    """Copy the pinned UI font bundle beside an opt-in player build."""
    source_root = repo_root / "assets" / "fonts" / "ui"
    destination_root = target_dir / "assets" / "fonts" / "ui"
    if not source_root.is_dir():
        raise StageError(f"UI_FONT_ASSETS_MISSING: expected {source_root}")
    if destination_root.exists() and not destination_root.is_dir():
        raise StagingCollisionError(
            f"UI font staging collision: {destination_root} is not a directory"
        )
    files = sorted(path for path in source_root.rglob("*") if path.is_file())
    if not files:
        raise StageError(f"UI_FONT_ASSETS_EMPTY: no files under {source_root}")
    staged: list[str] = []
    for source in files:
        relative = source.relative_to(source_root)
        destination = destination_root / relative
        if destination.exists() and not destination.is_file():
            raise StagingCollisionError(
                f"UI font staging collision: {destination} is not a file"
            )
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        except OSError as exc:
            raise StageError(
                f"UI_FONT_STAGE_FAILED: could not copy {relative.as_posix()}: {exc}"
            ) from exc
        staged.append((Path("assets") / "fonts" / "ui" / relative).as_posix())
    return staged


def _files_equal(first: Path, second: Path) -> bool:
    """Whether two files have identical bytes; a file that cannot be stat'ed counts as different."""
    try:
        if first.stat().st_size != second.stat().st_size:
            return False
    except OSError:
        return False
    first_digest = hashlib.sha256()
    second_digest = hashlib.sha256()
    with first.open("rb") as first_file, second.open("rb") as second_file:
        for chunk in iter(lambda: first_file.read(65536), b""):
            first_digest.update(chunk)
        for chunk in iter(lambda: second_file.read(65536), b""):
            second_digest.update(chunk)
    return first_digest.digest() == second_digest.digest()


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
    include_ui_fonts: bool = False,
    refresh: bool = False,
    runtime_manifest: bool = False,
) -> list[str]:
    """Stage the runtime DLL roots, their resolved closure, and (optionally)
    the generated licence notices into ``target_dir``.

    ``roots`` defaults to :data:`RUNTIME_ROOTS`; ``env`` defaults to the process
    environment and supplies each root's override. A root already present in
    ``target_dir`` is kept (a package may pre-bundle it) but still contributes
    its closure. ``include_ui_fonts`` copies the project font bundle for the
    standalone player target. Returns the sorted names of every file staged.

    With ``refresh=True``, a root or closure DLL already in ``target_dir`` is
    replaced by the resolved toolchain/override file when their bytes differ
    (compared by size then SHA-256, never by modification time, because MSYS2
    package files keep upstream timestamps). Identical files are left alone.
    With ``refresh=False`` (the default) pre-bundled package DLLs are kept,
    preserving the existing package-staging behaviour.

    With ``runtime_manifest=True``, ``runtime-manifest.json`` is written last
    (after the notices, which can add toolchain DLLs), describing every
    executable and DLL then in ``target_dir``.
    """
    target_dir.mkdir(parents=True, exist_ok=True)
    names = list(roots) if roots is not None else [name for name, _ in RUNTIME_ROOTS]
    overrides = dict(RUNTIME_ROOTS)
    environ = os.environ if env is None else env
    dirs = list(search_dirs) if search_dirs is not None else default_search_dirs(environ)

    staged: set[str] = set()
    for name in names:
        existing = target_dir / name
        if existing.is_file():
            if refresh:
                root_dll = resolve_root_dll(
                    name,
                    environ.get(overrides.get(name, ""), ""),
                    dirs,
                    override_env=overrides.get(name),
                )
            else:
                root_dll = existing
        elif existing.exists() and not existing.is_file():
            raise StagingCollisionError(
                f"{name} staging collision: {existing.name} is a directory; "
                f"remove or move the directory to stage the runtime DLL"
            )
        else:
            root_dll = resolve_root_dll(
                name,
                environ.get(overrides.get(name, ""), ""),
                dirs,
                override_env=overrides.get(name),
            )
        closure = resolve_dll_closure(
            root_dll, search_dirs=dirs, imports_of=imports_of, is_system=is_system
        )
        for source in closure:
            destination = target_dir / source.name
            if destination.exists() and not destination.is_file():
                raise StagingCollisionError(
                    f"{source.name} staging collision: {destination.name} is a directory; "
                    f"remove or move the directory to stage the runtime DLL"
                )
            if not destination.is_file() or (refresh and not _files_equal(source, destination)):
                shutil.copyfile(source, destination)
            staged.add(source.name)

    if include_ui_fonts:
        staged.update(stage_ui_font_assets(target_dir, repo_root=repo_root))

    if notices:
        package_notices.generate_package_notices(
            target_dir, repo_root=repo_root, toolchain_root=toolchain_root
        )
    if runtime_manifest:
        write_runtime_manifest(target_dir, imports_of=imports_of, is_system=is_system)
    return sorted(staged)


# ---- Runtime manifest (#324) ---------------------------------------------

#: The manifest's file name, beside the executables it describes.
RUNTIME_MANIFEST_NAME = "runtime-manifest.json"
RUNTIME_MANIFEST_FORMAT = "nakagawa-runtime-manifest"
RUNTIME_MANIFEST_SCHEMA_VERSION = 1
#: The binaries the manifest covers, by suffix, and the kind each is recorded as.
RUNTIME_BINARY_KINDS = {".exe": "executable", ".dll": "runtime-library"}
_SHA256_TEXT = re.compile(r"[0-9a-f]{64}")
_ENTRY_KEYS = {"path", "kind", "size", "sha256", "imports"}


class RuntimeManifestError(StageError):
    """The runtime manifest is missing, malformed, or does not match the folder."""


def host_resolved_libraries() -> frozenset[str]:
    """Lower-case names of the DLLs the host provides and no route ships (the Vulkan loader).

    They come from ``host_resolved_components`` in ``assets/third_party_components.json``.
    A binary may import one without the folder holding it; the manifest names each
    such import in ``host_libraries`` as a true runtime requirement of the host.
    """
    inventory = package_notices.load_component_inventory()
    return frozenset(str(record["dll"]).lower()
                     for record in inventory["native"].get("host_resolved_components", [])
                     if record.get("dll"))


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _runtime_binaries(target_dir: Path) -> list[Path]:
    """Every executable and DLL directly in ``target_dir``, sorted by lower-case name."""
    if not target_dir.is_dir():
        raise RuntimeManifestError(f"RUNTIME_MANIFEST_FOLDER_MISSING: {target_dir} is not a folder")
    found: list[Path] = []
    for entry in target_dir.iterdir():
        if entry.suffix.lower() not in RUNTIME_BINARY_KINDS:
            continue
        if entry.is_symlink() or not entry.is_file():
            raise RuntimeManifestError(
                f"RUNTIME_FILE_NOT_REGULAR: {entry.name} is a link or not a regular file"
            )
        found.append(entry)
    return sorted(found, key=lambda path: (path.name.lower(), path.name))


def build_runtime_manifest(
    target_dir: Path,
    *,
    imports_of: Callable[[Path], list[str]] = package_notices.get_binary_imports,
    is_system: Callable[[str], bool] = package_notices.is_system_dll,
    host_libraries: frozenset[str] | None = None,
) -> dict:
    """The runtime manifest of ``target_dir``: identity and imports of each binary in it.

    Raises :class:`RuntimeManifestError` when the folder has no executable or when
    any binary imports a non-system DLL that is not in the folder: a closure that
    would only load on a machine with a developer toolchain on ``PATH``.
    """
    binaries = _runtime_binaries(target_dir)
    if not any(path.suffix.lower() == ".exe" for path in binaries):
        raise RuntimeManifestError(f"RUNTIME_MANIFEST_NO_EXECUTABLE: {target_dir} holds no .exe")
    present = {path.name.lower() for path in binaries}
    host = host_resolved_libraries() if host_libraries is None else host_libraries
    files: list[dict] = []
    unresolved: list[str] = []
    required_from_host: set[str] = set()
    for path in binaries:
        imports = sorted({name for name in imports_of(path) if not is_system(name)},
                         key=lambda name: (name.lower(), name))
        for name in imports:
            if name.lower() in present:
                continue
            if name.lower() in host:
                required_from_host.add(name.lower())
            else:
                unresolved.append(f"{name} (imported by {path.name})")
        files.append({
            "path": path.name,
            "kind": RUNTIME_BINARY_KINDS[path.suffix.lower()],
            "size": path.stat().st_size,
            "sha256": _sha256_file(path),
            "imports": imports,
        })
    if unresolved:
        raise RuntimeManifestError(
            "RUNTIME_CLOSURE_INCOMPLETE: " + ", ".join(unresolved)
            + f" is not in {target_dir}; stage the runtime DLLs first"
        )
    return {
        "format": RUNTIME_MANIFEST_FORMAT,
        "schema_version": RUNTIME_MANIFEST_SCHEMA_VERSION,
        "files": files,
        "host_libraries": sorted(required_from_host),
    }


def _manifest_bytes(manifest: dict) -> bytes:
    return (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")


def write_runtime_manifest(target_dir: Path, **resolver) -> dict:
    """Write ``runtime-manifest.json`` into ``target_dir`` and return it."""
    manifest = build_runtime_manifest(target_dir, **resolver)
    (target_dir / RUNTIME_MANIFEST_NAME).write_bytes(_manifest_bytes(manifest))
    return manifest


def _load_runtime_manifest(target_dir: Path) -> dict:
    path = target_dir / RUNTIME_MANIFEST_NAME
    if not path.is_file():
        raise RuntimeManifestError(f"RUNTIME_MANIFEST_MISSING: {RUNTIME_MANIFEST_NAME} is not in {target_dir}")

    def invalid(detail: str) -> RuntimeManifestError:
        return RuntimeManifestError(f"RUNTIME_MANIFEST_INVALID: {detail}")

    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise invalid(f"{RUNTIME_MANIFEST_NAME} is not readable JSON ({exc})") from exc
    if (not isinstance(manifest, dict)
            or set(manifest) != {"format", "schema_version", "files", "host_libraries"}
            or manifest["format"] != RUNTIME_MANIFEST_FORMAT
            or manifest["schema_version"] != RUNTIME_MANIFEST_SCHEMA_VERSION
            or not isinstance(manifest["files"], list)
            or not isinstance(manifest["host_libraries"], list)
            or not all(isinstance(name, str) and name == name.lower() for name in manifest["host_libraries"])):
        raise invalid(f"not a {RUNTIME_MANIFEST_FORMAT} schema {RUNTIME_MANIFEST_SCHEMA_VERSION} record")
    seen: set[str] = set()
    for entry in manifest["files"]:
        if not isinstance(entry, dict) or set(entry) != _ENTRY_KEYS:
            raise invalid("a files entry does not have exactly path, kind, size, sha256 and imports")
        name = entry["path"]
        if (not isinstance(name, str) or not name or Path(name).name != name or "/" in name
                or "\\" in name or ":" in name
                or RUNTIME_BINARY_KINDS.get(Path(name).suffix.lower()) != entry["kind"]):
            raise invalid(f"entry {name!r} is not a plain .exe or .dll name of its recorded kind")
        if name.lower() in seen:
            raise invalid(f"{name} is listed twice")
        seen.add(name.lower())
        if (not isinstance(entry["size"], int) or isinstance(entry["size"], bool) or entry["size"] < 0
                or not isinstance(entry["sha256"], str) or not _SHA256_TEXT.fullmatch(entry["sha256"])
                or not isinstance(entry["imports"], list)
                or not all(isinstance(item, str) and item for item in entry["imports"])):
            raise invalid(f"entry {name} has a malformed size, sha256 or imports")
    return manifest


def verify_runtime_manifest(
    target_dir: Path,
    *,
    imports_of: Callable[[Path], list[str]] | None = package_notices.get_binary_imports,
    is_system: Callable[[str], bool] = package_notices.is_system_dll,
    require_licence_records: bool = True,
    host_libraries: frozenset[str] | None = None,
) -> dict:
    """Check ``target_dir`` against its runtime manifest; return the manifest.

    Every listed binary must exist with its recorded size and SHA-256, no
    unlisted executable or DLL may be present, every recorded import must be a
    listed file or a recorded host library (``host_libraries``, only names from
    :func:`host_resolved_libraries`), and (with ``imports_of``) the recorded imports must be the
    binary's real non-system imports. With ``require_licence_records`` every
    listed DLL must also have a component in ``THIRD_PARTY_NOTICES/index.json``.
    """
    manifest = _load_runtime_manifest(target_dir)
    listed = {entry["path"].lower(): entry for entry in manifest["files"]}
    host = host_resolved_libraries() if host_libraries is None else host_libraries
    stray_host = sorted(set(manifest["host_libraries"]) - host)
    if stray_host:
        raise RuntimeManifestError(
            f"RUNTIME_MANIFEST_INVALID: {', '.join(stray_host)} is not a recorded host-resolved library"
        )
    for path in _runtime_binaries(target_dir):
        if path.name.lower() not in listed:
            raise RuntimeManifestError(
                f"RUNTIME_FILE_UNLISTED: {path.name} is in {target_dir} but not in {RUNTIME_MANIFEST_NAME}"
            )
    for entry in manifest["files"]:
        path = target_dir / entry["path"]
        if not path.is_file():
            raise RuntimeManifestError(f"RUNTIME_FILE_MISSING: {entry['path']}")
        if path.stat().st_size != entry["size"] or _sha256_file(path) != entry["sha256"]:
            raise RuntimeManifestError(
                f"RUNTIME_FILE_CHANGED: {entry['path']} does not match its recorded size and SHA-256"
            )
        for name in entry["imports"]:
            if name.lower() not in listed and name.lower() not in manifest["host_libraries"]:
                raise RuntimeManifestError(
                    f"RUNTIME_CLOSURE_INCOMPLETE: {name} (imported by {entry['path']}) is not listed"
                )
        if imports_of is not None:
            actual = sorted({name for name in imports_of(path) if not is_system(name)},
                            key=lambda name: (name.lower(), name))
            if actual != entry["imports"]:
                raise RuntimeManifestError(
                    f"RUNTIME_IMPORTS_CHANGED: {entry['path']} imports {actual}, "
                    f"the manifest records {entry['imports']}"
                )
    if require_licence_records:
        index_path = target_dir / "THIRD_PARTY_NOTICES" / "index.json"
        try:
            index = json.loads(index_path.read_text(encoding="utf-8"))
            licensed = {str(component.get("binary", "")).lower() for component in index["components"]}
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError, AttributeError) as exc:
            raise RuntimeManifestError(
                f"RUNTIME_LICENCE_INDEX_MISSING: THIRD_PARTY_NOTICES/index.json is not readable ({exc})"
            ) from exc
        for entry in manifest["files"]:
            if entry["kind"] == "runtime-library" and entry["path"].lower() not in licensed:
                raise RuntimeManifestError(
                    f"RUNTIME_LICENCE_RECORD_MISSING: {entry['path']} has no component in "
                    "THIRD_PARTY_NOTICES/index.json"
                )
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Stage the SDL3/SDL3_ttf runtime DLL closure and their licence "
            "notices beside a built player or package."
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
    parser.add_argument(
        "--include-ui-fonts",
        action="store_true",
        help="also stage the pinned UI font bundle beside the standalone player",
    )
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="replace staged DLLs whose bytes differ from the resolved toolchain copy",
    )
    manifest_mode = parser.add_mutually_exclusive_group()
    manifest_mode.add_argument(
        "--runtime-manifest",
        action="store_true",
        help=f"after staging, write {RUNTIME_MANIFEST_NAME} (size, SHA-256 and imports of "
             "every .exe and .dll in the target)",
    )
    manifest_mode.add_argument(
        "--write-runtime-manifest-only",
        action="store_true",
        help=f"stage nothing; write {RUNTIME_MANIFEST_NAME} for the files already in the target",
    )
    manifest_mode.add_argument(
        "--verify-runtime-manifest",
        action="store_true",
        help=f"stage nothing; check the target against its {RUNTIME_MANIFEST_NAME}",
    )
    args = parser.parse_args(argv)

    if args.verify_runtime_manifest or args.write_runtime_manifest_only:
        try:
            if args.verify_runtime_manifest:
                manifest = verify_runtime_manifest(args.target)
                verb = "verified"
            else:
                manifest = write_runtime_manifest(args.target)
                verb = "wrote"
        except (StageError, package_notices.PackageRouteError) as exc:
            print(f"stage_runtime_dlls: FAIL: {exc}", file=sys.stderr)
            return 1
        print(f"stage_runtime_dlls: {verb} {RUNTIME_MANIFEST_NAME} for "
              f"{len(manifest['files'])} file(s) in {args.target}")
        return 0

    if os.name != "nt":
        # POSIX hosts resolve shared libraries through the system loader. The
        # opt-in player still needs its font assets beside the executable.
        if args.include_ui_fonts:
            try:
                fonts = stage_ui_font_assets(args.target, repo_root=ROOT)
            except StageError as exc:
                print(f"stage_runtime_dlls: FAIL: {exc}", file=sys.stderr)
                return 1
            print(f"stage_runtime_dlls: staged {len(fonts)} UI font asset(s) into {args.target}")
            return 0
        print(
            "stage_runtime_dlls: non-Windows host; shared libraries resolve "
            "through the system loader, nothing to stage"
        )
        return 0

    try:
        staged = stage_runtime_dlls(
            args.target,
            notices=not args.no_notices,
            include_ui_fonts=args.include_ui_fonts,
            refresh=args.refresh,
            runtime_manifest=args.runtime_manifest,
        )
    except (StageError, package_notices.PackageRouteError) as exc:
        print(f"stage_runtime_dlls: FAIL: {exc}", file=sys.stderr)
        return 1
    font_assets = [name for name in staged if name.startswith("assets/fonts/ui/")]
    dlls = [name for name in staged if name not in font_assets]
    print(
        f"stage_runtime_dlls: staged {len(dlls)} runtime DLL(s) into "
        f"{args.target}: {', '.join(dlls)}"
    )
    if font_assets:
        print(f"stage_runtime_dlls: staged {len(font_assets)} UI font asset(s) into {args.target}")
    if args.runtime_manifest:
        print(f"stage_runtime_dlls: wrote {RUNTIME_MANIFEST_NAME} into {args.target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
