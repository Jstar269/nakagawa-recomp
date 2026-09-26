# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""No-network tests for the pinned prerequisite download and install path."""

from __future__ import annotations

import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))

from nk_core.prereq_fetcher import (
    PrerequisiteFetchError,
    download_verified_item,
    install_items,
)


def _zip_payload() -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("python314/LICENSE.txt", "Python license text\n")
        archive.writestr("python314/python.exe", b"synthetic fixture\n")
    return stream.getvalue()


class _PayloadHandler(BaseHTTPRequestHandler):
    payload = b""
    mode = "ok"
    requests = 0

    def do_GET(self) -> None:
        type(self).requests += 1
        if self.path == "/foreign-redirect":
            self.send_response(302)
            self.send_header("Location", f"http://localhost:{self.server.server_port}/payload")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(type(self).payload)))
        self.end_headers()
        if type(self).mode == "interrupt" and type(self).requests == 1:
            self.wfile.write(type(self).payload[: max(1, len(type(self).payload) // 2)])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(type(self).payload)

    def log_message(self, _format: str, *_args: object) -> None:
        return


class TestPrerequisiteFetcher(unittest.TestCase):
    def setUp(self) -> None:
        _PayloadHandler.payload = _zip_payload()
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
            "version": "3.14-test",
            "url": f"http://127.0.0.1:{self.server.server_port}{path}",
            "allowed_hosts": ["127.0.0.1"],
            "filename": "payload.zip",
            "size_bytes": len(body),
            "sha256": hashlib.sha256(body).hexdigest(),
            "license": "PSF-2.0",
            "archive_format": "zip",
            "install_subdir": "python",
        }

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
        item["filename"] = "python-3.14-embed-amd64.zip"
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

    def test_sony_font_hook_is_fail_closed_and_never_downloaded_by_tests(self) -> None:
        manifest_path = Path(__file__).resolve().parents[1] / "assets" / "prereq_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        hooks = manifest["font_source_hooks"]
        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks[0]["type"], "sony-psp-system-software-update")
        self.assertIsNone(hooks[0]["url"])
        self.assertIsNone(hooks[0]["sha256"])
        self.assertFalse(hooks[0]["download_in_tests"])
        self.assertIn("decryption and your key file (#295)", hooks[0]["failure_message"])


if __name__ == "__main__":
    unittest.main()
