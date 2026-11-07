# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build the per-user Windows installer from an unpacked release package (#2208, #324).

The input is the folder a release ZIP unpacks to, nothing else: the installer carries the
same bytes as the ZIP, so there is one set of files and one SBOM. The package is checked
first (every file against its ``SHA256SUMS.txt``, ``bin/runtime-manifest.json`` against
``bin/``), the Inno Setup compiler is checked against ``assets/upstream/innosetup.lock.json``
(version, and the SHA-256 of the compiler and of every file it embeds in the installer), and
only then is ``assets/installer/nakagawa.iss`` compiled. File times inside the installer come
from ``SOURCE_DATE_EPOCH``. The tool opens no network connection and installs nothing.

    python tools/build_installer.py --package-dir <unpacked ZIP> --version 0.0.2 --out-dir <folder>

Writes ``nakagawa-recomp-<version>-windows-x64-setup.exe`` and its ``.sha256`` into ``--out-dir``
(a file that already exists is an error), prints one JSON line, and exits 1 with an
``INSTALLER_*`` code on any problem.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Callable, Mapping, NoReturn

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

LOCK_PATH = ROOT / "assets" / "upstream" / "innosetup.lock.json"
SCRIPT_PATH = ROOT / "assets" / "installer" / "nakagawa.iss"
ICON_PATH = ROOT / "assets" / "brand" / "nakagawa.ico"
SUMS_NAME = "SHA256SUMS.txt"
#: The compiler's default per-user folder when Inno Setup is installed with ``/CURRENTUSER``.
INNO_ENV_DIR = "NK_INNO_SETUP_DIR"
_VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?")
_SHA256 = re.compile(r"[0-9a-f]{64}")
#: What a package must hold for the script's ``[Files]``, icon and licence page to work.
REQUIRED_PACKAGE_FILES = ("bin/nakagawa_player.exe", "bin/runtime-manifest.json", "LICENSE", SUMS_NAME)

Runner = Callable[..., "subprocess.CompletedProcess[str]"]


class InstallerError(RuntimeError):
    """A problem with a stable ``INSTALLER_*`` code at the start of the message."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def installer_name(version: str) -> str:
    """The release asset name of the installer (docs/PREVIEW_RELEASE.md, "Release asset names")."""
    return f"nakagawa-recomp-{version}-windows-x64-setup.exe"


def load_lock(path: Path = LOCK_PATH) -> dict[str, Any]:
    """The pinned Inno Setup record, validated; ``INSTALLER_LOCK_INVALID`` on any problem."""
    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise InstallerError(f"INSTALLER_LOCK_INVALID: {path} cannot be read as JSON ({exc})") from exc

    def bad(why: str) -> NoReturn:
        raise InstallerError(f"INSTALLER_LOCK_INVALID: {path.name} {why}")

    if not isinstance(lock, dict) or lock.get("schema_version") != 1:
        bad("is not a schema_version 1 object")
    version = lock.get("version")
    if not isinstance(version, str) or not _VERSION.fullmatch(version):
        bad("has no plain version")
    installer = lock.get("installer")
    if not isinstance(installer, dict):
        bad("has no installer record")
    url = installer.get("url")
    prefix = "https://github.com/jrsoftware/issrc/releases/download/"
    if not (isinstance(url, str) and url.startswith(prefix) and url.endswith(f"innosetup-{version}.exe")):
        bad(f"installer url is not the {version} release of github.com/jrsoftware/issrc")
    if installer.get("file") != f"innosetup-{version}.exe" or not _SHA256.fullmatch(str(installer.get("sha256"))):
        bad("installer file name or sha256 is malformed")
    files = lock.get("files")
    if not isinstance(files, dict) or "ISCC.exe" not in files or not all(
            isinstance(name, str) and name == Path(name).name and _SHA256.fullmatch(str(digest))
            for name, digest in files.items()):
        bad("files must map plain file names, ISCC.exe among them, to a sha256")
    licence = lock.get("license")
    if not isinstance(licence, dict) or licence.get("name") != "Inno Setup License" or not _SHA256.fullmatch(
            str(licence.get("text_sha256"))):
        bad("license must name 'Inno Setup License' and carry the sha256 of its text")
    result: dict[str, Any] = lock
    return result


def find_iscc(explicit: Path | None = None, environ: Mapping[str, str] | None = None) -> Path:
    """``ISCC.exe``: the ``--iscc`` path, else ``NK_INNO_SETUP_DIR``, else the per-user install folder."""
    env: Mapping[str, str] = os.environ if environ is None else environ
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    if env.get(INNO_ENV_DIR):
        candidates.append(Path(env[INNO_ENV_DIR]) / "ISCC.exe")
    if env.get("LOCALAPPDATA"):
        candidates.append(Path(env["LOCALAPPDATA"]) / "Programs" / "Inno Setup 6" / "ISCC.exe")
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise InstallerError(
        "INSTALLER_COMPILER_MISSING: ISCC.exe was not found; install the pinned Inno Setup per user "
        f"(assets/upstream/innosetup.lock.json) or set {INNO_ENV_DIR} or pass --iscc")


def verify_toolchain(iscc: Path, lock: dict[str, Any]) -> None:
    """The compiler and every file it embeds in an installer are the pinned bytes."""
    folder = iscc.parent
    for name, expected in sorted(lock["files"].items()):
        path = folder / name
        if not path.is_file():
            raise InstallerError(f"INSTALLER_COMPILER_UNPINNED: {path} is missing; reinstall Inno Setup {lock['version']}")
        actual = sha256_file(path)
        if actual != expected:
            raise InstallerError(
                f"INSTALLER_COMPILER_UNPINNED: {name} is {actual}, the lock pins {expected} "
                f"(Inno Setup {lock['version']}); install exactly that version")


def read_sums(package_dir: Path) -> dict[str, str]:
    """``{relative path: sha256}`` from the package's ``SHA256SUMS.txt``."""
    sums: dict[str, str] = {}
    text = (package_dir / SUMS_NAME).read_text(encoding="utf-8")
    for number, line in enumerate(text.splitlines(), 1):
        digest, separator, name = line.partition("  ")
        if not separator or not _SHA256.fullmatch(digest) or not name or name.startswith(("/", "../")) or "\\" in name:
            raise InstallerError(f"INSTALLER_PACKAGE_INVALID: {SUMS_NAME} line {number} is not '<sha256>  <path>'")
        sums[name] = digest
    return sums


def files_under(folder: Path) -> dict[str, Path]:
    """Every regular file under ``folder`` by POSIX relative name; a link or junction is refused."""
    found: dict[str, Path] = {}
    pending = [(folder, "")]
    while pending:
        current, prefix = pending.pop()
        with os.scandir(current) as entries:
            for entry in entries:
                path = Path(entry.path)
                relative = prefix + entry.name
                if entry.is_symlink() or path.is_junction():
                    raise InstallerError(f"INSTALLER_PACKAGE_INVALID: {relative} is a link; stage real files")
                if entry.is_dir(follow_symlinks=False):
                    pending.append((path, relative + "/"))
                elif entry.is_file(follow_symlinks=False):
                    found[relative] = path
    return found


def compare_with_sums(folder: Path, sums: dict[str, str]) -> list[str]:
    """Differences between ``folder`` and a ``SHA256SUMS.txt`` mapping, as ``CODE: path`` lines.

    ``SHA256SUMS.txt`` itself is not listed in its own file, so it is skipped on the folder side."""
    actual = files_under(folder)
    problems = [f"MISSING: {name}" for name in sorted(sums) if name not in actual]
    problems += [f"UNLISTED: {name}" for name in sorted(actual) if name not in sums and name != SUMS_NAME]
    for name in sorted(set(sums) & set(actual)):
        if sha256_file(actual[name]) != sums[name]:
            problems.append(f"CHANGED: {name}")
    return problems


def check_package(package_dir: Path, verify_runtime: Callable[[Path], object] | None = None) -> dict[str, str]:
    """Refuse a folder that is not an unpacked release package; return its ``SHA256SUMS.txt`` map."""
    if not package_dir.is_dir():
        raise InstallerError(f"INSTALLER_PACKAGE_MISSING: {package_dir} is not a folder")
    for required in REQUIRED_PACKAGE_FILES:
        if not (package_dir / required).is_file():
            raise InstallerError(f"INSTALLER_PACKAGE_INVALID: {required} is missing from {package_dir}")
    sums = read_sums(package_dir)
    problems = compare_with_sums(package_dir, sums)
    if problems:
        raise InstallerError(
            f"INSTALLER_PACKAGE_CHANGED: {package_dir} does not match its {SUMS_NAME}: "
            + "; ".join(problems[:8]) + (" ..." if len(problems) > 8 else ""))
    if verify_runtime is None:
        import stage_runtime_dlls
        verify_runtime = stage_runtime_dlls.verify_runtime_manifest
    try:
        verify_runtime(package_dir / "bin")
    except Exception as exc:  # StageError and RuntimeManifestError both mean the same thing here
        raise InstallerError(f"INSTALLER_RUNTIME_UNVERIFIED: {exc}") from exc
    return sums


def touch_for_epoch(epoch: int) -> tuple[str, str]:
    """``(TouchDate, TouchTime)`` for ``[Setup]``: the epoch in UTC, as ``YYYY-MM-DD`` and ``HH:MM:SS``."""
    moment = datetime.fromtimestamp(max(epoch, 315532800), tz=timezone.utc)  # not before 1980-01-01
    return moment.strftime("%Y-%m-%d"), moment.strftime("%H:%M:%S")


def resolve_epoch(explicit: int | None, environ: Mapping[str, str] | None = None) -> int:
    env: Mapping[str, str] = os.environ if environ is None else environ
    if explicit is not None:
        return explicit
    value = env.get("SOURCE_DATE_EPOCH", "")
    if not value.isdigit():
        raise InstallerError(
            "INSTALLER_EPOCH_MISSING: set SOURCE_DATE_EPOCH or pass --source-date-epoch (the release "
            "commit's time), so the installer's file times do not follow the clock")
    return int(value)


def compiler_command(iscc: Path, package_dir: Path, version: str, out_dir: Path, epoch: int,
                     extra_defines: dict[str, str] | None = None) -> list[str]:
    touch_date, touch_time = touch_for_epoch(epoch)
    extra = [f"/D{name}={value}" for name, value in sorted((extra_defines or {}).items())]
    return [
        str(iscc), "/Q",
        f"/O{out_dir}", f"/F{installer_name(version).removesuffix('.exe')}",
        f"/DPackageDir={package_dir}", f"/DAppVersion={version}", f"/DIconFile={ICON_PATH}",
        f"/DTouchDate={touch_date}", f"/DTouchTime={touch_time}", *extra,
        str(SCRIPT_PATH),
    ]


def build_installer(package_dir: Path, version: str, out_dir: Path, *, source_date_epoch: int | None = None,
                    iscc: Path | None = None, lock: dict[str, Any] | None = None, run: Runner = subprocess.run,
                    verify_runtime: Callable[[Path], object] | None = None,
                    environ: Mapping[str, str] | None = None,
                    extra_defines: dict[str, str] | None = None) -> dict[str, Any]:
    """Compile the installer and write its ``.sha256``; return a summary.

    ``extra_defines`` is for tests only (the command line offers no way to pass one), so a release
    build is always the script with its documented inputs."""
    if not _VERSION.fullmatch(version):
        raise InstallerError(f"INSTALLER_VERSION_INVALID: {version!r} is not MAJOR.MINOR.PATCH[-suffix]")
    package_dir = package_dir.resolve()
    out_dir = out_dir.resolve()
    if out_dir == package_dir or package_dir in out_dir.parents:
        raise InstallerError("INSTALLER_OUTPUT_INSIDE_PACKAGE: write the installer outside the package folder")
    epoch = resolve_epoch(source_date_epoch, environ)
    lock = load_lock() if lock is None else lock
    compiler = find_iscc(iscc, environ)
    verify_toolchain(compiler, lock)
    sums = check_package(package_dir, verify_runtime)
    target = out_dir / installer_name(version)
    checksum = out_dir / f"{target.name}.sha256"
    for existing in (target, checksum):
        if existing.exists():
            raise InstallerError(f"INSTALLER_OUTPUT_EXISTS: {existing} already exists; write a new file")
    out_dir.mkdir(parents=True, exist_ok=True)
    command = compiler_command(compiler, package_dir, version, out_dir, epoch, extra_defines)
    completed = run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0 or not target.is_file():
        tail = "\n".join((completed.stdout + completed.stderr).strip().splitlines()[-12:])
        target.unlink(missing_ok=True)
        raise InstallerError(f"INSTALLER_COMPILE_FAILED: ISCC exited {completed.returncode}\n{tail}")
    digest = sha256_file(target)
    checksum.write_text(f"{digest}  {target.name}\n", encoding="utf-8", newline="\n")
    return {"installer": str(target), "sha256": digest, "size": target.stat().st_size,
            "checksum_file": str(checksum), "inno_setup": lock["version"], "package_files": len(sums),
            "source_date_epoch": epoch}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--package-dir", type=Path, required=True,
                        help="the folder a release ZIP unpacks to (checked against its SHA256SUMS.txt)")
    parser.add_argument("--version", required=True, help="the release version, for example 0.0.2")
    parser.add_argument("--out-dir", type=Path, required=True, help="where the installer and its .sha256 go")
    parser.add_argument("--source-date-epoch", type=int, default=None,
                        help="file time inside the installer (default: SOURCE_DATE_EPOCH)")
    parser.add_argument("--iscc", type=Path, default=None, help="path to the pinned ISCC.exe")
    args = parser.parse_args(argv)
    try:
        summary = build_installer(args.package_dir, args.version, args.out_dir,
                                  source_date_epoch=args.source_date_epoch, iscc=args.iscc)
    except InstallerError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(json.dumps({"event": "installer", **summary}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
