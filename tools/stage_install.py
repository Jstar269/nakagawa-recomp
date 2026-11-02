#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Stage a runnable player install outside the build tree, with a desktop shortcut.

The install uses the release layout the player already searches
(``docs/PREVIEW_RELEASE.md``)::

    <dest>/bin/nakagawa_player.exe   the built player, its runtime DLL closure,
                                     licence notices and UI fonts (on Linux the
                                     player is ``bin/nakagawa_player``, with no
                                     DLL closure: the system libraries apply)
    <dest>/source/                   the committed source tree (``git archive``),
                                     which carries tools/nk_cli.py, the Makefile
                                     and assets/prereq_manifest.json, so BUILD
                                     PACKAGE works from the install
    <dest>/nakagawa.ico              the app icon, assets/brand/nakagawa.ico
                                     (the one the executables carry; never a
                                     console icon)

Only a folder this tool created (it holds ``.nakagawa-install.json``) is ever
replaced, so pointing ``--dest`` at an unrelated folder fails closed. The
shortcut is a plain Windows ``.lnk`` written through ``WScript.Shell``; it
starts the player with ``--ui=next`` by default, the way Explorer would start it.
On Linux, ``--desktop-shortcut`` writes a per-user ``.desktop`` entry and a 256 px
icon under ``$XDG_DATA_HOME`` (``~/.local/share`` when unset). The marker records
the paths written outside ``--dest`` in ``integration``.

The Windows install's ``bin/`` also carries ``runtime-manifest.json``
(``tools/stage_runtime_dlls.py``): the size, SHA-256 and imports of the player
and every DLL beside it, the identity contract the portable ZIP shares.

``--portable-zip`` writes the release's portable ZIP from an already staged
folder (``docs/PREVIEW_RELEASE.md``) so the same commit and inputs give the same
bytes (#324): ``SHA256SUMS.txt`` is rewritten over every file in ordinal path
order, the runtime manifest in ``bin/`` is verified, and the archive stores its
entries sorted by path, all dated ``SOURCE_DATE_EPOCH`` (the source commit's
time unless set), with fixed permissions, no extra fields and no comment.
Links, absolute or parent-relative names and Git metadata (a `.git` entry,
which the source export rewrites on every run) are refused.

Usage::

    python tools/stage_install.py --dest "<install folder>" --desktop-shortcut
    python tools/stage_install.py --portable-zip <out.zip> --zip-stage <staged folder>
    python tools/stage_install.py --check-assets <release folder, or file names>

``--check-assets`` checks the names of a release's files against the one-archive-
per-platform contract in ``docs/PREVIEW_RELEASE.md`` (#1876).
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import re
import subprocess
import sys
import tarfile
import time
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import make_app_icon  # noqa: E402
import stage_runtime_dlls  # noqa: E402

IS_WINDOWS = os.name == "nt"
MARKER = ".nakagawa-install.json"
SCHEMA_VERSION = 2
SHORTCUT_NAME = "Nakagawa Recomp.lnk"
DESKTOP_ENTRY_NAME = "nakagawa-recomp.desktop"
DESKTOP_ICON_NAME = "nakagawa-recomp.png"
DEFAULT_ARGUMENTS = "--ui=next"


class InstallError(RuntimeError):
    """A named, fail-closed install boundary."""


def default_player_path(windows: bool = IS_WINDOWS) -> Path:
    """The player build's output on the given host, under ``build/``."""
    return ROOT / "build" / ("nakagawa_player.exe" if windows else "nakagawa_player")


def _build_hint() -> str:
    return "`mingw32-make player`" if IS_WINDOWS else "`make player`"


def _platform_name() -> str:
    return "windows" if IS_WINDOWS else sys.platform


def app_icon() -> bytes:
    """The committed app icon, the one the player executable carries."""
    return (ROOT / make_app_icon.OUTPUT).read_bytes()


def _git(repo: Path, *arguments: str, binary: bool = False):
    result = subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True,
                            check=False)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise InstallError(f"INSTALL_SOURCE_EXPORT_FAILED: git {arguments[0]} failed: {detail}")
    return result.stdout if binary else result.stdout.decode("utf-8", "replace").strip()


def export_source(repo: Path, destination: Path, revision: str = "HEAD") -> str:
    """Extract the committed tree at ``revision`` into ``destination``; return its SHA."""
    commit = _git(repo, "rev-parse", "--verify", f"{revision}^{{commit}}")
    archive = _git(repo, "archive", "--format=tar", commit, binary=True)
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        tar.extractall(destination, filter="data")
    if not (destination / "tools" / "nk_cli.py").is_file():
        raise InstallError("INSTALL_SOURCE_EXPORT_FAILED: the export has no tools/nk_cli.py")
    return commit


def _check_destination(dest: Path) -> None:
    if dest.exists() and not dest.is_dir():
        raise InstallError(f"INSTALL_DESTINATION_INVALID: {dest} is not a folder")
    if dest.is_dir() and any(dest.iterdir()) and not (dest / MARKER).is_file():
        raise InstallError(
            f"INSTALL_DESTINATION_NOT_OWNED: {dest} already holds files this tool did not "
            "create; choose an empty or new folder"
        )
    try:
        dest.resolve().relative_to(ROOT.resolve())
    except ValueError:
        return
    raise InstallError("INSTALL_DESTINATION_INVALID: the install must be outside the source tree")


def _replace_owned(staged: Path, final: Path) -> None:
    """Swap a freshly staged folder into place; the old copy is removed only after."""
    old = final.with_name(final.name + ".old")
    if old.exists():
        shutil.rmtree(old)
    if final.exists():
        try:
            final.rename(old)
        except OSError as exc:
            raise InstallError(
                f"INSTALL_IN_USE: {final} could not be replaced ({exc}); close the player "
                "and run the install again"
            ) from exc
    staged.rename(final)
    if old.exists():
        shutil.rmtree(old)


def stage_install(dest: Path, player: Path, *, repo: Path = ROOT,
                  revision: str = "HEAD", stage_dlls=None) -> dict:
    """Stage bin/, source/ and the icon under ``dest``; return the install record."""
    dest = dest.resolve()
    player = player.resolve()
    if not player.is_file():
        raise InstallError(
            f"INSTALL_PLAYER_MISSING: {player} was not found; build it with {_build_hint()}"
        )
    _check_destination(dest)
    dest.mkdir(parents=True, exist_ok=True)
    stage = dest / f".staging-{os.getpid()}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    try:
        bin_dir = stage / "bin"
        bin_dir.mkdir()
        shutil.copy2(player, bin_dir / player.name)
        # The PE DLL closure exists only for Windows; an injected stager still runs (tests).
        if IS_WINDOWS or stage_dlls is not None:
            stager = stage_dlls or (lambda target: stage_runtime_dlls.stage_runtime_dlls(
                target, include_ui_fonts=True, refresh=True, repo_root=repo,
                runtime_manifest=True))
            try:
                staged = stager(bin_dir)
            except stage_runtime_dlls.StageError as exc:
                raise InstallError(f"INSTALL_RUNTIME_STAGE_FAILED: {exc}") from exc
            runtime_stage = "STAGED"
        else:
            staged = []
            runtime_stage = "SKIPPED_NOT_PE"
        commit = export_source(repo, stage / "source", revision)
        (stage / "nakagawa.ico").write_bytes(app_icon())
        _replace_owned(bin_dir, dest / "bin")
        _replace_owned(stage / "source", dest / "source")
        os.replace(stage / "nakagawa.ico", dest / "nakagawa.ico")
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    record = {
        "schema_version": SCHEMA_VERSION,
        "source_commit": commit,
        "platform": _platform_name(),
        "player": f"bin/{player.name}",
        "runtime_stage": runtime_stage,
        "runtime_files": sorted(staged),
        "integration": [],
        "staged_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
    }
    _write_marker(dest, record)
    return record


def _write_marker(dest: Path, record: dict) -> None:
    (dest / MARKER).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def record_integration(dest: Path, paths) -> None:
    """Record the paths a shortcut wrote outside ``dest`` in the install marker."""
    marker = dest.resolve() / MARKER
    record = json.loads(marker.read_text(encoding="utf-8"))
    record["integration"] = [str(path) for path in paths]
    _write_marker(dest.resolve(), record)


def xdg_data_home() -> Path:
    """``$XDG_DATA_HOME``, or ``~/.local/share`` when it is unset or not an absolute path."""
    value = os.environ.get("XDG_DATA_HOME", "")
    if value and Path(value).is_absolute():
        return Path(value)
    return Path.home() / ".local" / "share"


def _exec_quote(value: str) -> str:
    """Quote one ``Exec=`` word as the Desktop Entry spec requires; ``%`` becomes ``%%``."""
    escaped = (value.replace("\\", "\\\\").replace('"', '\\"')
               .replace("`", "\\`").replace("$", "\\$"))
    return '"' + escaped.replace("%", "%%") + '"'


def write_desktop_entry(dest: Path, arguments: str = DEFAULT_ARGUMENTS) -> list[Path]:
    """Write the per-user application entry and its 256 px icon; return both paths."""
    dest = dest.resolve()
    data = xdg_data_home()
    entry = data / "applications" / DESKTOP_ENTRY_NAME
    icon = data / "icons" / "hicolor" / "256x256" / "apps" / DESKTOP_ICON_NAME
    player = dest / "bin" / "nakagawa_player"
    lines = [
        "[Desktop Entry]",
        "Type=Application",
        "Name=Nakagawa Recomp",
        "Comment=Nakagawa Recomp player",
        f"Exec={_exec_quote(str(player))} {arguments}".rstrip(),
        f"TryExec={player}",
        f"Path={dest / 'bin'}",
        f"Icon={icon}",
        "Terminal=false",
        "Categories=Game;",
        "",
    ]
    icon.parent.mkdir(parents=True, exist_ok=True)
    icon.write_bytes(make_app_icon.png_image(app_icon(), 256))
    entry.parent.mkdir(parents=True, exist_ok=True)
    entry.write_bytes("\n".join(lines).encode("utf-8"))
    return [entry, icon]


def desktop_folder() -> Path:
    """The signed-in user's Desktop folder, wherever Windows has redirected it."""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command",
         "[Environment]::GetFolderPath('Desktop')"],
        capture_output=True, text=True, check=False)
    folder = result.stdout.strip()
    if result.returncode != 0 or not folder:
        raise InstallError("INSTALL_DESKTOP_UNKNOWN: Windows did not report a Desktop folder")
    return Path(folder)


_SHORTCUT_SCRIPT = (
    "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:NK_LNK); "
    "$s.TargetPath = $env:NK_TARGET; $s.Arguments = $env:NK_ARGS; "
    "$s.WorkingDirectory = $env:NK_WORKDIR; $s.IconLocation = $env:NK_ICON + ',0'; "
    "$s.Description = 'Nakagawa Recomp'; $s.Save()"
)


def write_shortcut(shortcut: Path, dest: Path, arguments: str = DEFAULT_ARGUMENTS) -> None:
    """Write a .lnk that starts bin/nakagawa_player.exe the way Explorer does."""
    if os.name != "nt":
        raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: shortcuts are written on Windows only")
    dest = dest.resolve()
    environment = os.environ.copy()
    environment.update({
        "NK_LNK": str(shortcut),
        "NK_TARGET": str(dest / "bin" / "nakagawa_player.exe"),
        "NK_ARGS": arguments,
        "NK_WORKDIR": str(dest / "bin"),
        "NK_ICON": str(dest / "nakagawa.ico"),
    })
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", _SHORTCUT_SCRIPT],
        capture_output=True, text=True, env=environment, check=False)
    if result.returncode != 0 or not shortcut.is_file():
        raise InstallError(
            f"INSTALL_SHORTCUT_FAILED: {shortcut} was not written: {result.stderr.strip()}")


# ---- Reproducible portable ZIP (#324) --------------------------------------

SUMS_NAME = "SHA256SUMS.txt"
RUNTIME_MANIFEST_PATH = f"bin/{stage_runtime_dlls.RUNTIME_MANIFEST_NAME}"
#: The earliest and latest times a ZIP (MS-DOS) timestamp can hold, in UTC.
ZIP_EPOCH_MIN = 315532800        # 1980-01-01T00:00:00Z
ZIP_EPOCH_MAX = 4354819198       # 2107-12-31T23:59:58Z
ZIP_FILE_MODE = 0o100644
ZIP_DIR_MODE = 0o040755
ZIP_COMPRESS_LEVEL = 9
_MSDOS_DIRECTORY = 0x10


def resolve_source_date_epoch(explicit: str | None = None, *, repo: Path = ROOT,
                              revision: str = "HEAD", env=None) -> int:
    """The archive time: ``explicit``, else ``$SOURCE_DATE_EPOCH``, else the commit's time.

    The value is a decimal Unix time (reproducible-builds.org); anything else is
    refused rather than replaced by the clock.
    """
    environ = os.environ if env is None else env
    value = explicit if explicit is not None else environ.get("SOURCE_DATE_EPOCH", "")
    origin = "--source-date-epoch" if explicit is not None else "SOURCE_DATE_EPOCH"
    if not value:
        value = _git(repo, "log", "-1", "--format=%ct", f"{revision}^{{commit}}")
        origin = f"the commit time of {revision}"
    value = value.strip()
    if not value.isdigit() or not value.isascii():
        raise InstallError(f"PORTABLE_EPOCH_INVALID: {origin} is not a decimal Unix time: {value!r}")
    return int(value)


def _zip_date_time(epoch: int) -> tuple[int, int, int, int, int, int]:
    return time.gmtime(min(max(epoch, ZIP_EPOCH_MIN), ZIP_EPOCH_MAX))[:6]


def _check_entry_name(relative: str) -> str:
    path = PurePosixPath(relative)
    if (not relative or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts)
            or "\\" in relative or ":" in relative or "\0" in relative):
        raise InstallError(f"PORTABLE_STAGE_NAME_INVALID: {relative!r} cannot be a ZIP entry name")
    return relative


def _stage_tree(stage: Path) -> tuple[list[tuple[str, Path]], list[str]]:
    """Regular files and empty folders under ``stage``, by relative POSIX name.

    A symbolic link or junction anywhere in the stage is refused: a ZIP made on
    Windows cannot carry one, and following it would package bytes from outside.
    """
    files: list[tuple[str, Path]] = []
    empty: list[str] = []
    pending = [(stage, "")]
    while pending:
        folder, prefix = pending.pop()
        children = 0
        with os.scandir(folder) as entries:
            for entry in entries:
                children += 1
                relative = _check_entry_name(prefix + entry.name)
                if entry.name == ".git":
                    raise InstallError(
                        f"PORTABLE_STAGE_GIT_METADATA: {relative} is Git metadata, which differs on every "
                        "export (commit time, index timestamps); delete it from the stage before archiving")
                path = Path(entry.path)
                if entry.is_symlink() or path.is_junction():
                    raise InstallError(f"PORTABLE_STAGE_LINK: {relative} is a link; stage real files")
                if entry.is_dir(follow_symlinks=False):
                    pending.append((path, relative + "/"))
                elif entry.is_file(follow_symlinks=False):
                    files.append((relative, path))
                else:
                    raise InstallError(f"PORTABLE_STAGE_SPECIAL_FILE: {relative} is not a regular file")
        if children == 0 and prefix:
            empty.append(prefix)
    order = lambda name: name.encode("utf-8")  # noqa: E731 - one ordinal byte order everywhere
    return sorted(files, key=lambda item: order(item[0])), sorted(empty, key=order)


def _sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_sha256sums(stage: Path) -> int:
    """Rewrite ``SHA256SUMS.txt`` over every other file in ordinal path order; return the count."""
    files, _ = _stage_tree(stage)
    lines = [f"{_sha256_path(path)}  {relative}\n" for relative, path in files if relative != SUMS_NAME]
    (stage / SUMS_NAME).write_bytes("".join(lines).encode("utf-8"))
    return len(lines)


def _zip_info(name: str, date_time, *, directory: bool, size: int = 0) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time)
    info.create_system = 3
    if directory:
        info.external_attr = (ZIP_DIR_MODE << 16) | _MSDOS_DIRECTORY
        info.compress_type = zipfile.ZIP_STORED
    else:
        info.external_attr = ZIP_FILE_MODE << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        info.compress_level = ZIP_COMPRESS_LEVEL
        # Known before the write, so the Zip64 decision depends on the size alone.
        info.file_size = size
    return info


def write_portable_zip(stage: Path, zip_path: Path, *, source_date_epoch: int,
                       verify_runtime=None) -> dict:
    """Write ``stage`` as a byte-reproducible ZIP at ``zip_path``; return its summary.

    ``SHA256SUMS.txt`` is rewritten first and ``bin/runtime-manifest.json`` is
    verified (``verify_runtime`` replaces that check; tests pass their own).
    Entries carry no root folder, the layout ``ZipFile.CreateFromDirectory``
    produced for v0.0.1: one per file and one per empty folder.
    """
    stage = Path(stage).resolve()
    zip_path = Path(zip_path).resolve()
    if not stage.is_dir():
        raise InstallError(f"PORTABLE_STAGE_MISSING: {stage} is not a folder")
    if zip_path.exists():
        raise InstallError(f"PORTABLE_ZIP_EXISTS: {zip_path} already exists; write a new file")
    if zip_path.is_relative_to(stage):
        raise InstallError("PORTABLE_ZIP_INSIDE_STAGE: write the ZIP outside the staged folder")
    bin_dir = stage / "bin"
    try:
        if verify_runtime is None:
            stage_runtime_dlls.verify_runtime_manifest(bin_dir)
        else:
            verify_runtime(bin_dir)
    except stage_runtime_dlls.StageError as exc:
        raise InstallError(f"PORTABLE_RUNTIME_UNVERIFIED: {exc}") from exc
    write_sha256sums(stage)
    files, empty = _stage_tree(stage)
    date_time = _zip_date_time(source_date_epoch)
    entries = [(name, path) for name, path in files] + [(name, None) for name in empty]
    entries.sort(key=lambda item: item[0].encode("utf-8"))
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    partial = zip_path.with_name(zip_path.name + ".part")
    total = 0
    try:
        with zipfile.ZipFile(partial, "w", allowZip64=True) as archive:
            for name, path in entries:
                if path is None:
                    archive.writestr(_zip_info(name, date_time, directory=True), b"")
                    continue
                size = path.stat().st_size
                with path.open("rb") as source, \
                        archive.open(_zip_info(name, date_time, directory=False, size=size), "w") as output:
                    shutil.copyfileobj(source, output, 1 << 20)
                total += size
        os.replace(partial, zip_path)
    finally:
        partial.unlink(missing_ok=True)
    return {
        "zip": str(zip_path),
        "zip_bytes": zip_path.stat().st_size,
        "sha256": _sha256_path(zip_path),
        "files": len(files),
        "empty_folders": len(empty),
        "unpacked_bytes": total,
        "source_date_epoch": source_date_epoch,
        "zlib": zlib.ZLIB_RUNTIME_VERSION,
    }


_VERSION = r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?"
_PLAYER_ZIP = re.compile(rf"nakagawa-recomp-({_VERSION})-(windows-x64)\.zip")
_SOURCE_ZIP = re.compile(rf"nakagawa-recomp-({_VERSION})-prerequisite-sources\.zip")
# What a catalogue launcher drops before it ranks archives: checksum, signature, source and
# debug files. Every released file that is not the player archive carries one of these words.
_AUXILIARY_TOKENS = ("sha256", "checksum", ".minisig", ".sig", ".asc", "source", "src", "debug", "symbols",
                     ".pdb", "sbom")
_ARCHIVE_SUFFIXES = (".zip", ".7z", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".tar.zst", ".exe", ".msi", ".dmg",
                     ".appimage", ".deb", ".rpm")


def is_auxiliary_asset(name: str) -> bool:
    """True when a name filter that skips checksum, signature, source and debug files drops ``name``."""
    lowered = name.lower()
    return any(token in lowered for token in _AUXILIARY_TOKENS)


def check_release_assets(names) -> list[str]:
    """Return the problems with a release's file names; empty when the contract holds (#1876).

    Exactly one player archive per platform, ``nakagawa-recomp-<version>-<os>-<arch>.zip``, with its
    ``.sha256`` (and optionally that file's ``.minisig``); the prerequisite source archive with its
    ``.sha256``; one version throughout; and nothing else a launcher's name filter would keep as a
    second candidate.
    """
    names = sorted(set(names))
    problems: list[str] = []
    players: dict[str, list[str]] = {}
    versions: set[str] = set()
    for name in names:
        player = _PLAYER_ZIP.fullmatch(name)
        source = _SOURCE_ZIP.fullmatch(name)
        if player:
            players.setdefault(player[2], []).append(name)
        if player or source:
            versions.add((player or source)[1])
            continue
        checked = re.fullmatch(r"(nakagawa-recomp-.+\.zip)\.sha256(\.minisig)?", name)
        if checked and (_PLAYER_ZIP.fullmatch(checked[1]) or _SOURCE_ZIP.fullmatch(checked[1])):
            # A signature signs the checksum file, so it needs the checksum and the archive both.
            needed = [checked[1]] + ([f"{checked[1]}.sha256"] if checked[2] else [])
            problems += [f"ASSET_ORPHAN: {name} has no {other}" for other in needed if other not in names]
        elif name.lower().endswith(_ARCHIVE_SUFFIXES) and not is_auxiliary_asset(name):
            problems.append(f"ASSET_SECOND_ARCHIVE: {name} would compete with the player archive; name it "
                            "nakagawa-recomp-<version>-<os>-<arch>.zip, or as a source, checksum or debug file")
        else:
            problems.append(f"ASSET_NAME_UNKNOWN: {name} is not a name the release contract lists")
    if not players:
        problems.append("ASSET_PLAYER_MISSING: no nakagawa-recomp-<version>-windows-x64.zip")
    for platform, found in players.items():
        if len(found) > 1:
            problems.append(f"ASSET_PLATFORM_DUPLICATE: {platform} has {len(found)} archives: {', '.join(found)}")
        for archive in found:
            if f"{archive}.sha256" not in names:
                problems.append(f"ASSET_CHECKSUM_MISSING: {archive}.sha256")
    if len(versions) > 1:
        problems.append(f"ASSET_VERSION_MIXED: {', '.join(sorted(versions))}")
    kept = [name for name in names if not is_auxiliary_asset(name) and name.lower().endswith(_ARCHIVE_SUFFIXES)]
    if sorted(kept) != sorted(n for found in players.values() for n in found):
        problems.append("ASSET_FILTER_MISMATCH: a name filter that drops checksum, signature, source and debug "
                        f"files would leave {kept}, not the player archives")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--dest", type=Path,
                        help="install folder outside the source tree")
    parser.add_argument("--portable-zip", type=Path,
                        help="instead of installing, write this reproducible portable ZIP from "
                             "--zip-stage")
    parser.add_argument("--check-assets", nargs="+", metavar="PATH",
                        help="check a release folder's file names (or the file names given) against the "
                             "one-archive-per-platform contract")
    parser.add_argument("--zip-stage", type=Path,
                        help="the staged release folder --portable-zip archives")
    parser.add_argument("--source-date-epoch",
                        help="ZIP entry time as a Unix time (default: $SOURCE_DATE_EPOCH, else "
                             "the commit time of --revision)")
    default_player = default_player_path(IS_WINDOWS)
    parser.add_argument("--player", type=Path, default=default_player,
                        help=f"built player executable (default: build/{default_player.name})")
    parser.add_argument("--revision", default="HEAD",
                        help="committed revision exported as source/ (default: HEAD)")
    parser.add_argument("--desktop-shortcut", action="store_true",
                        help=f"also write '{SHORTCUT_NAME}' on the Desktop (Windows), or a "
                             f"'{DESKTOP_ENTRY_NAME}' entry (Linux)")
    parser.add_argument("--shortcut", type=Path,
                        help="write the Windows shortcut at this exact .lnk path instead")
    parser.add_argument("--arguments", default=DEFAULT_ARGUMENTS,
                        help=f"player arguments in the shortcut (default: {DEFAULT_ARGUMENTS})")
    args = parser.parse_args(argv)
    if args.check_assets is not None:
        paths = [Path(item) for item in args.check_assets]
        if len(paths) == 1 and paths[0].is_dir():
            names = [child.name for child in paths[0].iterdir()]
        else:
            names = [item.name for item in paths]
        problems = check_release_assets(names)
        for problem in problems:
            print(f"stage_install: FAIL: {problem}", file=sys.stderr)
        return 1 if problems else 0
    if args.portable_zip is not None or args.zip_stage is not None:
        if args.portable_zip is None or args.zip_stage is None or args.dest is not None:
            parser.error("--portable-zip and --zip-stage go together, without --dest")
        try:
            epoch = resolve_source_date_epoch(args.source_date_epoch, revision=args.revision)
            summary = write_portable_zip(args.zip_stage, args.portable_zip, source_date_epoch=epoch)
        except InstallError as exc:
            print(f"stage_install: FAIL: {exc}", file=sys.stderr)
            return 1
        print(json.dumps({"event": "portable-zip", **summary}, sort_keys=True))
        return 0
    if args.dest is None:
        parser.error("--dest is required unless --portable-zip is given")
    try:
        if args.shortcut is not None and not IS_WINDOWS:
            raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: --shortcut writes a Windows .lnk; "
                               "on Linux use --desktop-shortcut")
        record = stage_install(args.dest, args.player, revision=args.revision)
        print(f"stage_install: staged {record['player']} and source {record['source_commit'][:12]} "
              f"into {args.dest.resolve()}")
        if not IS_WINDOWS and args.desktop_shortcut:
            entry, icon = write_desktop_entry(args.dest, args.arguments)
            record_integration(args.dest, [entry, icon])
            print(f"stage_install: desktop entry {entry}")
        else:
            shortcut = args.shortcut or (desktop_folder() / SHORTCUT_NAME
                                         if args.desktop_shortcut else None)
            if shortcut is not None:
                write_shortcut(shortcut, args.dest, args.arguments)
                record_integration(args.dest, [shortcut])
                print(f"stage_install: shortcut {shortcut}")
    except InstallError as exc:
        print(f"stage_install: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
