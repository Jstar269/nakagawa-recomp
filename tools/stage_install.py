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

Usage::

    python tools/stage_install.py --dest "<install folder>" --desktop-shortcut
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

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
                target, include_ui_fonts=True, refresh=True, repo_root=repo))
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


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--dest", required=True, type=Path,
                        help="install folder outside the source tree")
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
