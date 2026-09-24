# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for nk_cli.py build-package --progress-json and --log-file options."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parent.parent
CLI_PATH = ROOT / "tools" / "nk_cli.py"


class NkCliProgressTests(unittest.TestCase):
    def test_progress_json_invalid_disc_id(self) -> None:
        """Verify --progress-json reports failure for an invalid disc ID format."""
        cmd = [
            sys.executable,
            str(CLI_PATH),
            "build-package",
            "INVALID_DISC",
            "--progress-json",
        ]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        self.assertGreaterEqual(len(lines), 1)
        events = [json.loads(line) for line in lines]
        self.assertTrue(all("stage" in ev and "status" in ev and "message" in ev for ev in events))
        self.assertEqual(events[-1]["status"], "FAIL")
        self.assertEqual(events[-1]["stage"], "preflight")

    def test_progress_json_nonexistent_disc_id(self) -> None:
        """Verify --progress-json reports failure when the library entry does not exist."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
                "--progress-json",
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            self.assertEqual(events[0]["status"], "START")
            self.assertEqual(events[0]["stage"], "preflight")
            self.assertEqual(events[-1]["status"], "FAIL")
            self.assertIn("stage", events[-1])

    def test_progress_json_to_file(self) -> None:
        """Verify --progress-json <file> writes JSON lines to a file."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            progress_file = user_root / "progress.jsonl"
            log_file = user_root / "build.log"
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
                "--progress-json",
                str(progress_file),
                "--log-file",
                str(log_file),
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(progress_file.is_file(), "Progress file was not created")
            lines = [line.strip() for line in progress_file.read_text(encoding="utf-8").splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            self.assertEqual(events[-1]["status"], "FAIL")
            self.assertTrue(log_file.is_file(), "Log file was not created")
            log_content = log_file.read_text(encoding="utf-8")
            self.assertIn("preflight", log_content)

    def test_human_output_unchanged_when_no_progress_flag(self) -> None:
        """Verify human output is printed when --progress-json is omitted."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            # Output should not contain JSON objects
            for line in proc.stdout.splitlines():
                with self.assertRaises(json.JSONDecodeError):
                    json.loads(line)

    def test_progress_json_reports_boundary_on_encrypted_iso(self) -> None:
        """Verify --progress-json reports the fail-closed boundary and issue #295 for encrypted ISO."""
        from test_iso_parity import build_psp_container, create_test_iso_with_executables

        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_boundary_") as tmpdir:
            user_root = Path(tmpdir)
            iso_path = user_root / "encrypted.iso"
            create_test_iso_with_executables(
                iso_path, build_psp_container(), disc_id="ULUS99998"
            )
            library = {
                "schema_version": 1,
                "games": [{
                    "disc_id": "ULUS99998",
                    "title_id": "experimental-ulus99998",
                    "iso_path": str(iso_path),
                    "selected_executable": "EBOOT.BIN",
                    "is_experimental": True,
                }],
            }
            (user_root / "library.json").write_text(json.dumps(library), encoding="utf-8")
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "ULUS99998",
                "--user-data-root",
                str(user_root),
                "--progress-json",
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            fail_event = events[-1]
            self.assertEqual(fail_event["status"], "FAIL")
            self.assertIn("in the works", fail_event["message"])
            self.assertIn("#295", fail_event["message"])


if __name__ == "__main__":
    unittest.main()
