# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""No-network tests for the pinned prerequisite download and install path."""

from __future__ import annotations

import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import os
import re
from pathlib import Path, PurePosixPath
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
REPOSITORY_ROOT_FOR_TESTS = Path(__file__).resolve().parent.parent

from nk_core.prereq_fetcher import (
    PrerequisiteFetchError,
    default_data_root,
    download_verified_item,
    install_items,
    main as fetcher_main,
    stage_bundle,
    verify_bundle,
)


def _zip_payload() -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("python314/LICENSE.txt", "Python license text\n")
        archive.writestr("python314/python.exe", b"synthetic fixture\n")
    return stream.getvalue()


class _MockPosixPath(PurePosixPath):
    __slots__ = ()

    @classmethod
    def home(cls) -> _MockPosixPath:
        return cls(os.environ["HOME"])

    def is_dir(self) -> bool:
        return False


class _PayloadHandler(BaseHTTPRequestHandler):
    payload = b""
    payloads: dict[str, bytes] = {}
    mode = "ok"
    requests = 0

    def do_GET(self) -> None:
        type(self).requests += 1
        if self.path == "/foreign-redirect":
            self.send_response(302)
            self.send_header("Location", f"http://localhost:{self.server.server_port}/payload")
            self.end_headers()
            return
        body = type(self).payloads.get(self.path, type(self).payload)
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if type(self).mode == "interrupt" and type(self).requests == 1:
            self.wfile.write(body[: max(1, len(body) // 2)])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


class TestPrerequisiteFetcher(unittest.TestCase):
    def setUp(self) -> None:
        _PayloadHandler.payload = _zip_payload()
        _PayloadHandler.payloads = {}
        _PayloadHandler.mode = "ok"
        _PayloadHandler.requests = 0
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), _PayloadHandler)
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        self.temp = tempfile.TemporaryDirectory(prefix="nk-prereq-fetch-test-")
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join(timeout=5)
        self.temp.cleanup()

    def item(self, *, path: str = "/payload", payload: bytes | None = None) -> dict:
        body = _PayloadHandler.payload if payload is None else payload
        return {
            "id": "python-test",
            "version": "3.15-test",
            "url": f"http://127.0.0.1:{self.server.server_port}{path}",
            "allowed_hosts": ["127.0.0.1"],
            "filename": "payload.zip",
            "size_bytes": len(body),
            "sha256": hashlib.sha256(body).hexdigest(),
            "license": "PSF-2.0",
            "archive_format": "zip",
            "install_subdir": "python",
        }

    def test_windows_data_root_does_not_fall_back_to_userprofile(self) -> None:
        import nk_cli
        import package_notices
        from nk_core import fonts

        with patch.dict(os.environ, {
            "LOCALAPPDATA": "",
            "APPDATA": "",
            "USERPROFILE": str(self.root / "profile"),
        }, clear=True), patch.object(os, "name", "nt"):
            with self.assertRaises(PrerequisiteFetchError) as raised:
                default_data_root()
            with self.assertRaisesRegex(nk_cli.PackageBuildError, "DATA_DIR_UNAVAILABLE"):
                nk_cli.default_user_data_root()
            with self.assertRaisesRegex(PrerequisiteFetchError, "DATA_DIR_UNAVAILABLE"):
                fonts.default_user_data_root()
            self.assertIsNone(package_notices._prerequisite_toolchain_root())

        self.assertIn("DATA_DIR_UNAVAILABLE", str(raised.exception))
        self.assertIn("LOCALAPPDATA", str(raised.exception))
        self.assertIn("APPDATA", str(raised.exception))

    def test_python_data_root_callers_share_the_platform_rule(self) -> None:
        import nk_cli
        import nk_doctor_checks
        import nk_doctor_core
        import nk_core.prereq_fetcher as prereq_fetcher
        import package_notices
        from nk_core import fonts

        home = self.root / "profile"
        environment = {
            "LOCALAPPDATA": "",
            "APPDATA": "",
            "XDG_DATA_HOME": str(self.root / "xdg"),
            "HOME": str(home),
            "USERPROFILE": str(home),
        }
        expected = _MockPosixPath(str(home)) / "Library" / "Application Support" / "NakagawaRecomp" / "data"
        with patch.dict(os.environ, environment, clear=True), \
             patch.object(os, "name", "posix"), \
             patch.object(sys, "platform", "darwin"), \
             patch.object(prereq_fetcher, "Path", _MockPosixPath), \
             patch.object(nk_cli, "Path", _MockPosixPath), \
             patch.object(fonts, "Path", _MockPosixPath), \
             patch.object(package_notices, "Path", _MockPosixPath), \
             patch.object(nk_doctor_checks, "Path", _MockPosixPath):
            roots = {
                "prerequisite fetcher": default_data_root(),
                "CLI": nk_cli.default_user_data_root(),
                "fonts": fonts.default_user_data_root(),
            }
            toolchain_root = package_notices._prerequisite_toolchain_root()
            self.assertIsNotNone(toolchain_root)
            roots["package notices"] = toolchain_root.parents[2]
            report = nk_doctor_core.Report(_MockPosixPath("/repo"), "repo")
            nk_doctor_checks.check_data_directory(report)
            roots["Doctor"] = next(
                result.path for result in report.results if result.code == "DATA_DIR_ROOT"
            )

        self.assertEqual(roots, {name: expected for name in roots})

    def test_happy_path_extracts_only_verified_payload_and_records_license(self) -> None:
        item = self.item()
        result = install_items({"artifacts": [item]}, self.root, allow_http=True)
        install_root = self.root / "prerequisites"
        self.assertEqual((install_root / "python" / "python314" / "python.exe").read_bytes(), b"synthetic fixture\n")
        notice = install_root / "notices" / "python-test" / "python314" / "LICENSE.txt"
        self.assertEqual(notice.read_text(encoding="utf-8"), "Python license text\n")
        self.assertEqual(result["python-test"]["sha256"], item["sha256"])
        self.assertTrue((install_root / "installed.json").is_file())

    def test_msys2_libstdcxx_artifact_id_allows_plus_characters(self) -> None:
        item = self.item()
        item["id"] = "mingw-w64-ucrt-x86_64-libstdc++"
        result = install_items({"artifacts": [item]}, self.root, allow_http=True)
        self.assertEqual(result[item["id"]]["sha256"], item["sha256"])

    def test_current_cpython_fetcher_does_not_replace_its_running_runtime(self) -> None:
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("python.exe", b"archive runtime\n")
            archive.writestr("libcrypto-3.dll", b"archive crypto\n")
            archive.writestr("LICENSE.txt", "Python license text\n")
        _PayloadHandler.payload = stream.getvalue()
        item = self.item(payload=_PayloadHandler.payload)
        item["id"] = "cpython-embed-amd64"
        item["filename"] = "python-3.15-embed-amd64.zip"
        python_root = self.root / "prerequisites" / "python"
        python_root.mkdir(parents=True)
        running_python = python_root / "python.exe"
        running_python.write_bytes(b"running bootstrap runtime\n")
        running_crypto = python_root / "libcrypto-3.dll"
        running_crypto.write_bytes(b"loaded OpenSSL library\n")

        with patch.object(sys, "executable", str(running_python)):
            records = install_items({"artifacts": [item]}, self.root, allow_http=True)

        self.assertEqual(running_python.read_bytes(), b"running bootstrap runtime\n")
        self.assertEqual(running_crypto.read_bytes(), b"loaded OpenSSL library\n")
        self.assertEqual(records[item["id"]]["sha256"], item["sha256"])
        notice = self.root / "prerequisites" / "notices" / item["id"] / "LICENSE.txt"
        self.assertTrue(notice.is_file())

    def test_hash_mismatch_fails_closed(self) -> None:
        item = self.item()
        item["sha256"] = "0" * 64
        with self.assertRaisesRegex(PrerequisiteFetchError, "HASH_MISMATCH"):
            download_verified_item(item, self.root / "payload.zip", allow_http=True)
        self.assertFalse((self.root / "payload.zip").exists())
        self.assertFalse((self.root / "payload.zip.part").exists())

    def test_size_mismatch_fails_closed_without_installing_partial_file(self) -> None:
        item = self.item()
        item["size_bytes"] += 1
        with self.assertRaisesRegex(PrerequisiteFetchError, "SIZE_MISMATCH"):
            download_verified_item(item, self.root / "payload.zip", allow_http=True)
        self.assertFalse((self.root / "payload.zip").exists())
        self.assertFalse((self.root / "payload.zip.part").exists())

    def test_redirect_to_foreign_host_is_rejected_before_following(self) -> None:
        item = self.item(path="/foreign-redirect")
        with self.assertRaisesRegex(PrerequisiteFetchError, "REDIRECT_REJECTED"):
            download_verified_item(item, self.root / "payload.zip", allow_http=True)
        self.assertEqual(_PayloadHandler.requests, 1)
        self.assertFalse((self.root / "payload.zip").exists())

    def test_interrupted_transfer_retries_from_clean_partial_and_succeeds(self) -> None:
        _PayloadHandler.mode = "interrupt"
        item = self.item()
        destination = download_verified_item(item, self.root / "payload.zip", allow_http=True)
        self.assertEqual(destination.read_bytes(), _PayloadHandler.payload)
        self.assertEqual(_PayloadHandler.requests, 2)
        self.assertFalse((self.root / "payload.zip.part").exists())

    def test_valid_cached_archive_reports_completed_progress_without_network(self) -> None:
        item = self.item()
        destination = download_verified_item(item, self.root / "payload.zip", allow_http=True)
        first_request_count = _PayloadHandler.requests
        progress: list[tuple[str, int, int]] = []
        cached = download_verified_item(
            item, destination,
            progress=lambda item_id, received, expected:
                progress.append((item_id, received, expected)),
            allow_http=True,
        )
        self.assertEqual(cached, destination)
        self.assertEqual(_PayloadHandler.requests, first_request_count)
        self.assertEqual(progress, [(item["id"], item["size_bytes"], item["size_bytes"])])

    def test_cached_archive_never_loads_urllib_and_a_download_still_does(self) -> None:
        # Fast path: a verified cached archive is served in a fresh interpreter
        # without urllib.request ever being imported. Slow path: the same item,
        # when absent, is fetched by that interpreter, makes exactly one request,
        # loads urllib, and verifies to the same bytes.
        destination = download_verified_item(self.item(), self.root / "payload.zip", allow_http=True)
        item = self.item()
        probe = (
            "import json, sys\n"
            "from pathlib import Path\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "from nk_core.prereq_fetcher import download_verified_item\n"
            "dest = download_verified_item(json.loads(sys.argv[2]), Path(sys.argv[3]), allow_http=True)\n"
            "print(json.dumps({'path': str(dest), 'urllib_loaded': 'urllib.request' in sys.modules}))\n"
        )
        tools = str(Path(__file__).resolve().parent)

        def run_probe(target: Path) -> dict:
            result = subprocess.run(
                [sys.executable, "-c", probe, tools, json.dumps(item), str(target)],
                capture_output=True, text=True, check=True,
            )
            return json.loads(result.stdout)

        before = _PayloadHandler.requests
        self.assertEqual(run_probe(destination),
                         {"path": str(destination), "urllib_loaded": False})
        self.assertEqual(_PayloadHandler.requests, before)

        fetched = self.root / "fetched.zip"
        self.assertEqual(run_probe(fetched), {"path": str(fetched), "urllib_loaded": True})
        self.assertEqual(_PayloadHandler.requests, before + 1)
        self.assertEqual(fetched.read_bytes(), destination.read_bytes())

    def test_cancel_during_transfer_discards_partial_archive(self) -> None:
        item = self.item()

        def cancel_after_chunk(_item_id: str, _received: int, _expected: int) -> None:
            raise PrerequisiteFetchError("INSTALL_CANCELLED: download cancelled by the user.")

        with self.assertRaisesRegex(PrerequisiteFetchError, "INSTALL_CANCELLED"):
            download_verified_item(item, self.root / "cancelled.zip", progress=cancel_after_chunk,
                                   allow_http=True)
        self.assertFalse((self.root / "cancelled.zip").exists())
        self.assertFalse((self.root / "cancelled.zip.part").exists())

    def test_cancel_before_install_leaves_no_component_files(self) -> None:
        item = self.item()
        with self.assertRaisesRegex(PrerequisiteFetchError, "INSTALL_CANCELLED"):
            install_items({"artifacts": [item]}, self.root, allow_http=True,
                          cancelled=lambda: True)
        install_root = self.root / "prerequisites"
        self.assertFalse((install_root / "python" / "python314" / "python.exe").exists())
        self.assertFalse((install_root / "installed.json").exists())

    def test_cancel_during_extraction_discards_staged_component(self) -> None:
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("python314/LICENSE.txt", "Python license text\n")
            archive.writestr("python314/python.exe", b"synthetic fixture\n" * 12000)
        _PayloadHandler.payload = stream.getvalue()
        item = self.item()
        cancel_checks = 0

        def cancel_during_copy() -> bool:
            nonlocal cancel_checks
            cancel_checks += 1
            return cancel_checks >= 7

        with self.assertRaisesRegex(PrerequisiteFetchError, "INSTALL_CANCELLED"):
            install_items({"artifacts": [item]}, self.root, allow_http=True,
                          cancelled=cancel_during_copy)
        install_root = self.root / "prerequisites"
        self.assertFalse((install_root / "python" / "python314" / "python.exe").exists())
        self.assertFalse((install_root / "installed.json").exists())
        staging_root = install_root / ".staging"
        self.assertTrue(not staging_root.exists() or not any(staging_root.iterdir()))

    def test_installed_record_names_notices_relative_to_the_install_root(self) -> None:
        # The notices were recorded at their temporary staging paths, which are
        # deleted after the merge and would put the packaging machine's folders
        # into a release bundle's installed.json (#1285).
        records = install_items({"artifacts": [self.item()]}, self.root, allow_http=True)
        self.assertEqual(records["python-test"]["notices"], ["notices/python-test/python314/LICENSE.txt"])
        install_root = self.root / "prerequisites"
        self.assertTrue((install_root / records["python-test"]["notices"][0]).is_file())

    # ---- Release bundle (#1285) ----------------------------------------------

    def bundle_manifest(self, *, msys_members: dict[str, bytes] | None = None) -> Path:
        python_zip = io.BytesIO()
        with zipfile.ZipFile(python_zip, "w") as archive:
            archive.writestr("python.exe", b"synthetic python\n")
            archive.writestr("LICENSE.txt", "Python license text\n")
        members = msys_members if msys_members is not None else {
            "ucrt64/bin/gcc.exe": b"synthetic gcc\n",
            "ucrt64/bin/mingw32-make.exe": b"synthetic make\n",
            "ucrt64/share/licenses/gcc/COPYING": b"GPL text\n",
        }
        msys_tar = io.BytesIO()
        with tarfile.open(fileobj=msys_tar, mode="w:gz") as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            if "ucrt64/bin/gcc.exe" in members:
                link = tarfile.TarInfo("ucrt64/bin/cc.exe")
                link.type = tarfile.SYMTYPE
                link.linkname = "gcc.exe"
                archive.addfile(link)
        artifacts = []
        for item_id, path, body, fmt, subdir in (
            ("cpython-embed-amd64", "/python.zip", python_zip.getvalue(), "zip", "python"),
            ("mingw-w64-ucrt-x86_64-gcc", "/gcc.pkg.tar.gz", msys_tar.getvalue(), "tar.gz", "msys64"),
        ):
            _PayloadHandler.payloads[path] = body
            artifacts.append({
                "id": item_id, "name": item_id, "version": "1.0-test",
                "url": f"http://127.0.0.1:{self.server.server_port}{path}",
                "allowed_hosts": ["127.0.0.1"], "filename": path.lstrip("/"),
                "size_bytes": len(body), "sha256": hashlib.sha256(body).hexdigest(),
                "license": "GPL-3.0-or-later", "archive_format": fmt, "install_subdir": subdir,
                "status": "ready",
            })
        manifest = self.root / f"prereq_manifest_{len(list(self.root.glob('prereq_manifest_*')))}.json"
        manifest.write_text(json.dumps({"schema_version": 1, "artifacts": artifacts}), encoding="utf-8")
        return manifest

    def test_bundle_stages_the_per_user_layout_with_a_full_inventory(self) -> None:
        manifest = self.bundle_manifest()
        dest = self.root / "release" / "prereqs"
        bundle = stage_bundle(manifest, dest, self.root / "cache", allow_http=True)
        self.assertEqual((dest / "python" / "python.exe").read_bytes(), b"synthetic python\n")
        self.assertEqual((dest / "msys64" / "ucrt64" / "bin" / "gcc.exe").read_bytes(), b"synthetic gcc\n")
        # A zip cannot carry links: the package's link is a regular copy of its target.
        cc = dest / "msys64" / "ucrt64" / "bin" / "cc.exe"
        self.assertFalse(cc.is_symlink())
        self.assertEqual(cc.read_bytes(), b"synthetic gcc\n")
        self.assertFalse((dest / ".staging").exists())
        self.assertFalse((dest / "downloads").exists())
        lines = (dest / "bundle-inventory.txt").read_text(encoding="utf-8").splitlines()
        listed = {}
        for line in lines:
            digest, size, relative = line.split(" ", 2)
            listed[relative] = (digest, int(size))
        self.assertEqual(sorted(listed), [
            "msys64/ucrt64/bin/cc.exe", "msys64/ucrt64/bin/gcc.exe",
            "msys64/ucrt64/bin/mingw32-make.exe", "msys64/ucrt64/share/licenses/gcc/COPYING",
            "notices/cpython-embed-amd64/LICENSE.txt",
            "notices/mingw-w64-ucrt-x86_64-gcc/ucrt64/share/licenses/gcc/COPYING",
            "python/LICENSE.txt", "python/python.exe",
        ])
        for relative, (digest, size) in listed.items():
            data = (dest / relative).read_bytes()
            self.assertEqual((hashlib.sha256(data).hexdigest(), len(data)), (digest, size), relative)
        record = json.loads((dest / "installed.json").read_text(encoding="utf-8"))
        self.assertEqual(record["bundle"], bundle)
        self.assertEqual(bundle["file_count"], len(lines))
        self.assertEqual(bundle["unpacked_bytes"], sum(size for _, size in listed.values()))
        self.assertEqual(bundle["manifest_sha256"], hashlib.sha256(manifest.read_bytes()).hexdigest())
        self.assertEqual(record["installed"]["mingw-w64-ucrt-x86_64-gcc"]["notices"],
                         ["notices/mingw-w64-ucrt-x86_64-gcc/ucrt64/share/licenses/gcc/COPYING"])
        self.assertEqual(record["installed"]["mingw-w64-ucrt-x86_64-gcc"]["archive"], "gcc.pkg.tar.gz")
        # Nothing in the records names the packaging machine's folders.
        for name in ("installed.json", "bundle-inventory.txt"):
            text = (dest / name).read_text(encoding="utf-8")
            self.assertNotIn(self.root.as_posix(), text)
            self.assertNotIn(str(self.root), text)
        self.assertEqual(verify_bundle(manifest, dest), bundle)

    def test_bundle_restages_from_the_verified_cache_without_network(self) -> None:
        manifest = self.bundle_manifest()
        stage_bundle(manifest, self.root / "first", self.root / "cache", allow_http=True)
        requests = _PayloadHandler.requests
        self.server.shutdown()
        bundle = stage_bundle(manifest, self.root / "second", self.root / "cache", allow_http=True)
        self.assertEqual(_PayloadHandler.requests, requests)
        self.assertEqual(verify_bundle(manifest, self.root / "second"), bundle)

    def test_bundle_verification_refuses_changed_trees(self) -> None:
        manifest = self.bundle_manifest()
        cache = self.root / "cache"

        def fresh(name: str) -> Path:
            dest = self.root / name
            stage_bundle(manifest, dest, cache, allow_http=True)
            return dest

        dest = fresh("same-size")
        gcc = dest / "msys64" / "ucrt64" / "bin" / "gcc.exe"
        gcc.write_bytes(b"tampered gcc\n\n")
        self.assertEqual(len(gcc.read_bytes()), len(b"synthetic gcc\n"))
        with self.assertRaisesRegex(PrerequisiteFetchError, "gcc.exe does not match its recorded size and SHA-256"):
            verify_bundle(manifest, dest)
        dest = fresh("missing")
        (dest / "python" / "python.exe").unlink()
        with self.assertRaisesRegex(PrerequisiteFetchError, "python/python.exe is missing"):
            verify_bundle(manifest, dest)
        dest = fresh("unlisted")
        (dest / "msys64" / "ucrt64" / "bin" / "extra.exe").write_bytes(b"x")
        with self.assertRaisesRegex(PrerequisiteFetchError, "extra.exe is not in the file inventory"):
            verify_bundle(manifest, dest)
        dest = fresh("record")
        record = json.loads((dest / "installed.json").read_text(encoding="utf-8"))
        record["installed"]["mingw-w64-ucrt-x86_64-gcc"]["version"] = "0.9-test"
        (dest / "installed.json").write_text(json.dumps(record), encoding="utf-8")
        with self.assertRaisesRegex(PrerequisiteFetchError,
                                    "mingw-w64-ucrt-x86_64-gcc differs from the pinned manifest"):
            verify_bundle(manifest, dest)
        other_manifest = self.root / "other_manifest.json"
        other_manifest.write_bytes(manifest.read_bytes() + b"\n")
        with self.assertRaisesRegex(PrerequisiteFetchError, "staged from a different manifest"):
            verify_bundle(other_manifest, fresh("manifest"))
        dest = fresh("escape")
        with (dest / "bundle-inventory.txt").open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(f"{'0' * 64} 1 msys64/../../outside.txt\n")
        with self.assertRaisesRegex(PrerequisiteFetchError, "BUNDLE_PATH_REJECTED"):
            verify_bundle(manifest, dest)

    def test_bundle_refuses_unsafe_destinations_and_incomplete_toolchains(self) -> None:
        manifest = self.bundle_manifest()
        occupied = self.root / "occupied"
        occupied.mkdir()
        (occupied / "old.txt").write_text("x", encoding="utf-8")
        with self.assertRaisesRegex(PrerequisiteFetchError, "BUNDLE_DESTINATION_NOT_EMPTY"):
            stage_bundle(manifest, occupied, self.root / "cache", allow_http=True)
        with self.assertRaisesRegex(PrerequisiteFetchError, "BUNDLE_CACHE_REJECTED"):
            stage_bundle(manifest, self.root / "dest", self.root / "dest" / "cache", allow_http=True)
        with self.assertRaisesRegex(PrerequisiteFetchError, "INSTALL_ROOT_REJECTED"):
            stage_bundle(manifest, REPOSITORY_ROOT_FOR_TESTS / "build" / "prereqs-test",
                         self.root / "cache", allow_http=True)
        self.assertFalse((REPOSITORY_ROOT_FOR_TESTS / "build" / "prereqs-test").exists())
        without_make = self.bundle_manifest(msys_members={"ucrt64/bin/gcc.exe": b"synthetic gcc\n"})
        with self.assertRaisesRegex(PrerequisiteFetchError, "BUNDLE_INCOMPLETE: .*mingw32-make.exe"):
            stage_bundle(without_make, self.root / "incomplete", self.root / "cache2", allow_http=True)

    def test_bundle_command_line_reports_json_and_refuses_a_missing_cache(self) -> None:
        manifest = self.bundle_manifest()
        dest = self.root / "cli"
        bundle = stage_bundle(manifest, dest, self.root / "cache", allow_http=True)
        out = io.StringIO()
        with patch("sys.stdout", out):
            self.assertEqual(fetcher_main(["--manifest", str(manifest), "--verify-bundle", str(dest)]), 0)
        self.assertEqual(json.loads(out.getvalue()), {"event": "bundle-verified", **bundle})
        out = io.StringIO()
        with patch("sys.stdout", out):
            self.assertEqual(fetcher_main(["--manifest", str(manifest), "--bundle-dest", str(self.root / "x")]), 2)
        self.assertEqual(json.loads(out.getvalue())["code"], "BUNDLE_CACHE_REJECTED")

    def test_offline_failure_names_network_boundary(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join(timeout=5)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            unused_port = probe.getsockname()[1]
        item = self.item()
        item["url"] = f"http://127.0.0.1:{unused_port}/payload"
        with self.assertRaisesRegex(PrerequisiteFetchError, "OFFLINE_OR_NETWORK_ERROR"):
            download_verified_item(item, self.root / "payload.zip", retries=2, allow_http=True)

    def test_font_closure_packages_are_pinned_in_the_manifest(self) -> None:
        """The readable-font DLL closure must ship on the prerequisite route (#421).

        The 17-DLL closure of SDL3.dll/SDL3_ttf.dll maps (mechanically, via
        pacman -Qo) to twelve MSYS2 UCRT64 packages; the nine that the manifest
        did not yet pin must be pinned like every other artifact: official
        repository database %SHA256SUM%, exact size, HTTPS host allow-list.
        """
        manifest_path = Path(__file__).resolve().parents[1] / "assets" / "prereq_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        artifacts = {item["id"]: item for item in manifest["artifacts"]}
        expected = {
            "mingw-w64-ucrt-x86_64-brotli",
            "mingw-w64-ucrt-x86_64-bzip2",
            "mingw-w64-ucrt-x86_64-freetype",
            "mingw-w64-ucrt-x86_64-glib2",
            "mingw-w64-ucrt-x86_64-graphite2",
            "mingw-w64-ucrt-x86_64-harfbuzz",
            "mingw-w64-ucrt-x86_64-libpng",
            "mingw-w64-ucrt-x86_64-pcre2",
            "mingw-w64-ucrt-x86_64-sdl3-ttf",
        }
        self.assertTrue(
            expected <= set(artifacts),
            f"missing pinned font-closure packages: {sorted(expected - set(artifacts))}",
        )
        for package in sorted(expected):
            with self.subTest(package=package):
                item = artifacts[package]
                self.assertEqual(item["allowed_hosts"], ["repo.msys2.org"])
                self.assertTrue(item["url"].startswith("https://repo.msys2.org/mingw/ucrt64/"))
                self.assertEqual(item["filename"], f"{package}-{item['version']}-any.pkg.tar.zst")
                self.assertTrue(item["url"].endswith(item["filename"]))
                digest = item["sha256"]
                self.assertEqual(len(digest), 64)
                self.assertTrue(all(ch in "0123456789abcdef" for ch in digest))
                self.assertIsInstance(item["size_bytes"], int)
                self.assertGreater(item["size_bytes"], 0)
                self.assertEqual(item["archive_format"], "tar.zst")
                self.assertEqual(item["install_subdir"], "msys64")
                self.assertEqual(item["status"], "ready")
                self.assertTrue(item["license"])
                self.assertTrue(item["license_metadata"])
                source = item["sha256_source"]
                self.assertEqual(source["url"],
                                 "https://repo.msys2.org/mingw/ucrt64/ucrt64.db")
                self.assertEqual(source["signature_url"],
                                 "https://repo.msys2.org/mingw/ucrt64/ucrt64.db.sig")
                self.assertIn("%SHA256SUM%", source["method"])
        declared_total = manifest["total_download_bytes"]
        self.assertEqual(
            declared_total,
            sum(item["size_bytes"] for item in manifest["artifacts"]),
            "total_download_bytes must match the pinned artifact sizes",
        )

    def test_brotli_pin_includes_the_cve_2026_111497_fix(self) -> None:
        """The shipped Brotli decoder must be 1.2.1 or later (CVE-2026-111497).

        Brotli 1.0.3 to 1.2.0 can write out of bounds while decoding an
        attacker-controlled stream; the player stages libbrotlidec.dll from
        this pin (SDL3_ttf -> FreeType, WOFF2). A re-pin after the manifest's
        source snapshot records the date its database entry was read.
        """
        manifest_path = Path(__file__).resolve().parents[1] / "assets" / "prereq_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        item = next(item for item in manifest["artifacts"]
                    if item["id"] == "mingw-w64-ucrt-x86_64-brotli")
        upstream, _, pkgrel = item["version"].partition("-")
        self.assertTrue(pkgrel.isdigit(), item["version"])
        self.assertGreaterEqual(tuple(int(part) for part in upstream.split(".")), (1, 2, 1))
        retrieved = item["sha256_source"].get("retrieved_utc", manifest["source_snapshot_date"])
        self.assertRegex(retrieved, r"^\d{4}-\d{2}-\d{2}$")
        self.assertGreaterEqual(retrieved, manifest["source_snapshot_date"])

    def test_synthetic_installed_toolchain_resolves_the_font_closure(self) -> None:
        """The installer's extracted layout must resolve the closure (#421).

        The pinned prerequisites extract to
        <data>/prerequisites/msys64/ucrt64 with bin/ and share/licenses/;
        stage_runtime_dlls must find the DLL closure there without any MSYS2 on
        PATH. Fake DLLs and a fake import resolver keep this offline.
        """
        import package_notices
        import stage_runtime_dlls

        with patch.dict(os.environ, {
            "LOCALAPPDATA": str(self.root),
            "XDG_DATA_HOME": str(self.root),
        }):
            data_root = default_data_root()
        toolchain_root = data_root / "prerequisites" / "msys64" / "ucrt64"
        bin_dir = toolchain_root / "bin"
        (toolchain_root / "share" / "licenses" / "mingw-w64-ucrt-x86_64-sdl3-ttf").mkdir(parents=True)
        bin_dir.mkdir(parents=True)
        (bin_dir / "gcc.exe").write_bytes(b"source-owned synthetic test payload\n")
        closure = [
            "SDL3.dll", "SDL3_ttf.dll", "libbrotlicommon.dll", "libbrotlidec.dll",
            "libbz2-1.dll", "libfreetype-6.dll", "libgcc_s_seh-1.dll",
            "libglib-2.0-0.dll", "libgraphite2.dll", "libharfbuzz-0.dll",
            "libiconv-2.dll", "libintl-8.dll", "libpcre2-8-0.dll",
            "libpng16-16.dll", "libstdc++-6.dll", "libwinpthread-1.dll", "zlib1.dll",
        ]
        for name in closure:
            (bin_dir / name).write_bytes(b"source-owned synthetic test payload\n")

        def fake_imports(path: Path) -> list[str]:
            if path.name == "SDL3_ttf.dll":
                return [name for name in closure if name != "SDL3_ttf.dll"]
            return []

        with patch.dict(os.environ, {
            "LOCALAPPDATA": str(self.root),
            "XDG_DATA_HOME": str(self.root),
            "MINGW_PREFIX": "",
        }), patch.object(package_notices.shutil, "which", return_value=None):
            resolved_root = package_notices.resolve_toolchain_root()
            self.assertEqual(resolved_root, toolchain_root.resolve())
            search_dirs = stage_runtime_dlls.default_search_dirs()
            self.assertIn(bin_dir.resolve(), [d.resolve() for d in search_dirs])
            root_dll = stage_runtime_dlls.resolve_root_dll("SDL3_ttf.dll", "", search_dirs)
            resolved = stage_runtime_dlls.resolve_dll_closure(
                root_dll, search_dirs=search_dirs, imports_of=fake_imports
            )
        self.assertEqual(
            {path.name.lower() for path in resolved},
            {name.lower() for name in closure},
        )

    def test_manifest_has_no_firmware_or_font_download_hook(self) -> None:
        # The player never fetches PSP system software, firmware or system fonts:
        # those come only from the user's own device. The prerequisite manifest
        # therefore carries no font-source hook and names no firmware download.
        manifest_path = Path(__file__).resolve().parents[1] / "assets" / "prereq_manifest.json"
        text = manifest_path.read_text(encoding="utf-8")
        manifest = json.loads(text)
        self.assertNotIn("font_source_hooks", manifest)
        self.assertNotIn("system-software-update", text.lower())
        self.assertNotIn("firmware", text.lower())


REPO_ROOT = Path(__file__).resolve().parent.parent

# Offline by default (AGENTS.md section 5): the shipped player, runtime and
# their Python helpers reach the network only through these files, both of
# them the consented, pinned prerequisite download. prereq_fetcher.py also
# serves the maintainer's release staging (--bundle-dest, #1285) from the same
# pinned hosts; the player never passes that option, and a release that carries
# prereqs/ makes no download at all. A new egress needs a maintainer decision,
# a "Network use" entry in docs/SETUP.md, and a line here.
NETWORK_EGRESS_ALLOWLIST = frozenset({
    "src/player/package_builder.c",
    "tools/nk_core/prereq_fetcher.py",
})

# Shipped code: everything under src/ and the Python the player runs. Maintainer
# tools elsewhere in tools/ (SBOM and issue-link audits) never ship and are not
# covered.
NETWORK_EGRESS_SCOPES = (("src", (".c", ".h", ".cpp", ".hpp")), ("tools/nk_core", (".py",)))

NETWORK_EGRESS_PATTERN = re.compile(
    r"\bWinHttp\w*|\bwinhttp\.h\b|\bInternet(?:Open|Connect|ReadFile)\w*|\bwininet\b"
    r"|\bURLDownloadToFile\w*|\bWSAStartup\b|\bwinsock2?\.h\b|<sys/socket\.h>"
    r"|\bgetaddrinfo\b|\bSDL_net\b|\bcurl_easy_\w+|\bsocket\s*\("
    r"|\burllib\.request\b|\bfrom\s+urllib\s+import\s+request\b|\burlopen\b"
    r"|\bhttp\.client\b|^\s*(?:import|from)\s+(?:requests|socket|httpx|aiohttp)\b",
    re.MULTILINE,
)


class TestNetworkEgressAllowlist(unittest.TestCase):
    def test_only_the_prerequisite_downloaders_use_network_apis(self) -> None:
        offenders = []
        for scope, suffixes in NETWORK_EGRESS_SCOPES:
            for path in sorted((REPO_ROOT / scope).rglob("*")):
                if not path.is_file() or path.suffix not in suffixes:
                    continue
                rel = path.relative_to(REPO_ROOT).as_posix()
                if rel in NETWORK_EGRESS_ALLOWLIST:
                    continue
                match = NETWORK_EGRESS_PATTERN.search(path.read_text(encoding="utf-8", errors="replace"))
                if match:
                    offenders.append(f"{rel}: {match.group(0).strip()}")
        self.assertEqual(offenders, [], "network API outside the egress allowlist; see AGENTS.md section 5")

    def test_allowlisted_downloaders_still_exist(self) -> None:
        for rel in sorted(NETWORK_EGRESS_ALLOWLIST):
            self.assertTrue((REPO_ROOT / rel).is_file(), f"stale allowlist entry: {rel}")

    def test_pattern_catches_each_api_family(self) -> None:
        for sample in ("WinHttpOpen(", "#include <winsock2.h>", "#include <sys/socket.h>",
                       "getaddrinfo(", "socket (AF_INET", "import urllib.request",
                       "from urllib import request", "import requests", "import socket",
                       "http.client.HTTPSConnection", "InternetOpenW("):
            self.assertIsNotNone(NETWORK_EGRESS_PATTERN.search(sample), sample)
        for sample in ("from urllib.parse import quote", "sceNetAdhocInit", "s_socket_count"):
            self.assertIsNone(NETWORK_EGRESS_PATTERN.search(sample), sample)


if __name__ == "__main__":
    unittest.main()
