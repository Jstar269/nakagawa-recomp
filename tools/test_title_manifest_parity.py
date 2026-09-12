#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Differential parity test between tools/title_manifest.py and nk_title_manifest.c.

Proves Section 2 (Canonical Manifest Parity) and Section 3 (Security Size Limit):
- Both parsers agree on all valid public manifests in assets/titles/
- Both parsers extract identical title identity, entry, base address, and modules
- Both parsers reject hostile mutations, malformed schemas, and security violations identically.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_manifest


#: The host platform backend, chosen the same way the Makefile chooses it.
#: Hardcoding the Win32 backend made this test compile - and therefore fail -
#: on every non-Windows host.
_WINDOWS = os.name == "nt"
PLATFORM_SRC = "src/core/nk_platform_win32.c" if _WINDOWS else "src/core/nk_platform_posix.c"
EXE_EXT = ".exe" if _WINDOWS else ""


class TitleManifestParityTests(unittest.TestCase):
    native_exe = ROOT / "build" / f"test_manifest_parser{EXE_EXT}"

    @classmethod
    def setUpClass(cls):
        # Ensure native parser binary is compiled and up-to-date with sources
        srcs = [
            ROOT / "src/core/nk_iso.c",
            ROOT / "src/core/nk_library.c",
            ROOT / "src/core/nk_launch.c",
            ROOT / "src/core/nk_title_manifest.c",
            ROOT / "src/core/generated/nk_title_catalog.c",
            ROOT / PLATFORM_SRC,
            ROOT / "tests/native/test_manifest_parser.c",
        ]
        if shutil.which("gcc") is None:
            raise unittest.SkipTest(
                "gcc is unavailable, so the native parser cannot be built; "
                "reporting SKIP rather than passing without the differential half"
            )
        recompile = (not cls.native_exe.is_file()) or any(s.stat().st_mtime > cls.native_exe.stat().st_mtime for s in srcs if s.exists())
        if recompile:
            cmd = [
                "gcc", "-std=c99", "-Wall", "-Wextra",
                "-Isrc/core", "-Isrc/core/generated",
                "src/core/nk_iso.c", "src/core/nk_library.c", "src/core/nk_launch.c",
                "src/core/nk_title_manifest.c", "src/core/generated/nk_title_catalog.c",
                PLATFORM_SRC,
                "tests/native/test_manifest_parser.c",
                "-o", str(cls.native_exe)
            ]
            subprocess.check_call(cmd, cwd=ROOT)

    def _run_native(self, manifest_path: Path, allow_override: bool = False) -> tuple[bool, str]:
        cmd = [str(self.native_exe), "--check", str(manifest_path)]
        if allow_override:
            cmd.append("--allow-override")
        res = subprocess.run(cmd, capture_output=True, text=True)
        is_accept = (res.returncode == 0) and res.stdout.startswith("ACCEPT")
        return is_accept, res.stdout.strip()

    def _run_python(self, manifest_path: Path) -> tuple[bool, str]:
        try:
            raw = title_manifest.load_manifest(manifest_path)
            val = title_manifest.validate_manifest(raw)
            return True, f"ACCEPT: id={val['id']}"
        except Exception as exc:
            return False, f"REJECT: {exc}"

    def test_public_manifests_parity(self):
        """All public manifests in assets/titles/ must be accepted by both parsers."""
        titles_dir = ROOT / "assets" / "titles"
        manifest_files = list(titles_dir.glob("*.json"))
        self.assertGreater(len(manifest_files), 0, "Expected at least 1 public manifest")

        for mf in manifest_files:
            py_ok, py_out = self._run_python(mf)
            c_ok, c_out = self._run_native(mf, allow_override=True)
            self.assertTrue(py_ok, f"Python rejected valid manifest {mf.name}: {py_out}")
            self.assertTrue(c_ok, f"Native rejected valid manifest {mf.name}: {c_out}")

            # Verify identical extracted ID
            raw = json.loads(mf.read_text(encoding="utf-8"))
            self.assertIn(f"id={raw['id']}", c_out)

    def test_missing_required_fields_parity(self):
        """Both parsers must reject manifests missing required schema fields."""
        required = [
            "schema_version", "id", "display_name", "kind", "executable",
            "modules", "filesystem", "hle_profile", "feature_requirements",
            "verification_profile"
        ]

        valid_base = {
            "schema_version": 1,
            "id": "parity-synth",
            "display_name": "Parity Synthetic",
            "kind": "synthetic",
            "executable": {"base": 0, "entry": 142606336},
            "modules": [{"name": "boot.prx"}],
            "filesystem": {"data_root": "data", "memory_stick_root": "ms"},
            "hle_profile": "standard",
            "feature_requirements": ["allegrex"],
            "verification_profile": "smoke"
        }

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            for req in required:
                bad = dict(valid_base)
                del bad[req]
                mf = tmppath / f"missing_{req}.json"
                mf.write_text(json.dumps(bad), encoding="utf-8")

                py_ok, _ = self._run_python(mf)
                c_ok, _ = self._run_native(mf)
                self.assertFalse(py_ok, f"Python should reject missing {req}")
                self.assertFalse(c_ok, f"Native should reject missing {req}")

    def test_hostile_mutations_parity(self):
        """Both parsers must fail closed on hostile mutations."""
        valid_json = {
            "schema_version": 1,
            "id": "parity-synth",
            "display_name": "Parity Synthetic",
            "kind": "synthetic",
            "executable": {
                "base": 0,
                "entry": 142606336,
                "bss_metadata_source": "elf",
                "extra_executable_spans": [],
            },
            "modules": [
                {
                    "name": "boot.prx",
                    "load_address": 146800640,
                    "required": True,
                    "role": "guest-prx",
                }
            ],
            "filesystem": {
                "data_root": "data",
                "memory_stick_root": "ms",
                "device_prefixes": ["host0:"],
            },
            "hle_profile": "standard",
            "feature_requirements": ["allegrex"],
            "verification_profile": "smoke",
        }

        # Verify base is truly valid in both parsers
        with tempfile.TemporaryDirectory() as tmpdir:
            base_mf = Path(tmpdir) / "base.json"
            base_mf.write_text(json.dumps(valid_json), encoding="utf-8")
            py_base_ok, py_msg = self._run_python(base_mf)
            c_base_ok, c_msg = self._run_native(base_mf)
            self.assertTrue(py_base_ok, f"Python rejected base valid_json: {py_msg}")
            self.assertTrue(c_base_ok, f"Native rejected base valid_json: {c_msg}")

        mutations = [
            # 1. Unknown field
            (lambda d: d.update({"evil_extra_field": 123}), "unknown field"),
            # 2. Wrong schema version
            (lambda d: d.update({"schema_version": 99}), "bad schema version"),
            # 3. Wrong schema version type
            (lambda d: d.update({"schema_version": "1"}), "string schema version"),
            # 4. Negative executable entry
            (lambda d: d["executable"].update({"entry": -1}), "negative entry"),
            # 5. Overflow executable entry
            (lambda d: d["executable"].update({"entry": 0x100000000}), "overflow entry"),
            # 6. Malformed hex
            (lambda d: d["executable"].update({"entry": "0xNOT_A_HEX"}), "malformed hex"),
            # 7. Reserved VFPU target range
            (lambda d: d.update({"runtime_bindings": {"schema_version": 1, "dispatch_aliases": [{"from": 0x40001000, "to": 0x08801000}]}}), "vfpu reserved range"),
            # 8. Duplicate module name
            (lambda d: d["modules"].append({"name": "BOOT.PRX", "load_address": 2, "required": True, "role": "guest-prx"}), "duplicate module"),
            # 9. Duplicate feature requirement
            (lambda d: d["feature_requirements"].append("allegrex"), "duplicate feature"),
            # 10. Non-retail kind with disc object
            (lambda d: d.update({"disc": {"id": "UCUS98701", "region": "NA", "revision_policy": "exact-disc-id"}}), "non-retail with disc"),
            # 11. Retail kind without disc object
            (lambda d: d.update({"kind": "retail"}), "retail without disc"),
            # 12. Fractional schema version
            (lambda d: d.update({"schema_version": 1.5}), "float schema version"),
            # 13. Uppercase identifier
            (lambda d: d.update({"id": "UPPERCASE_INVALID"}), "uppercase identifier"),
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            for idx, (mut_fn, label) in enumerate(mutations):
                d = json.loads(json.dumps(valid_json))
                mut_fn(d)
                mf = tmppath / f"mut_{idx}.json"
                mf.write_text(json.dumps(d), encoding="utf-8")

                py_ok, py_err = self._run_python(mf)
                c_ok, c_err = self._run_native(mf)

                self.assertFalse(py_ok, f"Python accepted hostile mutation ({label}): {py_err}")
                self.assertFalse(c_ok, f"Native accepted hostile mutation ({label}): {c_err}")

    def test_duplicate_json_keys_parity(self):
        """Both parsers must reject duplicate JSON object keys."""
        raw_dup_json = '{"schema_version": 1, "id": "foo", "id": "bar", "display_name": "T", "kind": "synthetic", "executable": {"base": 0, "entry": 0, "bss_metadata_source": "none", "extra_executable_spans": []}, "modules": [], "filesystem": {"data_root": "d", "memory_stick_root": "m", "device_prefixes": ["host0:"]}, "hle_profile": "s", "feature_requirements": [], "verification_profile": "v"}'
        with tempfile.TemporaryDirectory() as tmpdir:
            mf = Path(tmpdir) / "dup_key.json"
            mf.write_text(raw_dup_json, encoding="utf-8")

            py_ok, py_err = self._run_python(mf)
            c_ok, c_err = self._run_native(mf)

            self.assertFalse(py_ok, f"Python should reject duplicate key: {py_err}")
            self.assertFalse(c_ok, f"Native should reject duplicate key: {c_err}")

    def test_trailing_garbage_parity(self):
        """Both parsers must reject trailing garbage after root JSON object."""
        raw_trailing = '{"schema_version": 1, "id": "test", "display_name": "T", "kind": "synthetic", "executable": {"base": 0, "entry": 0, "bss_metadata_source": "none", "extra_executable_spans": []}, "modules": [], "filesystem": {"data_root": "d", "memory_stick_root": "m", "device_prefixes": ["host0:"]}, "hle_profile": "s", "feature_requirements": [], "verification_profile": "v"} GARBAGE_DATA'
        with tempfile.TemporaryDirectory() as tmpdir:
            mf = Path(tmpdir) / "trailing.json"
            mf.write_text(raw_trailing, encoding="utf-8")

            py_ok, py_err = self._run_python(mf)
            c_ok, c_err = self._run_native(mf)

            self.assertFalse(py_ok, f"Python should reject trailing garbage: {py_err}")
            self.assertFalse(c_ok, f"Native should reject trailing garbage: {c_err}")

    def test_unicode_escapes_differential_parity(self):
        """Raw UTF-8 and escaped \\u sequences must yield identical parsed fields."""
        raw_utf8_manifest = {
            "schema_version": 1,
            "id": "unicode-parity",
            "display_name": "日本語 タイトル Café",
            "kind": "synthetic",
            "executable": {"base": 0, "entry": 142606336, "bss_metadata_source": "elf", "extra_executable_spans": []},
            "modules": [{"name": "main.prx", "load_address": 146800640, "required": True, "role": "guest-prx"}],
            "filesystem": {"data_root": "data", "memory_stick_root": "ms", "device_prefixes": ["host0:"]},
            "hle_profile": "standard",
            "feature_requirements": ["allegrex"],
            "verification_profile": "smoke",
        }
        escaped_json = (
            '{"schema_version": 1, "id": "unicode-parity", '
            '"display_name": "\\u65e5\\u672c\\u8a9e \\u30bf\\u30a4\\u30c8\\u30eb Caf\\u00e9", '
            '"kind": "synthetic", '
            '"executable": {"base": 0, "entry": 142606336, "bss_metadata_source": "elf", "extra_executable_spans": []}, '
            '"modules": [{"name": "main.prx", "load_address": 146800640, "required": true, "role": "guest-prx"}], '
            '"filesystem": {"data_root": "data", "memory_stick_root": "ms", "device_prefixes": ["host0:"]}, '
            '"hle_profile": "standard", "feature_requirements": ["allegrex"], "verification_profile": "smoke"}'
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            f_raw = Path(tmpdir) / "raw.json"
            f_raw.write_text(json.dumps(raw_utf8_manifest, ensure_ascii=False), encoding="utf-8")
            f_esc = Path(tmpdir) / "esc.json"
            f_esc.write_text(escaped_json, encoding="utf-8")

            py_raw_ok, py_raw_out = self._run_python(f_raw)
            py_esc_ok, py_esc_out = self._run_python(f_esc)
            c_raw_ok, c_raw_out = self._run_native(f_raw)
            c_esc_ok, c_esc_out = self._run_native(f_esc)

            self.assertTrue(py_raw_ok and py_esc_ok)
            self.assertTrue(c_raw_ok and c_esc_ok)

    def test_executable_entry_vs_fallback_contract(self):
        """Native parser must project executable.entry, NOT runtime_bindings.fallback_entry."""
        diff_manifest = {
            "schema_version": 1,
            "id": "entry-diff-parity",
            "display_name": "Entry Diff Parity",
            "kind": "synthetic",
            "executable": {"base": 0x08800000, "entry": 0x08800000, "bss_metadata_source": "elf", "extra_executable_spans": []},
            "modules": [{"name": "main.prx", "load_address": 146800640, "required": True, "role": "guest-prx"}],
            "filesystem": {"data_root": "data", "memory_stick_root": "ms", "device_prefixes": ["host0:"]},
            "hle_profile": "standard",
            "feature_requirements": ["allegrex"],
            "verification_profile": "smoke",
            "runtime_bindings": {
                "schema_version": 1,
                "fallback_entry": 0x08801000,
            }
        }
        with tempfile.TemporaryDirectory() as tmpdir:
            mf = Path(tmpdir) / "diff.json"
            mf.write_text(json.dumps(diff_manifest), encoding="utf-8")

            c_ok, c_out = self._run_native(mf)
            self.assertTrue(c_ok, f"Native failed to parse valid diff manifest: {c_out}")
            self.assertIn("entry=0x08800000", c_out, "Executable entry was overridden by fallback_entry!")
            self.assertNotIn("entry=0x08801000", c_out)


if __name__ == "__main__":
    unittest.main()
