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
import shutil
import sys
import tarfile
import tempfile
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
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
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or os.environ.get("USERPROFILE")
        if not base:
            raise PrerequisiteFetchError("DATA_DIR_UNAVAILABLE: Windows per-user data is unavailable.")
        return Path(base) / "Nakagawa" / "data"
    if os.name == "posix" and os.environ.get("XDG_DATA_HOME"):
        return Path(os.environ["XDG_DATA_HOME"]) / "nakagawa-recomp"
    return Path.home() / ".local" / "share" / "nakagawa-recomp"


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

    opener = build_opener(_AllowListedRedirects(hosts, allow_http=allow_http))
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


def _copy_notices(item_id: str, archive_member: str, source: Path, notices_root: Path) -> str | None:
    normalized = archive_member.replace("\\", "/")
    lower = normalized.lower()
    if "/share/licenses/" not in f"/{lower}" and Path(normalized).name.lower() not in {
        "license", "license.txt", "copying", "copying.txt", "notice", "notice.txt", "copyright"
    }:
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
            f"ARCHIVE_UNSUPPORTED: Python 3.14 with zstd support is required for {item['id']}."
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
    data_root = Path(data_root).resolve(strict=False)
    if data_root == REPOSITORY_ROOT or REPOSITORY_ROOT in data_root.parents:
        raise PrerequisiteFetchError("INSTALL_ROOT_REJECTED: prerequisites cannot be stored in the repository.")
    install_root = data_root / "prerequisites"
    downloads_root = install_root / "downloads"
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
        records[item["id"]] = {
            "version": item["version"],
            "size_bytes": item["size_bytes"],
            "sha256": item["sha256"],
            "url": item["url"],
            "license": item["license"],
            "archive": archive.relative_to(install_root).as_posix(),
            "notices": notice_paths,
        }
        _write_install_record(install_root / "installed.json", {
            "schema_version": 1,
            "installed": records,
        })
    if cancelled and cancelled():
        raise PrerequisiteFetchError("INSTALL_CANCELLED: installation cancelled by the user.")
    return records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Install verified build prerequisites into per-user data.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--data-root", type=Path)
    parser.add_argument("--progress-file", type=Path)
    parser.add_argument("--cancel-file", type=Path)
    parser.add_argument("--progress-base", type=int, default=0)
    parser.add_argument("--total-bytes", type=int)
    args = parser.parse_args(argv)
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
