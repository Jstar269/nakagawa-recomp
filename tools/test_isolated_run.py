# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for tools/isolated_run.py, on a synthetic Memory Stick tree and stub commands."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import isolated_run  # noqa: E402

# Writes argv[2] under the directory named by the environment variable argv[1].
STUB = (
    "import os, sys, pathlib\n"
    "t = pathlib.Path(os.environ[sys.argv[1]]) / sys.argv[2]\n"
    "t.parent.mkdir(parents=True, exist_ok=True)\n"
    "t.write_text('x')\n"
)


class IsolatedRunTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        self.ms = base / "ms"
        (self.ms / "PSP" / "SAVEDATA").mkdir(parents=True)
        (self.ms / "PSP" / "SAVEDATA" / "A.BIN").write_bytes(b"save")
        self.settings = base / "settings.ini"
        self.settings.write_text("a=1")
        self.work = base / "work"
        self.work.mkdir()

    def _main(self, *tail: str, settings: bool = False) -> tuple[int, str]:
        argv = ["--memstick", str(self.ms), "--work-dir", str(self.work)]
        if settings:
            argv += ["--settings", str(self.settings), "--settings-env", "SR_TEST_SETTINGS"]
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = isolated_run.main([*argv, "--", sys.executable, *tail])
        return code, out.getvalue()

    def test_write_to_copy_passes(self) -> None:
        code, out = self._main("-c", STUB, "SR_MEMSTICK", "PSP/SAVEDATA/NEW.BIN")
        self.assertEqual(code, 0, out)
        self.assertIn("ORIGINALS_UNCHANGED", out)
        self.assertFalse((self.ms / "PSP" / "SAVEDATA" / "NEW.BIN").exists())
        self.assertEqual(
            len(list(self.work.glob("isolated-*/memstick/PSP/SAVEDATA/NEW.BIN"))), 1)

    def test_settings_write_to_copy_passes(self) -> None:
        # The variable names the copied settings file, so write beside it, not under it.
        stub = ("import os, sys, pathlib\n"
                "pathlib.Path(os.environ[sys.argv[1]]).with_name(sys.argv[2]).write_text('x')\n")
        code, out = self._main("-c", stub, "SR_TEST_SETTINGS", "extra.ini", settings=True)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.settings.read_text(), "a=1")

    def test_write_to_original_fails_and_names_file(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_bytes(b'bad')" % str(
            self.ms / "PSP" / "SAVEDATA" / "A.BIN")
        code, out = self._main("-c", stub)
        self.assertEqual(code, 3)
        self.assertIn("ORIGINAL_CHANGED", out)
        self.assertIn("PSP/SAVEDATA/A.BIN", out)

    def test_created_file_in_original_is_named(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_text('x')" % str(self.ms / "NEW.TXT")
        code, out = self._main("-c", stub)
        self.assertEqual(code, 3)
        self.assertIn("NEW.TXT", out)

    def test_changed_settings_original_fails(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_text('b=2')" % str(self.settings)
        code, out = self._main("-c", stub, settings=True)
        self.assertEqual(code, 3)
        self.assertIn("settings0", out)

    def test_work_dir_inside_original_refused(self) -> None:
        self.work = self.ms / "work"
        self.work.mkdir()
        code, _ = self._main("-c", "pass")
        self.assertEqual(code, 2)

    def test_command_status_passes_through(self) -> None:
        code, _ = self._main("-c", "raise SystemExit(7)")
        self.assertEqual(code, 7)


if __name__ == "__main__":
    unittest.main()
