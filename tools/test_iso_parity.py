# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Differential parity tests comparing native C core (nk_iso, nk_library, nk_launch) against Python."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from nk_core.iso_inspect import inspect_iso
from nk_core.title_registry import get_default_registry


def build_param_sfo(disc_id: str, title: str, version: str = "1.00") -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with given keys."""
    entries = [
        ("DISC_ID", 0x0204, disc_id.encode("utf-8") + b"\0"),
        ("DISC_VERSION", 0x0204, version.encode("utf-8") + b"\0"),
        ("TITLE", 0x0204, title.encode("utf-8") + b"\0"),
    ]
    entries.sort(key=lambda e: e[0])

    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def build_custom_param_sfo(entries: list[tuple[str, int, bytes]]) -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with arbitrary raw entries (e.g. duplicates)."""
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def create_test_iso(
    path: Path,
    disc_id: str = "TEST00001",
    title: str = "Test Game",
    version: str = "1.00",
    volume_id: str = "TEST_VOL",
) -> None:
    sector_size = 2048
    num_sectors = 1024  # 2 MiB image
    data = bytearray(num_sectors * sector_size)

    # Sector 16: PVD
    pvd_off = 16 * sector_size
    data[pvd_off] = 0x01
    data[pvd_off + 1 : pvd_off + 6] = b"CD001"
    data[pvd_off + 6] = 0x01
    data[pvd_off + 40 : pvd_off + 40 + len(volume_id)] = volume_id.encode("latin-1")

    # PARAM.SFO at sector 32, reachable through a real root directory. A
    # scan-only fixture no longer proves anything: the native reader refuses to
    # let an SFO found by scanning raw bytes authorize a catalog match, because
    # any image can embed one. Only an SFO reached through a validated
    # directory extent carries filesystem provenance, so the fixture has to be
    # a genuine ISO9660 structure rather than a blob parked in free space.
    sfo_bytes = build_param_sfo(disc_id, title, version)
    sfo_off = 32 * sector_size
    data[sfo_off : sfo_off + len(sfo_bytes)] = sfo_bytes

    # Real PSP layout: root -> PSP_GAME/ -> PARAM.SFO, which is what the
    # native reader traverses.
    root_lba, game_lba = 33, 34
    root_off, game_off = root_lba * sector_size, game_lba * sector_size
    root_entries = (
        _dir_record(bytes([0]), root_lba, sector_size, True)        # "."
        + _dir_record(bytes([1]), root_lba, sector_size, True)      # ".."
        + _dir_record(b"PSP_GAME", game_lba, sector_size, True)
    )
    data[root_off : root_off + len(root_entries)] = root_entries

    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)        # "."
        + _dir_record(bytes([1]), root_lba, sector_size, True)      # ".."
        + _dir_record(b"PARAM.SFO;1", 32, len(sfo_bytes), False)
    )
    data[game_off : game_off + len(game_entries)] = game_entries

    # PVD root directory record (ECMA-119 8.4.18) at byte 156.
    data[pvd_off + 156 : pvd_off + 156 + 34] = _dir_record(
        bytes([0]), root_lba, sector_size, True
    )[:34]

    path.write_bytes(data)


def _both_endian32(value: int) -> bytes:
    """ECMA-119 7.3.3: a 32-bit value recorded little-endian then big-endian."""
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _dir_record(name: bytes, extent_lba: int, size_bytes: int, is_dir: bool) -> bytes:
    """Build one ECMA-119 9.1 directory record, padded to an even length."""
    rec = bytearray(33 + len(name))
    rec[1] = 0                                          # extended attribute length
    rec[2:10] = _both_endian32(extent_lba)              # 9.1.3 extent
    rec[10:18] = _both_endian32(size_bytes)             # 9.1.4 data length
    rec[25] = 0x02 if is_dir else 0x00                  # 9.1.6 file flags
    rec[28:32] = (1).to_bytes(2, "little") + (1).to_bytes(2, "big")
    rec[32] = len(name)                                 # 9.1.10 identifier length
    rec[33:33 + len(name)] = name
    if len(rec) % 2:                                    # 9.1.12 padding
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def create_custom_sfo_iso(path: Path, sfo_bytes: bytes, volume_id: str = "CUSTOM_VOL") -> None:
    sector_size = 2048
    num_sectors = 1024
    data = bytearray(num_sectors * sector_size)

    pvd_off = 16 * sector_size
    data[pvd_off] = 0x01
    data[pvd_off + 1 : pvd_off + 6] = b"CD001"
    data[pvd_off + 6] = 0x01
    data[pvd_off + 40 : pvd_off + 40 + len(volume_id)] = volume_id.encode("latin-1")

    sfo_off = 32 * sector_size
    data[sfo_off : sfo_off + len(sfo_bytes)] = sfo_bytes
    path.write_bytes(data)


class IsoParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_parity_test_"))
        self.gcc = shutil.which("gcc")
        if not self.gcc:
            self.skipTest("gcc not available")

        # Compile native test runner
        self.harness_c = self.temp_dir / "parity_harness.c"
        self.exe_path = self.temp_dir / ("parity_harness.exe" if sys.platform == "win32" else "parity_harness")

        has_launch = (ROOT / "src" / "core" / "nk_launch.c").is_file()
        core_srcs = [
            ROOT / "src" / "core" / "nk_iso.c",
            ROOT / "src" / "core" / "nk_library.c",
            ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
        ]
        if has_launch:
            core_srcs.append(ROOT / "src" / "core" / "nk_launch.c")
        if sys.platform == "win32":
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_posix.c")

        harness_code = f"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_iso.h"
#include "nk_library.h"
#if {1 if has_launch else 0}
#include "nk_launch.h"
#endif

int main(int argc, char **argv) {{
    if (argc < 2) return 1;
    const char *mode = argv[1];

    if (strcmp(mode, "inspect") == 0) {{
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoMetadata meta;
        NkResult res = nk_iso_inspect(iso_path, &meta);
        if (res != NK_OK) {{
            printf("RESULT:ERROR:%d:%s\\n", (int)res, meta.error_message);
            return 0;
        }}
        printf("RESULT:OK\\n");
        printf("DISC_ID:%s\\n", meta.disc_id);
        printf("TITLE:%s\\n", meta.title_name);
        printf("VERSION:%s\\n", meta.disc_version);
        printf("VOLUME_ID:%s\\n", meta.volume_id);
        printf("SUPPORTED:%d\\n", meta.is_supported ? 1 : 0);
        printf("MATCHED_ID:%s\\n", meta.matched_title ? meta.matched_title->id : "NONE");
        printf("STATUS:%d\\n", (int)meta.status);
        return 0;
    }}

    if (strcmp(mode, "library_test") == 0) {{
        if (argc < 3) return 1;
        const char *lib_json = argv[2];
        NkLibrary lib;
        nk_library_init(&lib);

        NkGameEntry g1;
        memset(&g1, 0, sizeof(g1));
        snprintf(g1.disc_id, sizeof(g1.disc_id), "UCUS98701");
        snprintf(g1.title_name, sizeof(g1.title_name), "Hot Shots Tennis");
        snprintf(g1.title_id, sizeof(g1.title_id), "hst-ucus98701-v1");
        snprintf(g1.iso_path, sizeof(g1.iso_path), "C:/games/hst.iso");
        g1.status = NK_STATUS_VERIFIED;
        g1.is_prepared = true;
        g1.iso_size_bytes = 1000000;

        assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
        assert(nk_library_count(&lib) == 1);
        assert(nk_library_save(&lib, lib_json) == NK_OK);

        NkLibrary loaded;
        assert(nk_library_load(&loaded, lib_json) == NK_OK);
        assert(nk_library_count(&loaded) == 1);

        const NkGameEntry *entry = nk_library_find_by_disc_id(&loaded, "UCUS98701");
        assert(entry != NULL);
        assert(strcmp(entry->title_name, "Hot Shots Tennis") == 0);
        assert(strcmp(entry->title_id, "hst-ucus98701-v1") == 0);
        assert(entry->status == NK_STATUS_VERIFIED);
        assert(entry->is_prepared == true);

        /* Test remove */
        assert(nk_library_remove(&loaded, "UCUS98701") == NK_OK);
        assert(nk_library_count(&loaded) == 0);
        assert(nk_library_save(&loaded, lib_json) == NK_OK);

        printf("LIBRARY_TEST_PASSED\\n");
        return 0;
    }}

#if {1 if has_launch else 0}
    if (strcmp(mode, "launch_test") == 0) {{
        if (argc < 4) return 1;
        const char *repo_root = argv[2];
        const char *iso_path = argv[3];

        const char *want_disc = argc > 4 ? argv[4] : "UCUS98701";
        const char *want_title = argc > 5 ? argv[5] : "hst-ucus98701-v1";

        NkGameEntry g;
        memset(&g, 0, sizeof(g));
        snprintf(g.disc_id, sizeof(g.disc_id), "%s", want_disc);
        snprintf(g.title_id, sizeof(g.title_id), "%s", want_title);
        snprintf(g.iso_path, sizeof(g.iso_path), "%s", iso_path);

        NkLaunchSession session;
        NkResult res = nk_launch_prepare_session(&session, &g, repo_root);
        if (res != NK_OK) {{
            printf("LAUNCH_PREPARE_ERROR:%d:%s\\n", (int)res, session.last_error);
            return 0;
        }}
        printf("LAUNCH_PREPARE_OK\\n");
        printf("EXE:%s\\n", session.executable_path);
        printf("ISO:%s\\n", session.iso_path);
        printf("BASE:0x%08x\\n", session.base_address);
        printf("ENTRY:0x%08x\\n", session.entry_point);
        return 0;
    }}
#endif

    return 2;
}}
"""
        self.harness_c.write_text(harness_code, encoding="utf-8")

        cmd = [
            self.gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(ROOT / "src" / "core"),
            "-I", str(ROOT / "src" / "core" / "generated"),
            str(self.harness_c),
        ] + [str(s) for s in core_srcs] + [
            "-o", str(self.exe_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Compilation of parity harness failed: {res.stderr}")

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _run_native_inspect(self, iso_path: Path) -> dict[str, str]:
        cmd = [str(self.exe_path), "inspect", str(iso_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Native inspect failed: {res.stderr}")
        lines = res.stdout.strip().splitlines()
        out: dict[str, str] = {}
        for l in lines:
            if ":" in l:
                k, v = l.split(":", 1)
                out[k] = v
        return out

    def test_synthetic_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on synthetic title."""
        iso_file = self.temp_dir / "synthetic.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title", volume_id="SYNTH_VOL")

        # Python inspection
        py_meta = inspect_iso(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertTrue(py_meta.is_supported)
        self.assertIsNotNone(py_meta.matched_profile)

        # Native C inspection
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), py_meta.disc_id)
        self.assertEqual(c_meta.get("TITLE"), py_meta.title)
        self.assertEqual(c_meta.get("VERSION"), py_meta.version)
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_second_title_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on second public synthetic disc ID."""
        iso_file = self.temp_dir / "test5_mock.iso"
        create_test_iso(iso_file, disc_id="TEST00005", title="Phase 5 Test", volume_id="TEST00005")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00005")
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), "pspdev-phase5-v1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_uncataloged_retail_iso_parity(self) -> None:
        """Verify retail disc IDs are uncataloged in public mode by both inspectors."""
        iso_file = self.temp_dir / "retail_mock.iso"
        create_test_iso(iso_file, disc_id="UCUS98701", title="Hot Shots Tennis", volume_id="UCUS98701")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "UCUS98701")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_unsupported_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on unsupported disc ID."""
        iso_file = self.temp_dir / "unsupported.iso"
        create_test_iso(iso_file, disc_id="ULUS99999", title="Random Unsupported Game", volume_id="ULUS99999")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "ULUS99999")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_native_library_crud(self) -> None:
        """Verify native C library persistence (add, save, load, lookup, remove)."""
        lib_json = self.temp_dir / "library.json"
        cmd = [str(self.exe_path), "library_test", str(lib_json)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Library test failed: {res.stderr}")
        self.assertIn("LIBRARY_TEST_PASSED", res.stdout)

    def test_native_launch_plan(self) -> None:
        """Verify native C launch session resolves mock executable and ISO."""
        if not (ROOT / "src" / "core" / "nk_launch.c").is_file():
            self.skipTest("nk_launch.c not present in this slice")
        mock_root = self.temp_dir / "mock_repo"
        bin_dir = mock_root / "build" / "hst"
        bin_dir.mkdir(parents=True, exist_ok=True)
        exe_name = "hst.exe" if sys.platform == "win32" else "hst"
        mock_exe = bin_dir / exe_name
        mock_exe.write_bytes(b"MZfake")

        mock_iso = self.temp_dir / "game.iso"
        create_test_iso(mock_iso)

        # A title the public catalog DOES describe resolves, and takes its load
        # addresses from the catalog rather than from a constant.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn(str(mock_exe), res.stdout)
        self.assertIn(str(mock_iso), res.stdout)
        self.assertIn("BASE:0x08810000", res.stdout)
        self.assertIn("ENTRY:0x08810000", res.stdout)

        # A title it does not describe is refused. This used to "succeed" by
        # launching the guest at a hard-coded 0x0029a060 with a zero base: an
        # address belonging to no public title, so the runtime was started at a
        # constant rather than at anything the catalog knew. A public-safe tree
        # has no addresses for a retail identity and must say so.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "UCUS98701", "hst-ucus98701-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("No catalog entry", res.stdout)

    def test_duplicate_sfo_keys_parity(self) -> None:
        """Verify identical duplicate SFO keys accepted and conflicting rejected by both Python and C."""
        from nk_core.iso_inspect import IsoInspectionError

        # 1. Conflicting keys: Python and Native C must reject
        sfo_conflict = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"ULUS10001\0"),
            ("TITLE", 0x0204, b"Test Conflict\0"),
        ])
        iso_conflict = self.temp_dir / "conflict.iso"
        create_custom_sfo_iso(iso_conflict, sfo_conflict)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_conflict)

        c_conflict = self._run_native_inspect(iso_conflict)
        self.assertTrue(c_conflict.get("RESULT", "").startswith("ERROR"))

        # 2. Byte-identical keys: Python and Native C must accept
        sfo_identical = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Test Identical\0"),
        ])
        iso_identical = self.temp_dir / "identical.iso"
        create_custom_sfo_iso(iso_identical, sfo_identical)

        py_identical = inspect_iso(iso_identical)
        self.assertEqual(py_identical.disc_id, "UCUS98701")

        c_identical = self._run_native_inspect(iso_identical)
        self.assertEqual(c_identical.get("RESULT"), "OK")
        self.assertEqual(c_identical.get("DISC_ID"), "UCUS98701")

    def test_identity_parameter_format_parity(self) -> None:
        """An identity field in a non-string format is refused by both parsers.

        The native reader used to ignore the parameter format at entry offset
        +2 and decode the bytes as text regardless, while the Python inspector
        yielded the decoded integer. A disc whose DISC_ID declared an integer
        format could therefore be matched to a catalog title by one parser and
        not the other -- the differential contract these two are meant to keep.
        """
        from nk_core.iso_inspect import IsoInspectionError

        # DISC_ID declared as uint32 (0x0404) while carrying string bytes.
        sfo_int_id = build_custom_param_sfo([
            ("DISC_ID", 0x0404, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Wrong Format\0"),
        ])
        iso_int_id = self.temp_dir / "int_disc_id.iso"
        create_custom_sfo_iso(iso_int_id, sfo_int_id)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_int_id)

        c_int_id = self._run_native_inspect(iso_int_id)
        self.assertTrue(c_int_id.get("RESULT", "").startswith("ERROR"))

        # An unrecognised format is refused the same way, not silently ignored.
        sfo_unknown = build_custom_param_sfo([
            ("DISC_ID", 0x0101, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Unknown Format\0"),
        ])
        iso_unknown = self.temp_dir / "unknown_fmt.iso"
        create_custom_sfo_iso(iso_unknown, sfo_unknown)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_unknown)

        c_unknown = self._run_native_inspect(iso_unknown)
        self.assertTrue(c_unknown.get("RESULT", "").startswith("ERROR"))

        # A non-identity key in a non-string format is NOT a reason to reject:
        # only identity decides whether a disc matches a catalog title.
        sfo_other = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Fine\0"),
            ("PARENTAL_LEVEL", 0x0404, bytes([1, 0, 0, 0])),
        ])
        iso_other = self.temp_dir / "other_int_key.iso"
        create_custom_sfo_iso(iso_other, sfo_other)

        py_other = inspect_iso(iso_other)
        self.assertEqual(py_other.disc_id, "UCUS98701")

        c_other = self._run_native_inspect(iso_other)
        self.assertEqual(c_other.get("RESULT"), "OK")
        self.assertEqual(c_other.get("DISC_ID"), "UCUS98701")



if __name__ == "__main__":
    unittest.main()
