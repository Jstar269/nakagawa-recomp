# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Install the Windows installer into a temp folder, check it, and uninstall it (#2208, #324).

The check a built installer has to pass before it is a release asset:

1. a silent per-user install into a temp folder (no administrator);
2. every installed file equals the release package's ``SHA256SUMS.txt`` (the installer carries the
   ZIP's bytes), and nothing else but Inno's own ``unins000.*`` is there;
3. the Start-menu shortcut exists and carries the player's taskbar identity (``Nakagawa.Recomp``);
4. the uninstall entry is under HKCU only, never HKLM;
5. a silent uninstall removes the shortcut, the entry and the program, keeps a portable ``bin/data``,
   ``bin/portable.txt``, a file the person added, and the per-user data (a sentinel file this tool
   plants in ``%LOCALAPPDATA%\\Nakagawa\\logs`` only when that folder already exists, and removes after).

It refuses to run when a shortcut of the name it tests already exists, so a PC that has the real
shortcut keeps it: test an installer built with ``/DShortcutName=...`` (``build_installer.build_installer``'s
``extra_defines``) or on a PC without one. Windows only; nothing here uses the network.

    python tools/verify_installer.py --installer <setup.exe> --package-dir <unpacked ZIP> [--shortcut-name NAME]
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_installer  # noqa: E402
import stage_install  # noqa: E402

APP_ID = "A37F65B8-FB5B-41BB-AC7B-3700FE43CB02"
UNINSTALL_KEY = rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{{{APP_ID}}}_is1"
SENTINEL_NAME = "installer-verify-sentinel.txt"
#: Inno's own files, beside the program in the install folder.
INNO_OWN_PREFIX = "unins"


def compare_installed(package_dir: Path, installed_dir: Path) -> list[str]:
    """Differences between an install folder and the package it came from; empty when they match.

    ``unins000.*`` (Inno's uninstaller and its data) is the one allowed addition."""
    sums = build_installer.read_sums(package_dir)
    problems = []
    for problem in build_installer.compare_with_sums(installed_dir, sums):
        code, _, name = problem.partition(": ")
        if code == "UNLISTED" and "/" not in name and name.startswith(INNO_OWN_PREFIX):
            continue
        problems.append(problem)
    return problems


def egress_findings(script_text: str) -> list[str]:
    """Names in an Inno script that reach the network. A deterministic text test, like the repo's other egress tests."""
    lowered = script_text.lower()
    names = ("downloadtemporaryfile", "itdownload", "itd_", "createoleobject", "winhttp", "urlmon", "wininet",
             "shellexec('open', 'http", 'shellexec("open", "http')
    return [f"INSTALLER_NETWORK_CALL: {name}" for name in names if name in lowered]


def _hkcu_key_exists() -> bool:
    if sys.platform != "win32":
        return False
    import winreg
    try:
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY))
        return True
    except OSError:
        return False


def _hklm_key_exists() -> bool:
    if sys.platform != "win32":
        return False
    import winreg
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            winreg.CloseKey(winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, UNINSTALL_KEY, 0, winreg.KEY_READ | view))
            return True
        except OSError:
            continue
    return False


def _wait_for(condition: Callable[[], bool], seconds: float = 90.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.5)
    return condition()


def verify_installer(installer: Path, package_dir: Path, *, shortcut_name: str = "Nakagawa Recomp") -> dict:
    """Run the whole check; raise ``InstallerError`` on the first failure. Windows only."""
    if os.name != "nt":
        raise build_installer.InstallerError("INSTALLER_VERIFY_UNSUPPORTED: the installer runs on Windows only")
    installer = installer.resolve()
    package_dir = package_dir.resolve()
    start_menu = stage_install.start_menu_folder()
    shortcut = start_menu / f"{shortcut_name}.lnk"
    if shortcut.exists() or _hkcu_key_exists():
        raise build_installer.InstallerError(
            f"INSTALLER_VERIFY_REFUSED: {shortcut} or the installer's uninstall entry already exists on this "
            "PC; the check would replace and then remove it. Use an installer built with a different "
            "/DShortcutName on a PC with no installer-made install")
    sentinel = None
    logs = Path(os.environ.get("LOCALAPPDATA", "")) / "Nakagawa" / "logs"
    if logs.is_dir():
        sentinel = logs / SENTINEL_NAME
        sentinel.write_text("planted by tools/verify_installer.py; safe to delete\n", encoding="utf-8")

    def fail(code: str, why: str):
        raise build_installer.InstallerError(f"{code}: {why}")

    result: dict = {"installer": str(installer), "shortcut": str(shortcut)}
    with tempfile.TemporaryDirectory(prefix="nk-installer-verify-") as temp:
        dest = Path(temp) / "Nakagawa Recomp"
        try:
            done = subprocess.run(
                [str(installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/CURRENTUSER", "/NORESTART", "/SP-",
                 "/NOCANCEL", f"/DIR={dest}"], capture_output=True, text=True, timeout=600, check=False)
            if done.returncode != 0:
                fail("INSTALLER_INSTALL_FAILED", f"Setup exited {done.returncode}: {done.stdout}{done.stderr}")
            problems = compare_installed(package_dir, dest)
            if problems:
                fail("INSTALLER_INSTALL_DIFFERS", "; ".join(problems[:8]))
            if not (dest / "unins000.exe").is_file():
                fail("INSTALLER_NO_UNINSTALLER", f"{dest} has no unins000.exe")
            if not shortcut.is_file():
                fail("INSTALLER_NO_SHORTCUT", f"{shortcut} was not created")
            app_id = stage_install.read_shortcut_app_id(shortcut)
            if app_id != stage_install.APP_USER_MODEL_ID:
                fail("INSTALLER_SHORTCUT_IDENTITY", f"the shortcut carries {app_id!r}, not {stage_install.APP_USER_MODEL_ID!r}")
            if not _hkcu_key_exists():
                fail("INSTALLER_NO_UNINSTALL_ENTRY", f"HKCU\\{UNINSTALL_KEY} is missing")
            if _hklm_key_exists():
                fail("INSTALLER_WROTE_HKLM", "an uninstall entry exists under HKLM: the install was not per user")
            result["installed_files"] = len(build_installer.files_under(dest))
            # What the person owns beside the program survives the uninstall.
            (dest / "bin" / "data").mkdir()
            (dest / "bin" / "data" / "save.bin").write_bytes(b"save")
            (dest / "bin" / "portable.txt").write_text("", encoding="utf-8")
            (dest / "notes.txt").write_text("mine", encoding="utf-8")
            subprocess.run([str(dest / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                           capture_output=True, text=True, timeout=600, check=False)
            if not _wait_for(lambda: not _hkcu_key_exists() and not shortcut.exists()):
                fail("INSTALLER_UNINSTALL_INCOMPLETE", "the uninstall entry or the shortcut is still there after 90 s")
            _wait_for(lambda: not (dest / "unins000.exe").exists(), 30.0)
            left = sorted(str(path.relative_to(dest)).replace("\\", "/") for path in dest.rglob("*") if path.is_file())
            expected = ["bin/data/save.bin", "bin/portable.txt", "notes.txt"]
            if left != expected:
                fail("INSTALLER_UNINSTALL_LEFTOVER", f"left {left}, expected {expected}")
            if sentinel is not None and not sentinel.is_file():
                fail("INSTALLER_UNINSTALL_TOUCHED_USER_DATA", f"{sentinel} was deleted by a silent uninstall")
            result["left_after_uninstall"] = left
            result["status"] = "PASS"
        finally:
            if _hkcu_key_exists() and (dest / "unins000.exe").is_file():
                subprocess.run([str(dest / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                               capture_output=True, text=True, timeout=600, check=False)
                _wait_for(lambda: not _hkcu_key_exists(), 60.0)
            if sentinel is not None:
                sentinel.unlink(missing_ok=True)
            shutil.rmtree(dest, ignore_errors=True)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--shortcut-name", default="Nakagawa Recomp")
    args = parser.parse_args(argv)
    try:
        result = verify_installer(args.installer, args.package_dir, shortcut_name=args.shortcut_name)
    except build_installer.InstallerError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(json.dumps({"event": "installer-verify", **result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
