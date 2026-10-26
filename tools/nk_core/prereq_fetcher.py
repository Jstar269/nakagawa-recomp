# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Fetch and install pinned build prerequisites into per-user application data."""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile
import tempfile
from typing import Callable
from urllib.parse import urljoin, urlsplit
import zipfile


class PrerequisiteFetchError(RuntimeError):
    """A prerequisite could not be fetched or safely installed."""


ProgressCallback = Callable[[str, int, int], None]
CancelCheck = Callable[[], bool]
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CHUNK_SIZE = 64 * 1024


def default_data_root() -> Path:
    """Return the player's per-user data root; never place downloads by an ISO."""
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
        if not base:
            raise PrerequisiteFetchError(
                "DATA_DIR_UNAVAILABLE: set LOCALAPPDATA or APPDATA; USERPROFILE alone "
                "does not identify an application-data directory."
            )
        return Path(base) / "Nakagawa" / "data"
    if sys.platform == "darwin":
        try:
            return Path.home() / "Library" / "Application Support" / "NakagawaRecomp" / "data"
        except RuntimeError as exc:
            raise PrerequisiteFetchError(
                "DATA_DIR_UNAVAILABLE: set HOME to locate per-user data."
            ) from exc
    if os.name == "posix" and os.environ.get("XDG_DATA_HOME"):
        return Path(os.environ["XDG_DATA_HOME"]) / "nakagawa-recomp"
    try:
        return Path.home() / ".local" / "share" / "nakagawa-recomp"
    except RuntimeError as exc:
        raise PrerequisiteFetchError(
            "DATA_DIR_UNAVAILABLE: set XDG_DATA_HOME or HOME to locate per-user data."
        ) from exc


def _safe_relative_path(value: str) -> PurePosixPath:
    if not isinstance(value, str) or not value or "\\" in value:
        raise PrerequisiteFetchError("MANIFEST_INVALID: archive path is empty or malformed.")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise PrerequisiteFetchError("ARCHIVE_PATH_REJECTED: archive member escapes its install directory.")
    return path


def _validated_item(item: dict, *, allow_http: bool) -> tuple[str, int, str, set[str]]:
    try:
        item_id = item["id"]
        size = item["size_bytes"]
        raw_digest = item["sha256"]
        url = item["url"]
        hosts = {host.lower().rstrip(".") for host in item["allowed_hosts"]}
    except (KeyError, AttributeError, TypeError) as exc:
        raise PrerequisiteFetchError("MANIFEST_INVALID: prerequisite fields are missing.") from exc
    if not isinstance(item_id, str) or not item_id or any(ch not in "abcdefghijklmnopqrstuvwxyz0123456789-._+" for ch in item_id):
        raise PrerequisiteFetchError("MANIFEST_INVALID: prerequisite ID is unsafe.")
    if not isinstance(size, int) or size <= 0:
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} has no positive byte size.")
    if not isinstance(raw_digest, str):
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} has no SHA-256 digest.")
    digest = raw_digest.lower()
    if len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} has no SHA-256 digest.")
    parsed = urlsplit(url)
    if parsed.scheme not in ({"https", "http"} if allow_http else {"https"}) or not parsed.hostname:
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} must use an allowed HTTPS URL.")
    if parsed.hostname.lower().rstrip(".") not in hosts:
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} URL host is not in its allow-list.")
    if not hosts:
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: {item_id} has an empty host allow-list.")
    return url, size, digest, hosts


def _allow_listed_opener(hosts: set[str], *, allow_http: bool):
    # urllib.request pulls in http.client and the email parser, about 0.7 s of
    # interpreter start-up on every CLI invocation. Only a real download needs
    # it, so the import is deferred to here and never runs on the cached path.
    from urllib.request import HTTPRedirectHandler, build_opener

    class _AllowListedRedirects(HTTPRedirectHandler):
        def __init__(self, hosts: set[str], *, allow_http: bool) -> None:
            super().__init__()
            self.hosts = hosts
            self.allow_http = allow_http

        def redirect_request(self, req, fp, code, msg, headers, newurl):
            old = urlsplit(req.full_url)
            target = urlsplit(urljoin(req.full_url, newurl))
            host = (target.hostname or "").lower().rstrip(".")
            allowed_schemes = {"https", "http"} if self.allow_http else {"https"}
            if target.scheme not in allowed_schemes or host not in self.hosts:
                raise PrerequisiteFetchError(
                    f"REDIRECT_REJECTED: {req.full_url} redirected outside its HTTPS host allow-list."
                )
            if old.scheme == "https" and target.scheme != "https":
                raise PrerequisiteFetchError("REDIRECT_REJECTED: HTTPS downgrade redirects are not allowed.")
            return super().redirect_request(req, fp, code, msg, headers, target.geturl())

    return build_opener(_AllowListedRedirects(hosts, allow_http=allow_http))


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(CHUNK_SIZE):
            digest.update(block)
    return digest.hexdigest()


def download_verified_item(
    item: dict,
    destination: Path,
    *,
    progress: ProgressCallback | None = None,
    retries: int = 3,
    allow_http: bool = False,
) -> Path:
    """Download an item to a same-directory .part, verify it, then atomically keep it."""
    url, expected_size, expected_hash, hosts = _validated_item(item, allow_http=allow_http)
    if retries < 1:
        raise ValueError("retries must be positive")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    host = urlsplit(url).hostname or "unknown host"

    if destination.is_file():
        if destination.stat().st_size == expected_size and _sha256_file(destination) == expected_hash:
            if progress:
                progress(item["id"], expected_size, expected_size)
            return destination
        destination.unlink()

    from urllib.error import HTTPError, URLError
    from urllib.request import Request

    opener = _allow_listed_opener(hosts, allow_http=allow_http)
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        partial.unlink(missing_ok=True)
        digest = hashlib.sha256()
        received = 0
        request = Request(url, headers={"Accept-Encoding": "identity", "User-Agent": "NakagawaRecomp/PrerequisiteFetcher"})
        try:
            with opener.open(request, timeout=30) as response, partial.open("wb") as output:
                response_length = response.headers.get("Content-Length")
                if response_length is not None:
                    try:
                        declared_size = int(response_length)
                    except ValueError as exc:
                        raise PrerequisiteFetchError(
                            f"SIZE_MISMATCH: {item['id']} response has an invalid Content-Length."
                        ) from exc
                    if declared_size != expected_size:
                        raise PrerequisiteFetchError(
                            f"SIZE_MISMATCH: {item['id']} declared {declared_size} bytes, expected {expected_size}."
                        )
                while True:
                    block = response.read(CHUNK_SIZE)
                    if not block:
                        break
                    received += len(block)
                    if received > expected_size:
                        raise PrerequisiteFetchError(f"SIZE_MISMATCH: {item['id']} exceeded {expected_size} bytes.")
                    output.write(block)
                    digest.update(block)
                    if progress:
                        progress(item["id"], received, expected_size)
                output.flush()
                os.fsync(output.fileno())
            if received != expected_size:
                raise ConnectionError(f"interrupted after {received} of {expected_size} bytes")
            if digest.hexdigest() != expected_hash:
                raise PrerequisiteFetchError(f"HASH_MISMATCH: SHA-256 verification failed for {item['id']}.")
            os.replace(partial, destination)
            return destination
        except PrerequisiteFetchError:
            partial.unlink(missing_ok=True)
            raise
        except (HTTPError, URLError, TimeoutError, OSError, ConnectionError) as exc:
            partial.unlink(missing_ok=True)
            last_error = exc
            if attempt == retries:
                break
    detail = str(last_error) if last_error else "unknown network error"
    raise PrerequisiteFetchError(
        f"OFFLINE_OR_NETWORK_ERROR: could not download {item['id']} from {host} after {retries} safe attempts; "
        f"check the connection and retry. {detail}"
    ) from last_error


# A licence or copying file anywhere in a package: COPYING, LICENSE, LICENCE, NOTICE or
# COPYRIGHT, alone or with a dotted suffix such as COPYING.LESSER or LICENSE.txt. Some
# packages keep these only in share/doc (MSYS2 mpfr's COPYING.LESSER), which a release
# bundle strips (#1285).
_NOTICE_NAME = re.compile(r"(?:copying|licen[cs]e|notice|copyright)(?:\.[a-z0-9][a-z0-9._-]*)?", re.IGNORECASE)


def _is_notice_member(archive_member: str) -> bool:
    normalized = archive_member.replace("\\", "/")
    return ("/share/licenses/" in f"/{normalized.lower()}"
            or _NOTICE_NAME.fullmatch(PurePosixPath(normalized).name) is not None)


def _copy_notices(item_id: str, archive_member: str, source: Path, notices_root: Path) -> str | None:
    normalized = archive_member.replace("\\", "/")
    if not _is_notice_member(normalized):
        return None
    relative = _safe_relative_path(normalized)
    target = notices_root / item_id / Path(*relative.parts)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return target.as_posix()


def _copy_checked(source, destination, cancelled: CancelCheck | None) -> None:
    while block := source.read(CHUNK_SIZE):
        if cancelled and cancelled():
            raise PrerequisiteFetchError("INSTALL_CANCELLED: extraction cancelled by the user.")
        destination.write(block)


def _extract_zip(item: dict, archive: Path, target: Path, notices_root: Path,
                 cancelled: CancelCheck | None = None) -> list[str]:
    notices: list[str] = []
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if cancelled and cancelled():
                raise PrerequisiteFetchError("INSTALL_CANCELLED: extraction cancelled by the user.")
            if info.is_dir():
                continue
            relative = _safe_relative_path(info.filename)
            mode = info.external_attr >> 16
            if mode & 0o170000 == 0o120000:
                raise PrerequisiteFetchError("ARCHIVE_LINK_REJECTED: zip symlinks are not installed.")
            output = target.joinpath(*relative.parts)
            output.parent.mkdir(parents=True, exist_ok=True)
            with package.open(info) as source, output.open("wb") as destination:
                _copy_checked(source, destination, cancelled)
            notice = _copy_notices(item["id"], info.filename, output, notices_root)
            if notice:
                notices.append(notice)
    return notices


def _extract_tar(item: dict, archive: Path, target: Path, notices_root: Path,
                 cancelled: CancelCheck | None = None) -> list[str]:
    notices: list[str] = []
    try:
        package = tarfile.open(archive, mode="r:*")
    except (tarfile.TarError, OSError, ImportError) as exc:
        raise PrerequisiteFetchError(
            f"ARCHIVE_UNSUPPORTED: Python 3.14 or 3.15 with zstd support is required for {item['id']}."
        ) from exc
    with package:
        members = package.getmembers()
        safe_members: list[tuple[tarfile.TarInfo, PurePosixPath]] = []
        for member in members:
            relative = _safe_relative_path(member.name.rstrip("/"))
            if member.isdev() or member.isfifo():
                raise PrerequisiteFetchError("ARCHIVE_SPECIAL_FILE_REJECTED: package contains a device or FIFO.")
            if member.issym() or member.islnk():
                link_value = member.linkname
                link = PurePosixPath(link_value)
                if link.is_absolute() or "\\" in link_value:
                    raise PrerequisiteFetchError("ARCHIVE_LINK_REJECTED: package link is absolute or malformed.")
                combined = relative.parent.joinpath(link) if member.issym() else link
                if any(part == ".." for part in combined.parts):
                    raise PrerequisiteFetchError("ARCHIVE_LINK_REJECTED: package link escapes its install directory.")
            safe_members.append((member, relative))

        links: list[tuple[tarfile.TarInfo, PurePosixPath]] = []
        for member, relative in safe_members:
            if cancelled and cancelled():
                raise PrerequisiteFetchError("INSTALL_CANCELLED: extraction cancelled by the user.")
            destination = target.joinpath(*relative.parts)
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                source = package.extractfile(member)
                if source is None:
                    raise PrerequisiteFetchError("ARCHIVE_INVALID: a regular file has no payload.")
                with source, destination.open("wb") as output:
                    _copy_checked(source, output, cancelled)
                notice = _copy_notices(item["id"], member.name, destination, notices_root)
                if notice:
                    notices.append(notice)
            elif member.issym() or member.islnk():
                links.append((member, relative))

        for member, relative in links:
            if cancelled and cancelled():
                raise PrerequisiteFetchError("INSTALL_CANCELLED: extraction cancelled by the user.")
            destination = target.joinpath(*relative.parts)
            link = PurePosixPath(member.linkname)
            source_path = target.joinpath(*(relative.parent.joinpath(link).parts if member.issym() else link.parts))
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                if member.issym():
                    os.symlink(os.path.relpath(source_path, destination.parent), destination,
                               target_is_directory=source_path.is_dir())
                else:
                    os.link(source_path, destination)
            except (OSError, NotImplementedError):
                if source_path.is_file():
                    with source_path.open("rb") as source, destination.open("wb") as output:
                        _copy_checked(source, output, cancelled)
                elif source_path.is_dir():
                    shutil.copytree(source_path, destination, dirs_exist_ok=True)
                else:
                    raise PrerequisiteFetchError(
                        "ARCHIVE_LINK_UNRESOLVED: package link target is missing."
                    ) from None
    return notices


def _merge_staged_tree(source: Path, target: Path) -> None:
    """Move one fully extracted artifact into its shared install prefix."""
    for path in sorted(source.rglob("*"), key=lambda value: len(value.parts)):
        relative = path.relative_to(source)
        destination = target / relative
        if path.is_dir() and not path.is_symlink():
            if destination.exists() and not destination.is_dir():
                raise PrerequisiteFetchError(
                    "INSTALL_PATH_CONFLICT: an installed file conflicts with an artifact directory."
                )
            destination.mkdir(parents=True, exist_ok=True)
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and destination.is_dir() and not destination.is_symlink():
            raise PrerequisiteFetchError(
                "INSTALL_PATH_CONFLICT: an installed directory conflicts with an artifact file."
            )
        os.replace(path, destination)


def _is_running_executable(path: Path) -> bool:
    """Return whether this process is running from the supplied executable path."""
    try:
        return Path(sys.executable).resolve(strict=False) == path.resolve(strict=False)
    except (OSError, RuntimeError):
        return False


def _write_install_record(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(record, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _outside_repository(path: Path, what: str) -> Path:
    path = Path(path).resolve(strict=False)
    if path == REPOSITORY_ROOT or REPOSITORY_ROOT in path.parents:
        raise PrerequisiteFetchError(f"INSTALL_ROOT_REJECTED: {what} cannot be stored in the repository.")
    return path


def install_items(
    manifest: dict,
    data_root: Path,
    *,
    progress: ProgressCallback | None = None,
    retries: int = 3,
    allow_http: bool = False,
    cancelled: CancelCheck | None = None,
) -> dict:
    """Install all ready artifacts beneath app data and atomically record verified versions."""
    if not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list):
        raise PrerequisiteFetchError("MANIFEST_INVALID: artifacts must be a list.")
    install_root = _outside_repository(data_root, "prerequisites") / "prerequisites"
    return _install_artifacts(
        manifest, install_root, install_root / "downloads",
        progress=progress, retries=retries, allow_http=allow_http, cancelled=cancelled,
        record_each=True,
    )


def _install_artifacts(
    manifest: dict,
    install_root: Path,
    downloads_root: Path,
    *,
    progress: ProgressCallback | None,
    retries: int,
    allow_http: bool,
    cancelled: CancelCheck | None,
    record_each: bool,
) -> dict:
    """Download, verify and unpack every ready artifact into install_root's layout.

    record_each rewrites install_root/installed.json after every artifact, so an
    interrupted per-user install still lists what it finished.
    """
    binary_root = install_root / "msys64"
    python_root = install_root / "python"
    notices_root = install_root / "notices"
    staging_root = install_root / ".staging"
    records: dict[str, dict] = {}

    def report_progress(item_id: str, received: int, expected: int) -> None:
        if cancelled and cancelled():
            raise PrerequisiteFetchError("INSTALL_CANCELLED: download cancelled by the user.")
        if progress:
            progress(item_id, received, expected)

    for item in manifest["artifacts"]:
        if item.get("status", "ready") != "ready":
            continue
        if cancelled and cancelled():
            raise PrerequisiteFetchError("INSTALL_CANCELLED: download cancelled by the user.")
        filename = item.get("filename")
        if not isinstance(filename, str) or Path(filename).name != filename or filename in {".", ".."}:
            raise PrerequisiteFetchError("MANIFEST_INVALID: artifact filename must be a simple file name.")
        archive = download_verified_item(
            item, downloads_root / item["id"] / filename,
            progress=report_progress if progress or cancelled else None,
            retries=retries, allow_http=allow_http,
        )
        format_name = item.get("archive_format", "none")
        install_subdir = item.get("install_subdir", "msys64")
        target_root = python_root if install_subdir == "python" else binary_root
        running_bootstrap_python = (
            item.get("id") == "cpython-embed-amd64"
            and install_subdir == "python"
            and _is_running_executable(python_root / "python.exe")
        )
        staging_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f"{item['id']}-", dir=staging_root) as temporary:
            item_stage = Path(temporary)
            staged_payload = item_stage / "payload"
            staged_notices = item_stage / "notices"
            staged_payload.mkdir()
            staged_notices.mkdir()
            if format_name == "zip":
                notice_paths = _extract_zip(item, archive, staged_payload,
                                            staged_notices, cancelled)
            elif format_name in {"tar.gz", "tar.zst"}:
                notice_paths = _extract_tar(item, archive, staged_payload,
                                            staged_notices, cancelled)
            elif format_name == "none":
                notice_paths = []
            else:
                raise PrerequisiteFetchError(f"MANIFEST_INVALID: unsupported archive format {format_name!r}.")
            if cancelled and cancelled():
                raise PrerequisiteFetchError("INSTALL_CANCELLED: extraction cancelled by the user.")
            # The native player has already hash-verified and installed this archive
            # before launching its embedded Python. Replacing that running executable
            # or its loaded OpenSSL DLLs fails on Windows, so verify the cached archive
            # and refresh notices without moving its payload over the live runtime.
            if not running_bootstrap_python:
                _merge_staged_tree(staged_payload, target_root)
            _merge_staged_tree(staged_notices, notices_root)
            # Record where each notice lives after the merge, relative to the install
            # root: the staging folder is gone, and a bundle must not carry the
            # packaging machine's paths.
            notice_paths = sorted(
                "notices/" + Path(notice).relative_to(staged_notices).as_posix()
                for notice in notice_paths
            )
        records[item["id"]] = {
            "version": item["version"],
            "size_bytes": item["size_bytes"],
            "sha256": item["sha256"],
            "url": item["url"],
            "license": item["license"],
            "archive": (archive.relative_to(install_root).as_posix()
                        if install_root in archive.parents else archive.name),
            "notices": notice_paths,
        }
        if record_each:
            _write_install_record(install_root / "installed.json", {
                "schema_version": 1,
                "installed": records,
            })
    if cancelled and cancelled():
        raise PrerequisiteFetchError("INSTALL_CANCELLED: installation cancelled by the user.")
    return records


# ---- Release bundle (#1285) -------------------------------------------------
# A release zip carries the same tree a per-user install creates, in prereqs/
# beside bin/ and source/, so BUILD PACKAGE needs no download. The native player
# (src/player/package_builder.c) checks installed.json against the pinned manifest
# and every inventory line's presence and size; this module also checks contents.

BUNDLE_RECORD = "installed.json"
BUNDLE_INVENTORY = "bundle-inventory.txt"
BUNDLE_TREES = ("msys64", "notices", "python")
# What BUILD PACKAGE runs from the bundle; a bundle without them is useless.
BUNDLE_REQUIRED_FILES = (
    "python/python.exe",
    "msys64/ucrt64/bin/gcc.exe",
    "msys64/ucrt64/bin/mingw32-make.exe",
)
# Owner decision (2026-10-26, #1285): the GFDL-licensed documentation in these folders
# of every msys64 prefix is not needed to build and is not shipped in prereqs/. Every
# package's licence and copying files stay in notices/, and the full upstream
# documentation is in the corresponding-source archive.
BUNDLE_STRIPPED_DOCUMENTATION = ("share/doc", "share/info", "share/man")
# Names the source file of every bundled artifact (see _source_notice).
BUNDLE_SOURCE_NOTICE = "notices/SOURCES.txt"


def _check_bundle_path(relative: str) -> str:
    parts = relative.split("/")
    if (parts[0] not in BUNDLE_TREES or len(parts) < 2
            or any(part in {"", ".", ".."} for part in parts)
            or any(ord(ch) < 0x20 or ch in "\x7f\\:" for ch in relative)):
        raise PrerequisiteFetchError(f"BUNDLE_PATH_REJECTED: {relative!r} cannot be listed in the bundle inventory.")
    return relative


def _materialize_links(root: Path) -> None:
    """Replace every link under root with a copy of its target: a zip cannot carry links."""
    while True:
        links = [Path(directory) / name
                 for directory, dirnames, filenames in os.walk(root)
                 for name in dirnames + filenames
                 if (Path(directory) / name).is_symlink()]
        if not links:
            return
        for link in links:
            target = link.resolve(strict=False)
            if target != root and root not in target.parents:
                raise PrerequisiteFetchError("BUNDLE_LINK_REJECTED: a package link points outside the bundle.")
            if not target.exists():
                raise PrerequisiteFetchError("ARCHIVE_LINK_UNRESOLVED: package link target is missing.")
            link.unlink()
            if target.is_dir():
                shutil.copytree(target, link, symlinks=True)
            else:
                shutil.copyfile(target, link)


def _is_stripped_documentation(relative: str) -> bool:
    parts = relative.split("/")
    return (len(parts) > 4 and parts[0] == "msys64"
            and "/".join(parts[2:4]) in BUNDLE_STRIPPED_DOCUMENTATION)


def _strip_documentation(root: Path) -> dict:
    """Remove share/doc, share/info and share/man from every msys64 prefix of a staged bundle."""
    msys = root / "msys64"
    prefixes = sorted(path for path in msys.iterdir() if path.is_dir()) if msys.is_dir() else []
    removed: list[str] = []
    file_count = 0
    removed_bytes = 0
    for prefix in prefixes:
        for relative in BUNDLE_STRIPPED_DOCUMENTATION:
            folder = prefix / relative
            if not folder.exists():
                continue
            name = folder.relative_to(root).as_posix()
            if folder.is_symlink() or not folder.is_dir():
                raise PrerequisiteFetchError(f"BUNDLE_PATH_REJECTED: {name} is not a folder.")
            for directory, _dirnames, filenames in os.walk(folder):
                for filename in filenames:
                    file_count += 1
                    removed_bytes += (Path(directory) / filename).stat().st_size
            shutil.rmtree(folder)
            removed.append(name)
    return {"paths": removed, "file_count": file_count, "bytes": removed_bytes}


def _bundle_files(root: Path) -> list[str]:
    files: list[str] = []
    for tree in BUNDLE_TREES:
        for directory, _dirnames, filenames in os.walk(root / tree):
            for name in filenames:
                files.append(_check_bundle_path((Path(directory) / name).relative_to(root).as_posix()))
    return sorted(files)


def _bundle_inventory(root: Path) -> list[tuple[str, int, str]]:
    return [(_sha256_file(root / relative), (root / relative).stat().st_size, relative)
            for relative in _bundle_files(root)]


def stage_bundle(
    manifest_path: Path,
    destination: Path,
    download_cache: Path,
    *,
    progress: ProgressCallback | None = None,
    retries: int = 3,
    allow_http: bool = False,
    required_files: tuple[str, ...] = BUNDLE_REQUIRED_FILES,
) -> dict:
    """Stage the verified, unpacked prerequisite tree for a release zip's prereqs/ folder.

    Every artifact is downloaded into download_cache (or reused from it) and checked
    against its pinned size and SHA-256 before it is unpacked with the per-user
    installer's own extraction rules. The GFDL-licensed documentation folders are
    stripped, notices/SOURCES.txt names each artifact's pinned source file, and the
    result is verified again before returning. A manifest without a pinned source for
    every artifact is refused: the bundle never ships without its corresponding source.
    """
    manifest_path = Path(manifest_path)
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list):
        raise PrerequisiteFetchError("MANIFEST_INVALID: artifacts must be a list.")
    sources = _pinned_sources(manifest, allow_http=allow_http)
    destination = _outside_repository(destination, "the prerequisite bundle")
    download_cache = _outside_repository(download_cache, "the bundle download cache")
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise PrerequisiteFetchError("BUNDLE_DESTINATION_NOT_EMPTY: stage the bundle into a new, empty folder.")
    if download_cache == destination or destination in download_cache.parents:
        raise PrerequisiteFetchError("BUNDLE_CACHE_REJECTED: keep the download cache outside the bundle.")
    destination.mkdir(parents=True, exist_ok=True)
    records = _install_artifacts(
        manifest, destination, download_cache,
        progress=progress, retries=retries, allow_http=allow_http, cancelled=None,
        record_each=False,
    )
    shutil.rmtree(destination / ".staging", ignore_errors=True)
    _materialize_links(destination)
    stripped = _strip_documentation(destination)
    for required in required_files:
        if not (destination / required).is_file():
            raise PrerequisiteFetchError(f"BUNDLE_INCOMPLETE: the pinned artifacts do not provide {required}.")
    with (destination / BUNDLE_SOURCE_NOTICE).open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(_source_notice(manifest_bytes, manifest, sources))
    entries = _bundle_inventory(destination)
    with (destination / BUNDLE_INVENTORY).open("w", encoding="utf-8", newline="\n") as stream:
        for digest, size, relative in entries:
            stream.write(f"{digest} {size} {relative}\n")
    bundle = {
        "schema_version": 1,
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "inventory": BUNDLE_INVENTORY,
        "file_count": len(entries),
        "unpacked_bytes": sum(size for _, size, _ in entries),
        "stripped_documentation": stripped,
    }
    _write_install_record(destination / BUNDLE_RECORD, {
        "schema_version": 1,
        "bundle": bundle,
        "installed": records,
    })
    return verify_bundle(manifest_path, destination)


def verify_bundle(manifest_path: Path, bundle_root: Path) -> dict:
    """Check a staged or extracted bundle completely, file contents included.

    The record must name the manifest's exact bytes and hold exactly its ready
    artifacts with their pinned version, size and SHA-256; every inventory line must
    match its file's size and SHA-256; no file outside the inventory may exist
    under python/, msys64/ or notices/; no stripped documentation folder may hold a
    file; and notices/SOURCES.txt must name the manifest's pinned sources. Returns the
    record's bundle section.
    """
    manifest_bytes = Path(manifest_path).read_bytes()
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list):
        raise PrerequisiteFetchError("MANIFEST_INVALID: artifacts must be a list.")
    # Verification downloads nothing: only the pinned names, sizes and digests matter.
    sources = _pinned_sources(manifest, allow_http=True)
    bundle_root = Path(bundle_root)
    try:
        record = json.loads((bundle_root / BUNDLE_RECORD).read_text(encoding="utf-8"))
        bundle = record["bundle"]
        installed = record["installed"]
        if (record.get("schema_version") != 1 or bundle.get("schema_version") != 1
                or bundle.get("inventory") != BUNDLE_INVENTORY or not isinstance(installed, dict)):
            raise KeyError("schema")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise PrerequisiteFetchError("BUNDLE_INVALID: installed.json is not a schema 1 bundle record.") from exc
    if bundle.get("manifest_sha256") != hashlib.sha256(manifest_bytes).hexdigest():
        raise PrerequisiteFetchError("BUNDLE_INVALID: the bundle was staged from a different manifest.")
    ready = {item["id"]: item for item in manifest.get("artifacts", []) if item.get("status", "ready") == "ready"}
    if set(installed) != set(ready):
        raise PrerequisiteFetchError("BUNDLE_INVALID: the bundle does not hold exactly the pinned artifacts.")
    for item_id, item in ready.items():
        entry = installed[item_id]
        if (entry.get("version") != item["version"] or entry.get("size_bytes") != item["size_bytes"]
                or str(entry.get("sha256", "")).lower() != item["sha256"].lower()):
            raise PrerequisiteFetchError(f"BUNDLE_INVALID: {item_id} differs from the pinned manifest.")
    listed: dict[str, tuple[str, int]] = {}
    try:
        lines = (bundle_root / BUNDLE_INVENTORY).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise PrerequisiteFetchError("BUNDLE_INVALID: the file inventory is missing or unreadable.") from exc
    for number, line in enumerate(lines, 1):
        digest, _, rest = line.partition(" ")
        size_text, _, relative = rest.partition(" ")
        if (len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest)
                or not size_text.isdigit() or relative in listed):
            raise PrerequisiteFetchError(f"BUNDLE_INVALID: inventory line {number} is malformed.")
        listed[_check_bundle_path(relative)] = (digest, int(size_text))
        if _is_stripped_documentation(relative):
            raise PrerequisiteFetchError(f"BUNDLE_INVALID: {relative} is documentation the bundle does not ship.")
    if len(listed) != bundle.get("file_count") or sum(size for _, size in listed.values()) != bundle.get("unpacked_bytes"):
        raise PrerequisiteFetchError("BUNDLE_INVALID: the file inventory does not match its record.")
    for relative, (digest, size) in listed.items():
        path = bundle_root / relative
        if not path.is_file() or path.is_symlink():
            raise PrerequisiteFetchError(f"BUNDLE_INVALID: {relative} is missing.")
        if path.stat().st_size != size or _sha256_file(path) != digest:
            raise PrerequisiteFetchError(f"BUNDLE_INVALID: {relative} does not match its recorded size and SHA-256.")
    unlisted = sorted(set(_bundle_files(bundle_root)) - set(listed))
    if unlisted:
        raise PrerequisiteFetchError(f"BUNDLE_INVALID: {unlisted[0]} is not in the file inventory.")
    if (BUNDLE_SOURCE_NOTICE not in listed or (bundle_root / BUNDLE_SOURCE_NOTICE).read_bytes()
            != _source_notice(manifest_bytes, manifest, sources).encode("utf-8")):
        raise PrerequisiteFetchError(f"BUNDLE_INVALID: {BUNDLE_SOURCE_NOTICE} does not name the pinned sources.")
    return bundle


# ---- Corresponding source (#1285) -------------------------------------------
# Owner decision (2026-10-26): the release ships the source of every bundled
# package, not only the copyleft ones, as one archive published next to the zip,
# instead of a written offer. Every ready artifact names an entry of the
# manifest's "sources" list: the MSYS2 source package (*.src.tar.zst) of its
# pkgbase at the same version, or the python.org source release of CPython. Each
# entry is pinned by URL, size and SHA-256 like the binaries, and a version bump
# that leaves an artifact's source behind is refused.

SOURCE_ARCHIVE_NOTICE = "SOURCES.txt"
# Fixed member metadata, so the archive's bytes depend only on the pinned files.
_SOURCE_ARCHIVE_TIME = (1980, 1, 1, 0, 0, 0)


def _pinned_sources(manifest: dict, *, allow_http: bool) -> list[dict]:
    """Return the manifest's source pins after checking them and their artifact mapping."""
    sources = manifest.get("sources")
    if not isinstance(sources, list) or not sources:
        raise PrerequisiteFetchError("SOURCE_UNPINNED: the manifest lists no pinned sources.")
    by_id: dict[str, dict] = {}
    filenames: set[str] = set()
    for source in sources:
        if not isinstance(source, dict):
            raise PrerequisiteFetchError("MANIFEST_INVALID: a source entry is not an object.")
        _validated_item(source, allow_http=allow_http)
        filename = source.get("filename")
        if (not isinstance(filename, str) or PurePosixPath(filename).name != filename or "\\" in filename
                or filename in {".", "..", SOURCE_ARCHIVE_NOTICE}):
            raise PrerequisiteFetchError(f"MANIFEST_INVALID: source {source['id']} needs a simple file name.")
        if not isinstance(source.get("version"), str) or not source["version"]:
            raise PrerequisiteFetchError(f"MANIFEST_INVALID: source {source['id']} has no version.")
        if source["id"] in by_id or filename in filenames:
            raise PrerequisiteFetchError(f"MANIFEST_INVALID: source {source['id']} is listed twice.")
        by_id[source["id"]] = source
        filenames.add(filename)
    named: set[str] = set()
    for item in manifest["artifacts"]:
        if item.get("status", "ready") != "ready":
            continue
        source = by_id.get(item.get("source"))
        if source is None:
            raise PrerequisiteFetchError(f"SOURCE_UNPINNED: {item.get('id')} names no pinned source.")
        if source["version"] != item.get("version"):
            raise PrerequisiteFetchError(
                f"SOURCE_VERSION_MISMATCH: {item['id']} {item.get('version')} names source "
                f"{source['id']} {source['version']}."
            )
        named.add(source["id"])
    unused = sorted(set(by_id) - named)
    if unused:
        raise PrerequisiteFetchError(f"MANIFEST_INVALID: source {unused[0]} is not named by any bundled artifact.")
    return sources


def _source_notice(manifest_bytes: bytes, manifest: dict, sources: list[dict]) -> str:
    """The text of notices/SOURCES.txt and of the source archive's SOURCES.txt."""
    built_from: dict[str, list[dict]] = {}
    for item in manifest["artifacts"]:
        if item.get("status", "ready") == "ready":
            built_from.setdefault(item["source"], []).append(item)
    lines = [
        "Corresponding source of the bundled build prerequisites",
        "",
        "The prereqs/ folder of the Nakagawa Recomp release zip holds the build prerequisites",
        "pinned in source/assets/prereq_manifest.json. The source of every one of them is",
        "published with the same release as one archive next to the zip,",
        "nakagawa-recomp-<version>-prerequisite-sources.zip: the MSYS2 source package",
        "(*.src.tar.zst) of each MSYS2 package and the python.org source release of CPython.",
        "Each file below is pinned by size and SHA-256 in that manifest. The documentation",
        "folders share/doc, share/info and share/man are not shipped in prereqs/; the",
        "licence and copying files of every package are in prereqs/notices.",
        "",
        f"Manifest SHA-256: {hashlib.sha256(manifest_bytes).hexdigest()}",
    ]
    for source in sources:
        lines += [
            "",
            source["filename"],
            f"  SHA-256: {source['sha256'].lower()}",
            f"  Size: {source['size_bytes']} bytes",
            f"  From: {source['url']}",
        ]
        lines += [f"  Source of: {item['id']} {item['version']} ({item.get('license', 'no licence recorded')})"
                  for item in built_from[source["id"]]]
    return "\n".join(lines) + "\n"


def _source_archive_info(name: str, size: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, _SOURCE_ARCHIVE_TIME)
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.compress_type = zipfile.ZIP_STORED
    info.file_size = size
    return info


def stage_sources(
    manifest_path: Path,
    archive_path: Path,
    download_cache: Path,
    *,
    progress: ProgressCallback | None = None,
    retries: int = 3,
    allow_http: bool = False,
) -> dict:
    """Write the corresponding-source archive published next to the release zip.

    Every pinned source file is downloaded into download_cache/sources (or reused
    from it) and checked against its pinned size and SHA-256, then stored unchanged
    in one zip under a single folder named after the archive, with SOURCES.txt. The
    archive is verified again, contents included, before returning.
    """
    manifest_path = Path(manifest_path)
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list):
        raise PrerequisiteFetchError("MANIFEST_INVALID: artifacts must be a list.")
    sources = _pinned_sources(manifest, allow_http=allow_http)
    archive_path = _outside_repository(archive_path, "the source archive")
    download_cache = _outside_repository(download_cache, "the source download cache")
    if archive_path.suffix.lower() != ".zip" or not archive_path.stem:
        raise PrerequisiteFetchError("SOURCE_ARCHIVE_REJECTED: name the source archive <name>.zip.")
    if archive_path.exists():
        raise PrerequisiteFetchError("SOURCE_ARCHIVE_EXISTS: write the source archive to a new file.")
    files = [
        (source, download_verified_item(
            source, download_cache / "sources" / source["id"] / source["filename"],
            progress=progress, retries=retries, allow_http=allow_http,
        ))
        for source in sources
    ]
    notice = _source_notice(manifest_bytes, manifest, sources).encode("utf-8")
    root = archive_path.stem
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    partial = archive_path.with_name(archive_path.name + ".part")
    try:
        with zipfile.ZipFile(partial, "w", allowZip64=True) as archive:
            archive.writestr(_source_archive_info(f"{root}/{SOURCE_ARCHIVE_NOTICE}", len(notice)), notice)
            for source, path in files:
                info = _source_archive_info(f"{root}/{source['filename']}", path.stat().st_size)
                with path.open("rb") as stream, archive.open(info, "w") as output:
                    shutil.copyfileobj(stream, output, CHUNK_SIZE)
        os.replace(partial, archive_path)
    finally:
        partial.unlink(missing_ok=True)
    return verify_source_archive(manifest_path, archive_path)


def verify_source_archive(manifest_path: Path, archive_path: Path) -> dict:
    """Check a corresponding-source archive against the manifest's source pins.

    The archive must hold one top-level folder with exactly SOURCES.txt, matching the
    manifest, and every pinned source file with its pinned size and SHA-256.
    """
    manifest_bytes = Path(manifest_path).read_bytes()
    manifest = json.loads(manifest_bytes)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list):
        raise PrerequisiteFetchError("MANIFEST_INVALID: artifacts must be a list.")
    # Verification downloads nothing: only the pinned names, sizes and digests matter.
    sources = _pinned_sources(manifest, allow_http=True)
    archive_path = Path(archive_path)
    try:
        package = zipfile.ZipFile(archive_path)
    except (OSError, zipfile.BadZipFile) as exc:
        raise PrerequisiteFetchError("SOURCE_ARCHIVE_INVALID: the source archive is missing or not a zip.") from exc
    with package:
        names = [info.filename for info in package.infolist()]
        roots = {name.partition("/")[0] for name in names}
        if len(roots) != 1 or len(set(names)) != len(names) or any(name.count("/") != 1 for name in names):
            raise PrerequisiteFetchError("SOURCE_ARCHIVE_INVALID: the archive must hold its files in one folder.")
        root = roots.pop()
        expected = {f"{root}/{SOURCE_ARCHIVE_NOTICE}"} | {f"{root}/{source['filename']}" for source in sources}
        missing = sorted(expected - set(names))
        if missing:
            raise PrerequisiteFetchError(f"SOURCE_ARCHIVE_INVALID: {missing[0]} is missing.")
        unlisted = sorted(set(names) - expected)
        if unlisted:
            raise PrerequisiteFetchError(f"SOURCE_ARCHIVE_INVALID: {unlisted[0]} is not a pinned source.")
        try:
            if package.read(f"{root}/{SOURCE_ARCHIVE_NOTICE}") != _source_notice(
                    manifest_bytes, manifest, sources).encode("utf-8"):
                raise PrerequisiteFetchError(
                    f"SOURCE_ARCHIVE_INVALID: {SOURCE_ARCHIVE_NOTICE} does not match the manifest.")
            for source in sources:
                digest = hashlib.sha256()
                size = 0
                with package.open(f"{root}/{source['filename']}") as stream:
                    while block := stream.read(CHUNK_SIZE):
                        digest.update(block)
                        size += len(block)
                if size != source["size_bytes"] or digest.hexdigest() != source["sha256"].lower():
                    raise PrerequisiteFetchError(
                        f"SOURCE_ARCHIVE_INVALID: {source['filename']} does not match its pinned size and SHA-256.")
        except (OSError, zipfile.BadZipFile) as exc:
            raise PrerequisiteFetchError(f"SOURCE_ARCHIVE_INVALID: the archive cannot be read: {exc}") from exc
    return {
        "schema_version": 1,
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "source_count": len(sources),
        "source_bytes": sum(source["size_bytes"] for source in sources),
        "archive_bytes": archive_path.stat().st_size,
        "archive_sha256": _sha256_file(archive_path),
    }


def _bundle_main(args: argparse.Namespace) -> int:
    kind = "sources" if args.source_archive or args.verify_source_archive else "bundle"
    try:
        if (args.bundle_dest or args.source_archive) and not args.download_cache:
            raise PrerequisiteFetchError(
                "BUNDLE_CACHE_REJECTED: --bundle-dest and --source-archive need --download-cache.")
        if args.bundle_dest:
            result = stage_bundle(args.manifest, args.bundle_dest, args.download_cache)
            event = "bundle-staged"
        elif args.source_archive:
            result = stage_sources(args.manifest, args.source_archive, args.download_cache)
            event = "sources-staged"
        elif args.verify_source_archive:
            result = verify_source_archive(args.manifest, args.verify_source_archive)
            event = "sources-verified"
        else:
            result = verify_bundle(args.manifest, args.verify_bundle)
            event = "bundle-verified"
    except PrerequisiteFetchError as exc:
        print(json.dumps({"event": f"{kind}-failed", "code": str(exc).partition(":")[0], "message": str(exc)}))
        return 2
    except json.JSONDecodeError as exc:
        print(json.dumps({"event": f"{kind}-failed", "code": "MANIFEST_INVALID", "message": str(exc)}))
        return 2
    except OSError as exc:
        print(json.dumps({"event": f"{kind}-failed", "code": "BUNDLE_IO_FAILED", "message": str(exc)}))
        return 2
    print(json.dumps({"event": event, **result}))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Install verified build prerequisites into per-user data.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--data-root", type=Path)
    parser.add_argument("--progress-file", type=Path)
    parser.add_argument("--cancel-file", type=Path)
    parser.add_argument("--progress-base", type=int, default=0)
    parser.add_argument("--total-bytes", type=int)
    bundle = parser.add_mutually_exclusive_group()
    bundle.add_argument("--bundle-dest", type=Path,
                        help="release packaging: stage the verified tree for the zip's prereqs/ folder here")
    bundle.add_argument("--verify-bundle", type=Path,
                        help="check a staged or extracted prereqs/ folder completely, file contents included")
    bundle.add_argument("--source-archive", type=Path,
                        help="release packaging: write the corresponding-source archive (a new .zip) here")
    bundle.add_argument("--verify-source-archive", type=Path,
                        help="check a corresponding-source archive against the pinned sources, contents included")
    parser.add_argument("--download-cache", type=Path,
                        help="with --bundle-dest or --source-archive: keep and reuse verified archives here, "
                             "outside the bundle")
    args = parser.parse_args(argv)
    if args.bundle_dest or args.verify_bundle or args.source_archive or args.verify_source_archive:
        return _bundle_main(args)
    progress_stream = args.progress_file.open("a", encoding="utf-8") if args.progress_file else sys.stdout
    received_by_item: dict[str, int] = {}
    total_bytes = args.total_bytes

    def report(item_id: str, received: int, expected: int) -> None:
        received_by_item[item_id] = received
        received_total = args.progress_base + sum(received_by_item.values())
        progress_stream.write(json.dumps({
            "event": "download-progress", "item_id": item_id,
            "received_bytes": received, "expected_bytes": expected,
            "total_received_bytes": received_total, "total_bytes": total_bytes,
        }) + "\n")
        progress_stream.flush()

    def cancelled() -> bool:
        return bool(args.cancel_file and args.cancel_file.exists())

    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if total_bytes is None:
            total_bytes = manifest.get("total_download_bytes")
        data_root = args.data_root or default_data_root()
        records = install_items(manifest, data_root, progress=report, cancelled=cancelled)
        progress_stream.write(json.dumps({
            "event": "install-complete", "installed": sorted(records),
            "total_received_bytes": args.progress_base + sum(received_by_item.values()),
            "total_bytes": total_bytes,
        }) + "\n")
        progress_stream.flush()
        return 0
    except OSError as exc:
        if exc.errno == errno.ENOSPC:
            code = "DISK_FULL"
            message = "DISK_FULL: App data ran out of space while installing verified build tools. Free disk space and retry."
        else:
            code = "INSTALL_WRITE_FAILED"
            message = f"INSTALL_WRITE_FAILED: Could not write verified build tools in app data: {exc}"
        progress_stream.write(json.dumps({"event": "install-failed", "code": code, "message": message}) + "\n")
        progress_stream.flush()
        return 2
    except json.JSONDecodeError as exc:
        progress_stream.write(json.dumps({
            "event": "install-failed", "code": "MANIFEST_INVALID",
            "message": f"MANIFEST_INVALID: Could not parse the pinned prerequisite manifest: {exc}",
        }) + "\n")
        progress_stream.flush()
        return 2
    except PrerequisiteFetchError as exc:
        code = str(exc).partition(":")[0]
        progress_stream.write(json.dumps({"event": "install-failed", "code": code, "message": str(exc)}) + "\n")
        progress_stream.flush()
        return 2
    finally:
        if args.progress_file:
            progress_stream.close()


if __name__ == "__main__":
    raise SystemExit(main())
