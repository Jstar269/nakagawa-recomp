#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import hst_doctor  # noqa: E402
from hst_test_fixtures import write_elf, write_iso, write_psp_header  # noqa: E402


def write_pe(path: Path, *, pe_offset: int = 0x80) -> None:
    data = bytearray(max(0x100, pe_offset + 6))
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset : pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe_offset + 4, 0x8664)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class FormatHardeningTests(unittest.TestCase):
    def test_rejects_pe_header_offset_outside_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.exe"
            data = bytearray(0x80)
            data[:2] = b"MZ"
            struct.pack_into("<I", data, 0x3C, 0x10000000)
            path.write_bytes(data)
            ok, error = hst_doctor._validate_pe_x64(path)
            self.assertFalse(ok)
            self.assertIn("outside", error)

    def test_accepts_synthetic_x64_pe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "good.exe"
            write_pe(path)
            ok, detail = hst_doctor._validate_pe_x64(path)
            self.assertTrue(ok, detail)

    def test_rejects_non_primary_iso_descriptor(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.iso"
            write_iso(path, descriptor_type=2)
            metadata, error = hst_doctor._validate_iso(path)
            self.assertIsNone(metadata)
            self.assertIn("expected primary", error or "")


class InputPairHardeningTests(unittest.TestCase):
    def test_mismatched_eboot_segment_counts_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_elf(root / "place_game_here" / "EBOOT.elf", load_segments=1)
            write_psp_header(
                root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN",
                segments=2,
            )
            decrypted = root / "place_game_here" / "EXTRACTED" / "decrypted"
            for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
                write_elf(decrypted / name)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor.check_private_inputs(report, need_iso=False, need_assets=False)
            result = [item for item in report.results if item.code == "INPUT_EBOOT_PAIR"][-1]
            self.assertEqual(result.status, "FAIL")


class StrictModeHardeningTests(unittest.TestCase):
    def test_strict_warning_exit_is_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs").mkdir()
            (root / "interface").mkdir()
            (root / "assets").mkdir()
            (root / "LICENSE").write_text(
                "GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n",
                encoding="utf-8",
            )
            notice = """GPL-3.0-or-later
This repository does not grant rights to the game.
The project ships no decryption keys of any kind.
This project is independent and is not endorsed.
Users must supply their own legally obtained inputs.
This remains subject to legal review.
"""
            for name, content in {
                "NOTICE.md": notice,
                "README.md": "GPL-3.0-or-later\n",
                "CONTRIBUTING.md": "GPL-3.0-or-later\n",
                "SECURITY.md": "security\n",
                "CODE_OF_CONDUCT.md": "conduct\n",
            }.items():
                (root / name).write_text(content, encoding="utf-8")
            (root / "docs" / "PUBLICATION_READINESS.md").write_text("publication\n", encoding="utf-8")
            (root / "interface" / "package.json").write_text(
                json.dumps({"license": "GPL-2.0-or-later"}),
                encoding="utf-8",
            )
            (root / "assets" / "release_manifest.json").write_text(
                json.dumps({"license": "GPL-3.0-or-later"}),
                encoding="utf-8",
            )
            proc = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "hst_doctor.py"),
                    "--root",
                    str(root),
                    "--scope",
                    "repo",
                    "--json",
                    "--strict",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["exit_code"], 2)


class ManagerExitPropagationHardeningTests(unittest.TestCase):
    def test_parameterized_manager_actions_fail_closed(self) -> None:
        manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8-sig")
        # Each failing action records a nonzero termination code and breaks out of the
        # switch; the single `exit` after the finally block applies it, so the caller
        # sees a nonzero status AND the caller's location is restored first.
        self.assertIn('"BuildFull" { if (-not (Invoke-HstBuild -Mode "Full")) { $script:ManagerExitCode = 1; break } }', manager)
        self.assertIn('"BuildFast" { if (-not (Invoke-HstBuild -Mode "Fast")) { $script:ManagerExitCode = 1; break } }', manager)
        self.assertIn('if (-not (Invoke-Selftest)) { $script:ManagerExitCode = 1; break }', manager)
        self.assertIn('$script:LastRunResult = $null', manager)
        self.assertIn('if ($null -eq $script:LastRunResult) { $script:ManagerExitCode = 1; break }', manager)
        self.assertIn('if ($Action -and $script:ManagerExitCode -ne 0) {\n    exit $script:ManagerExitCode', manager)

    def test_frontend_does_not_mask_manager_failure(self) -> None:
        frontend = (ROOT / "hst.ps1").read_text(encoding="utf-8-sig")
        self.assertNotIn("Invoke-ManagerBuild", frontend)
        self.assertNotIn("Get-HstProductBackupPath", frontend)
        self.assertIn("$LASTEXITCODE = 0", frontend)
        self.assertIn("$exitCode = [int]$LASTEXITCODE", frontend)
        self.assertIn('DoctorScope "products"', frontend)


if __name__ == "__main__":
    unittest.main()
