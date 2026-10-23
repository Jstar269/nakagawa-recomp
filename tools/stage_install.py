#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Stage a runnable player install outside the build tree, with a desktop shortcut.

The install uses the release layout the player already searches
(``docs/PREVIEW_RELEASE.md``)::

    <dest>/bin/nakagawa_player.exe   the built player, its runtime DLL closure,
                                     licence notices and UI fonts
    <dest>/source/                   the committed source tree (``git archive``),
                                     which carries tools/nk_cli.py, the Makefile
                                     and assets/prereq_manifest.json, so BUILD
                                     PACKAGE works from the install
    <dest>/nakagawa.ico              the project mark, drawn from
                                     docs/images/brand (never a console icon)

Only a folder this tool created (it holds ``.nakagawa-install.json``) is ever
replaced, so pointing ``--dest`` at an unrelated folder fails closed. The
shortcut is a plain Windows ``.lnk`` written through ``WScript.Shell``; it
starts the player with ``--ui=next`` by default, the way Explorer would start it.

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
import struct
import subprocess
import sys
import tarfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import stage_runtime_dlls  # noqa: E402

MARKER = ".nakagawa-install.json"
SHORTCUT_NAME = "Nakagawa Recomp.lnk"
DEFAULT_ARGUMENTS = "--ui=next"
ICON_SIZES = (16, 24, 32, 48, 64, 256)

# The project mark (docs/images/brand/nakagawa-mark-dark.svg): viewBox 34..206,
# four lime blocks on the left, a light diagonal and right bar. Kept as geometry
# here so the icon is drawn from the committed mark rather than shipped as bytes.
_MARK_VIEW = (34.0, 34.0, 172.0)
_MARK_RECTS = (
    (34.0, 34.0, 46.0, 62.8, (0xD7, 0xF2, 0x4A)),
    (34.0, 104.0, 46.0, 29.2, (0xD7, 0xF2, 0x4A)),
    (34.0, 140.4, 46.0, 29.2, (0xD7, 0xF2, 0x4A)),
    (34.0, 176.8, 46.0, 29.2, (0xD7, 0xF2, 0x4A)),
    (160.0, 34.0, 46.0, 172.0, (0xF5, 0xF7, 0xFA)),
)
_MARK_POLYGON = (
    ((34.0, 34.0), (88.0, 34.0), (206.0, 195.1), (206.0, 206.0), (160.0, 206.0)),
    (0xF5, 0xF7, 0xFA),
)
_BACKGROUND = (0x10, 0x15, 0x1C)


class InstallError(RuntimeError):
    """A named, fail-closed install boundary."""


def _inside_polygon(x: float, y: float, points) -> bool:
    inside = False
    count = len(points)
    for index in range(count):
        x1, y1 = points[index]
        x2, y2 = points[(index + 1) % count]
        if (y1 > y) != (y2 > y):
            cross = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < cross:
                inside = not inside
    return inside


def _mark_color(u: float, v: float):
    """Colour of the mark at (u, v) in mark coordinates, or None outside it."""
    for x, y, w, h, color in _MARK_RECTS:
        if x <= u < x + w and y <= v < y + h:
            return color
    points, color = _MARK_POLYGON
    if _inside_polygon(u, v, points):
        return color
    return None


def render_icon_rgba(size: int, samples: int = 4) -> bytes:
    """RGBA pixels of the app icon: the mark on a rounded dark tile."""
    if size < 1:
        raise ValueError("icon size must be positive")
    origin, _, extent = _MARK_VIEW
    inset = size * 0.16
    scale = extent / (size - 2 * inset)
    radius = size * 0.18
    out = bytearray()
    for py in range(size):
        for px in range(size):
            totals = [0.0, 0.0, 0.0, 0.0]
            for sy in range(samples):
                for sx in range(samples):
                    fx = px + (sx + 0.5) / samples
                    fy = py + (sy + 0.5) / samples
                    # Rounded-square tile.
                    cx = min(max(fx, radius), size - radius)
                    cy = min(max(fy, radius), size - radius)
                    if (fx - cx) ** 2 + (fy - cy) ** 2 > radius * radius:
                        continue
                    color = _mark_color(origin + (fx - inset) * scale,
                                        origin + (fy - inset) * scale) or _BACKGROUND
                    totals[0] += color[0]
                    totals[1] += color[1]
                    totals[2] += color[2]
                    totals[3] += 255.0
            count = samples * samples
            alpha = totals[3] / count
            if alpha <= 0.0:
                out += b"\0\0\0\0"
                continue
            covered = totals[3] / 255.0
            out += bytes((
                round(totals[0] / covered), round(totals[1] / covered),
                round(totals[2] / covered), round(alpha),
            ))
    return bytes(out)


def _png(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    rows = b"".join(b"\0" + rgba[row * size * 4:(row + 1) * size * 4] for row in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def build_icon(sizes=ICON_SIZES) -> bytes:
    """A Windows .ico holding one PNG image per size (Vista and later)."""
    images = [(size, _png(size, render_icon_rgba(size))) for size in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    directory = b""
    payload = b""
    for size, data in images:
        dimension = 0 if size >= 256 else size
        directory += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                                 len(data), offset + len(payload))
        payload += data
    return header + directory + payload


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
            f"INSTALL_PLAYER_MISSING: {player} was not found; build it with `mingw32-make player`"
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
        stager = stage_dlls or (lambda target: stage_runtime_dlls.stage_runtime_dlls(
            target, include_ui_fonts=True, refresh=True, repo_root=repo))
        try:
            staged = stager(bin_dir)
        except stage_runtime_dlls.StageError as exc:
            raise InstallError(f"INSTALL_RUNTIME_STAGE_FAILED: {exc}") from exc
        commit = export_source(repo, stage / "source", revision)
        (stage / "nakagawa.ico").write_bytes(build_icon())
        _replace_owned(bin_dir, dest / "bin")
        _replace_owned(stage / "source", dest / "source")
        os.replace(stage / "nakagawa.ico", dest / "nakagawa.ico")
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    record = {
        "schema_version": 1,
        "source_commit": commit,
        "player": f"bin/{player.name}",
        "runtime_files": sorted(staged),
        "staged_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
    }
    (dest / MARKER).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


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
    parser.add_argument("--player", type=Path, default=ROOT / "build" / "nakagawa_player.exe",
                        help="built player executable (default: build/nakagawa_player.exe)")
    parser.add_argument("--revision", default="HEAD",
                        help="committed revision exported as source/ (default: HEAD)")
    parser.add_argument("--desktop-shortcut", action="store_true",
                        help=f"also write '{SHORTCUT_NAME}' on the Desktop")
    parser.add_argument("--shortcut", type=Path,
                        help="write the shortcut at this exact .lnk path instead")
    parser.add_argument("--arguments", default=DEFAULT_ARGUMENTS,
                        help=f"player arguments in the shortcut (default: {DEFAULT_ARGUMENTS})")
    args = parser.parse_args(argv)
    try:
        record = stage_install(args.dest, args.player, revision=args.revision)
        print(f"stage_install: staged {record['player']} and source {record['source_commit'][:12]} "
              f"into {args.dest.resolve()}")
        shortcut = args.shortcut or (desktop_folder() / SHORTCUT_NAME
                                     if args.desktop_shortcut else None)
        if shortcut is not None:
            write_shortcut(shortcut, args.dest, args.arguments)
            print(f"stage_install: shortcut {shortcut}")
    except InstallError as exc:
        print(f"stage_install: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
