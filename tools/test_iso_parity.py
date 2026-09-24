# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Differential parity tests comparing native C core (nk_iso, nk_library, nk_launch) against Python."""

from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from nk_core.iso_inspect import (
    inspect_compatibility_preflight,
    inspect_iso,
    write_experimental_profile,
)
import nk_cli


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


def create_test_iso_with_executables(
    path: Path, eboot: bytes, boot: bytes | None = None,
    disc_id: str = "TEST00001", title: str = "Test Game",
) -> None:
    """Add synthetic SYSDIR executables to the source-owned ISO fixture."""
    create_test_iso(path, disc_id=disc_id, title=title)
    sector_size = 2048
    data = bytearray(path.read_bytes())
    game_lba, sysdir_lba, eboot_lba, boot_lba = 34, 35, 36, 37
    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), 33, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, len(build_param_sfo(disc_id, title)), False)
        + _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
    )
    sysdir_entries = (
        _dir_record(bytes([0]), sysdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
        + _dir_record(b"EBOOT.BIN;1", eboot_lba, len(eboot), False)
    )
    if boot is not None:
        sysdir_entries += _dir_record(b"BOOT.BIN;1", boot_lba, len(boot), False)
    data[game_lba * sector_size : (game_lba + 1) * sector_size] = bytes(sector_size)
    data[sysdir_lba * sector_size : (sysdir_lba + 1) * sector_size] = bytes(sector_size)
    data[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = game_entries
    data[sysdir_lba * sector_size : sysdir_lba * sector_size + len(sysdir_entries)] = sysdir_entries
    data[eboot_lba * sector_size : eboot_lba * sector_size + len(eboot)] = eboot
    if boot is not None:
        data[boot_lba * sector_size : boot_lba * sector_size + len(boot)] = boot
    path.write_bytes(data)


def build_plain_mips_elf(e_type: int = 2) -> bytes:
    elf = bytearray(88)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, e_type, 8, 1)
    struct.pack_into("<III", elf, 24, 0x08800000, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 1, 0, 0)
    struct.pack_into("<8I", elf, 52, 1, 84, 0x08800000, 0x08800000, 4, 4, 5, 4)
    elf[84:88] = b"\x34\x12\x00\x00"
    return bytes(elf)


def build_psp_container() -> bytes:
    container = bytearray(0x80)
    container[:4] = b"~PSP"
    container[0x27] = 1
    struct.pack_into("<I", container, 0x54, 0x1000)
    return bytes(container)


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
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.gcc = shutil.which("gcc")
        if not cls.gcc:
            raise unittest.SkipTest("gcc not available")

        # Compile the native test runner once.  The tests assert parser and
        # launch behaviour, not compiler freshness; each test still receives
        # its own input/output directory below.
        cls._build_dir = Path(tempfile.mkdtemp(prefix="nk_parity_build_"))
        cls.harness_c = cls._build_dir / "parity_harness.c"
        cls.exe_path = cls._build_dir / ("parity_harness.exe" if sys.platform == "win32" else "parity_harness")

        has_launch = (ROOT / "src" / "core" / "nk_launch.c").is_file()
        core_srcs = [
            ROOT / "src" / "core" / "nk_iso.c",
            ROOT / "src" / "core" / "nk_library.c",
            ROOT / "src" / "core" / "nk_title_manifest.c",
            ROOT / "src" / "core" / "nk_json.c",
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
#include "nk_title_manifest.h"
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
        printf("PARAM_SFO_PARSED:%d\\n", meta.param_sfo_parsed ? 1 : 0);
        printf("EBOOT_KIND:%d\\n", (int)meta.executables.eboot.kind);
        printf("BOOT_KIND:%d\\n", (int)meta.executables.boot.kind);
        printf("SELECTED_EXECUTABLE:%d\\n", (int)meta.executables.selected);
        printf("BOOT_FALLBACK:%d\\n", meta.executables.boot_fallback ? 1 : 0);
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
        g1.is_experimental = true;
        g1.executable_eboot_kind = 2;
        g1.executable_boot_kind = 1;
        g1.executable_selection = 2;
        g1.executable_boot_fallback = true;
        snprintf(g1.selected_executable, sizeof(g1.selected_executable), "BOOT.BIN");
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
        assert(entry->is_experimental == true);
        assert(entry->executable_eboot_kind == 2);
        assert(entry->executable_boot_kind == 1);
        assert(entry->executable_selection == 2);
        assert(entry->executable_boot_fallback == true);
        assert(strcmp(entry->selected_executable, "BOOT.BIN") == 0);

        /* Test remove */
        assert(nk_library_remove(&loaded, "UCUS98701") == NK_OK);
        assert(nk_library_count(&loaded) == 0);
        assert(nk_library_save(&loaded, lib_json) == NK_OK);

        printf("LIBRARY_TEST_PASSED\\n");
        return 0;
    }}

    if (strcmp(mode, "experimental_profile") == 0) {{
        if (argc < 4) return 1;
        const char *iso_path = argv[2];
        const char *user_data_root = argv[3];
        NkIsoMetadata meta;
        NkResult inspect_result = nk_iso_inspect(iso_path, &meta);
        if (inspect_result != NK_OK) {{
            printf("PROFILE_RESULT:ERROR:DISC:%s\\n", meta.error_message);
            return 0;
        }}
        char profile_id[64] = {{0}};
        char error[256] = {{0}};
        bool ok = nk_title_manifest_write_experimental_profile(
            iso_path, meta.param_sfo_parsed, meta.disc_id, meta.title_name,
            meta.executables.selected_path, user_data_root, profile_id,
            sizeof(profile_id), error, sizeof(error));
        if (!ok) {{
            printf("PROFILE_RESULT:ERROR:%s\\n", error);
            return 0;
        }}
        printf("PROFILE_RESULT:OK\\n");
        printf("PROFILE_ID:%s\\n", profile_id);
        printf("PACKAGE_AVAILABLE:%d\\n",
               nk_launch_runtime_package_available(user_data_root, profile_id) ? 1 : 0);
        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", meta.disc_id);
        snprintf(game.title_id, sizeof(game.title_id), "%s", profile_id);
        snprintf(game.iso_path, sizeof(game.iso_path), "%s", iso_path);
        NkLaunchSession session;
        NkResult launch_result = nk_launch_prepare_session(
            &session, &game, user_data_root);
        printf("LAUNCH_PREPARE:%s\\n",
               launch_result == NK_OK ? "OK" : "REFUSED");
        printf("LAUNCH_REASON:%s\\n", session.last_error);
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

    if (strcmp(mode, "reader_test") == 0) {{
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoReader *reader = nk_iso_reader_open(iso_path);
        if (!reader) {{
            printf("READER:OPEN_FAILED\\n");
            return 0;
        }}
        printf("READER:OPEN_OK\\n");
        printf("VOLUME_ID:%s\\n", nk_iso_reader_volume_id(reader));
        printf("FILE_SIZE:%llu\\n", (unsigned long long)nk_iso_reader_file_size(reader));

        uint32_t lba = 0, sz = 0;
        bool is_dir = false;
        int rc = nk_iso_reader_lookup(reader, "PSP_GAME/PARAM.SFO", &lba, &sz, &is_dir);
        printf("LOOKUP_SFO:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "PSP_GAME", &lba, &sz, &is_dir);
        printf("LOOKUP_DIR:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "NONEXISTENT", &lba, &sz, &is_dir);
        printf("LOOKUP_MISS:%d\\n", rc);

        NkIsoDirEntry de;
        rc = nk_iso_reader_list(reader, "", 0, &de);
        printf("LIST_ROOT_0:%d:%s:%u:%u:%d\\n", rc, de.name, (unsigned)de.lba, (unsigned)de.size, de.is_directory ? 1 : 0);

        rc = nk_iso_reader_list(reader, "", 1, &de);
        printf("LIST_ROOT_1:%d\\n", rc);

        uint8_t buf[256];
        int bytes = nk_iso_reader_read(reader, 32, 0, buf, sizeof(buf));
        printf("READ_SFO:%d\\n", bytes);

        bytes = nk_iso_reader_read(reader, 2000, 0, buf, sizeof(buf));
        printf("READ_EOF:%d\\n", bytes);

        nk_iso_reader_close(reader);
        return 0;
    }}

    return 2;
}}
"""
        cls.harness_c.write_text(harness_code, encoding="utf-8")

        cmd = [
            cls.gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(ROOT / "src" / "core"),
            "-I", str(ROOT / "src" / "core" / "generated"),
            str(cls.harness_c),
        ] + [str(s) for s in core_srcs] + [
            "-o", str(cls.exe_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode:
            raise AssertionError(f"Compilation of parity harness failed: {res.stderr}")

    def setUp(self) -> None:
        # Inputs and any application data remain isolated per test even though
        # the immutable native runner is shared by the class.
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_parity_test_"))
        self.exe_path = type(self).exe_path

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    @classmethod
    def tearDownClass(cls) -> None:
        build_dir = getattr(cls, "_build_dir", None)
        if build_dir is not None:
            shutil.rmtree(build_dir, ignore_errors=True)
        super().tearDownClass()

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
        self.assertEqual(c_meta.get("PARAM_SFO_PARSED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_cli_inspect_shows_structured_compatibility_preflight(self) -> None:
        iso_file = self.temp_dir / "cli-preflight.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("compatibility_preflight", payload)
        checks = payload["compatibility_preflight"]["checks"]
        self.assertEqual(
            [check["code"] for check in checks],
            ["DISC_SFO", "EXECUTABLE", "RUNTIME_PACKAGE", "SYSTEM_FONTS", "AUDIO_OUTPUT"],
        )
        self.assertTrue(all(check["status"] in {
            "OK", "MISSING", "UNSUPPORTED", "IN_PROGRESS",
        } for check in checks))
        by_code = {check["code"]: check for check in checks}
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["SYSTEM_FONTS"]["status"], "MISSING")
        self.assertEqual(by_code["AUDIO_OUTPUT"]["status"], "OK")

        meta = inspect_iso(iso_file)
        assert meta.matched_profile is not None
        title_name = meta.matched_profile.game_name
        package_dir = runtime_root / "build" / title_name
        package_dir.mkdir(parents=True)
        (package_dir / f"{title_name}.exe").write_bytes(b"synthetic executable")
        (package_dir / f"{title_name}_image.bin").write_bytes(b"synthetic image")
        font_dir = runtime_root / "font"
        font_dir.mkdir()
        (font_dir / "jpn0.pgf").write_bytes(b"synthetic font marker")
        ready = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(ready.returncode, 0, ready.stderr)
        ready_checks = {check["code"]: check for check in
                        json.loads(ready.stdout)["compatibility_preflight"]["checks"]}
        self.assertEqual(ready_checks["RUNTIME_PACKAGE"]["status"], "OK")
        self.assertEqual(ready_checks["SYSTEM_FONTS"]["status"], "OK")

    def test_cli_preflight_selects_plain_boot_fallback(self) -> None:
        iso_file = self.temp_dir / "cli-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf()
        )
        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["EBOOT.BIN"]["classification"],
                         "PSP_ENCRYPTED_CONTAINER")
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        self.assertTrue(report["boot_fallback"])
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["PARAM_SFO_PARSED"], "1")
        self.assertEqual(native["EBOOT_KIND"], "2")
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")
        self.assertEqual(native["BOOT_FALLBACK"], "1")
        executable_check = next(
            check for check in report["checks"] if check["code"] == "EXECUTABLE"
        )
        self.assertEqual(executable_check["status"], "OK")
        self.assertIn("BOOT.BIN selected for analysis", executable_check["message"])

    def test_cli_preflight_guides_encrypted_eboot_to_title_scoped_user_input(self) -> None:
        iso_file = self.temp_dir / "cli-encrypted-user-input.iso"
        user_root = self.temp_dir / "user-data"
        decrypted_dir = user_root / "titles" / "TEST00001" / "decrypted"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id="TEST00001",
            title="Synthetic Test Title",
        )

        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "UNSUPPORTED")
        self.assertIn(f"supply decrypted modules at {decrypted_dir}".lower(),
                      executable["message"].lower())
        self.assertIn("#295", executable["message"])
        cli = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(cli.returncode, 0, cli.stderr)
        cli_checks = json.loads(cli.stdout)["compatibility_preflight"]["checks"]
        cli_executable = next(check for check in cli_checks
                              if check["code"] == "EXECUTABLE")
        self.assertIn(str(decrypted_dir), cli_executable["message"])
        self.assertIn("in the works", cli_executable["message"])
        self.assertIn("#295", cli_executable["message"])

        decrypted_dir.mkdir(parents=True)
        eboot = decrypted_dir / "EBOOT.elf"
        eboot.write_bytes(b"not an ELF")
        invalid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        invalid_check = next(check for check in invalid["checks"]
                             if check["code"] == "EXECUTABLE")
        self.assertEqual(invalid_check["status"], "UNSUPPORTED")
        self.assertIn("invalid", invalid_check["message"].lower())

        eboot.write_bytes(build_plain_mips_elf())
        valid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        valid_check = next(check for check in valid["checks"]
                           if check["code"] == "EXECUTABLE")
        self.assertEqual(valid_check["status"], "OK")
        self.assertEqual(valid["selected_executable"], "EBOOT.elf")
        self.assertEqual(valid["decrypted_executable"], str(eboot))
        for name, wrapped in (("sce", b"~SCE" + bytes(124)),
                              ("pbp", b"\0PBP" + bytes(124))):
            wrapped_iso = self.temp_dir / f"cli-wrapped-{name}.iso"
            create_test_iso_with_executables(
                wrapped_iso, wrapped, disc_id="TEST00001",
                title="Synthetic Test Title",
            )
            wrapped_meta = inspect_iso(wrapped_iso)
            wrapped_report = inspect_compatibility_preflight(
                wrapped_iso, metadata=wrapped_meta, runtime_root=user_root
            )
            wrapped_check = next(check for check in wrapped_report["checks"]
                                 if check["code"] == "EXECUTABLE")
            self.assertEqual(wrapped_check["status"], "OK")
            self.assertEqual(wrapped_report["selected_executable"], "EBOOT.elf")

    def test_build_package_discovers_user_decrypted_elf_and_modules(self) -> None:
        iso_file = self.temp_dir / "synthetic-package-input.iso"
        user_root = self.temp_dir / "package-user-data"
        disc_id = "TEST00002"
        title_id = "synthetic-title2-v1"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id=disc_id,
            title="Synthetic Title 2 Fixture",
        )
        entry = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        user_root.mkdir()
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry]}),
            encoding="utf-8",
        )
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True)
        eboot_bytes = build_plain_mips_elf()
        module_bytes = build_plain_mips_elf(0xFFA0)
        (decrypted_dir / "EBOOT.elf").write_bytes(eboot_bytes)
        (decrypted_dir / "synthetic2.prx").write_bytes(module_bytes)

        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["modules"][0]["required"] = True
        manifest["modules"][0]["role"] = "guest-prx"
        captured_command: list[str] = []

        def fake_package_build(command, **_kwargs):
            captured_command.extend(command)
            build_dir = Path(command[command.index("--output-dir") + 1])
            build_dir.mkdir(parents=True, exist_ok=True)
            package = {
                "title": {"id": title_id},
                "inputs": {"executable": {"sha256": hashlib.sha256(eboot_bytes).hexdigest()}},
            }
            (build_dir / "package.json").write_text(
                json.dumps(package), encoding="utf-8"
            )
            return subprocess.CompletedProcess(command, 0, "", "")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()
        with patch.object(nk_cli, "_load_entry_manifest",
                          return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_package_build), \
             patch.object(nk_cli, "_stage_runtime_assets"):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)

        game_elf = Path(captured_command[captured_command.index("--game-elf") + 1])
        module_dir = Path(captured_command[captured_command.index("--module-dir") + 1])
        self.assertEqual(game_elf.read_bytes(), eboot_bytes)
        self.assertEqual((module_dir / "synthetic2.prx").read_bytes(), module_bytes)
        (decrypted_dir / "synthetic2.prx").write_bytes(b"not an ELF")
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a decrypted ELF"):
            nk_cli._copy_optional_modules(
                iso_file, manifest, user_root / "bad-module-cache", None,
                decrypted_dir,
            )

    def test_prx_format_boot_fallback_is_selected_by_both_inspectors(self) -> None:
        """Retail BOOT.BIN is often PSP PRX-format (e_type 0xFFA0), which the
        analyzer and runtime loader accept; the preflight must not call it
        unusable."""
        iso_file = self.temp_dir / "prx-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root-prx"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf(0xFFA0)
        )
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")

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

    def test_uncatalogued_param_sfo_disc_is_experimental(self) -> None:
        """Unknown PSP discs get validated local identity profiles and stay launch-gated."""
        iso_file = self.temp_dir / "experimental_mock.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        executable_bytes = build_plain_mips_elf() + bytes((i % 251 for i in range(70000)))
        create_test_iso_with_executables(
            iso_file, executable_bytes, disc_id="ULUS99998",
            title="Experimental Fixture",
        )

        metadata = inspect_iso(iso_file)
        self.assertIsNone(metadata.matched_profile)
        preflight = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertTrue(preflight["is_experimental"])
        by_code = {check["code"]: check for check in preflight["checks"]}
        self.assertEqual(by_code["EXPERIMENTAL"]["issues"], [285, 308])
        self.assertIn("Compatibility is unknown", by_code["EXPERIMENTAL"]["message"])
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["issues"], [296, 297])

        python_root = self.temp_dir / "python-user-data"
        python_profile_path = write_experimental_profile(
            iso_file, python_root, metadata=metadata
        )
        python_profile = json.loads(python_profile_path.read_text(encoding="utf-8"))
        from title_manifest import validate_manifest
        manifest = validate_manifest(python_profile["manifest"])
        identity = python_profile["input_identity"]
        expected_hash = hashlib.sha256(executable_bytes).hexdigest()
        self.assertEqual(manifest["disc"]["id"], "ULUS99998")
        self.assertEqual(manifest["display_name"], "Experimental Fixture")
        self.assertEqual(manifest["executable"]["base"], 0)
        self.assertEqual(manifest["executable"]["entry"], 0)
        self.assertEqual(manifest["modules"], [])
        self.assertEqual(identity["selected_executable"], "PSP_GAME/SYSDIR/EBOOT.BIN")
        self.assertEqual(identity["executable_sha256"], expected_hash)
        self.assertEqual(identity["elf_sha256"], expected_hash)
        self.assertTrue(python_profile_path.is_relative_to(python_root))

        native_root = self.temp_dir / "native-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:OK", native.stdout)
        self.assertIn("PACKAGE_AVAILABLE:0", native.stdout)
        self.assertIn("LAUNCH_PREPARE:REFUSED", native.stdout)
        native_profile_path = native_root / "experimental" / "ULUS99998" / "profile.json"
        native_profile = json.loads(native_profile_path.read_text(encoding="utf-8"))
        native_manifest = validate_manifest(native_profile["manifest"])
        self.assertEqual(native_manifest["id"], "experimental-ulus99998")
        self.assertEqual(native_profile["input_identity"]["executable_sha256"], expected_hash)
        self.assertEqual(native_profile["input_identity"]["elf_sha256"], expected_hash)

    def test_non_psp_iso_without_directory_reachable_sfo_is_refused(self) -> None:
        """An embedded but unreferenced PARAM.SFO does not authorize experimental import."""
        iso_file = self.temp_dir / "non-psp.iso"
        create_test_iso(iso_file, disc_id="ULUS99997", title="Orphaned SFO")
        data = bytearray(iso_file.read_bytes())
        root_off = 33 * 2048
        original_root = bytes(data[root_off:root_off + 2048])
        data[root_off:root_off + 2048] = bytes(2048)
        # Keep only the ISO root's self and parent records, removing PSP_GAME.
        data[root_off:root_off + 68] = original_root[:68]
        iso_file.write_bytes(data)

        metadata = inspect_iso(iso_file)
        runtime_root = self.temp_dir / "empty-runtime-root-non-psp"
        runtime_root.mkdir()
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertFalse(report["is_experimental"])
        self.assertEqual(report["checks"][0]["code"], "DISC_SFO")
        with self.assertRaisesRegex(ValueError, "requires PARAM.SFO"):
            write_experimental_profile(iso_file, self.temp_dir / "refused-user-data",
                                       metadata=metadata)

        native_root = self.temp_dir / "native-refused-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:ERROR:Experimental profiles require", native.stdout)
        self.assertFalse((native_root / "experimental").exists())

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
        """Native launch resolution is identity-bound to the selected title.

        #366 failing-before contract: this fixture used to PROVE the defect --
        a stale build/<retail>/<retail> runtime satisfied an unrelated
        selected title (display-smoke-v1) and the session paired that binary
        with another title's session data. The stale artifact now must be
        irrelevant, and only the selected title's own build may resolve.
        """
        if not (ROOT / "src" / "core" / "nk_launch.c").is_file():
            self.skipTest("nk_launch.c not present in this slice")
        mock_root = self.temp_dir / "mock_repo"
        stale_dir = mock_root / "build" / "hst"
        stale_dir.mkdir(parents=True, exist_ok=True)
        exe_name = "hst.exe" if sys.platform == "win32" else "hst"
        stale_exe = stale_dir / exe_name
        stale_exe.write_bytes(b"MZfake")
        (stale_dir / "hst_image.bin").write_bytes(b"image")

        mock_iso = self.temp_dir / "game.iso"
        create_test_iso(mock_iso)

        # The native harness resolves application data from the environment, so
        # every launch below runs with it pointed at this test's temp directory.
        # Built once and passed to both invocations: the refusal case further
        # down must not escape the isolation either.
        env = {
            **os.environ,
            "LOCALAPPDATA": str(self.temp_dir),
            "APPDATA": str(self.temp_dir),
            "USERPROFILE": str(self.temp_dir),
            "HOME": str(self.temp_dir),
        }

        # A selected title the catalog describes must NOT resolve through the
        # stale retail-shaped build: only its own game_name layout counts, and
        # an absent own runtime is an honest missing-runtime error.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("Runtime binary not found", res.stdout)
        self.assertNotIn("hst.exe", res.stdout)

        # With the selected title's own runtime present, the session resolves
        # it -- still ignoring the stale sibling -- and takes its addresses
        # from the catalog rather than from a constant.
        own_dir = mock_root / "build" / "display-smoke"
        own_dir.mkdir(parents=True, exist_ok=True)
        own_exe = own_dir / ("display-smoke.exe" if sys.platform == "win32" else "display-smoke")
        own_exe.write_bytes(b"MZfake")
        (own_dir / "display-smoke_image.bin").write_bytes(b"image")

        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn(Path(own_exe).name, res.stdout)
        self.assertNotIn("hst.exe", res.stdout)
        self.assertIn(Path(mock_iso).name, res.stdout)
        self.assertIn("BASE:0x08810000", res.stdout)
        self.assertIn("ENTRY:0x08810000", res.stdout)

        # A title it does not describe is refused. This used to "succeed" by
        # launching the guest at a hard-coded 0x0029a060 with a zero base: an
        # address belonging to no public title, so the runtime was started at a
        # constant rather than at anything the catalog knew. A public-safe tree
        # has no addresses for a retail identity and must say so.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "UCUS98701", "hst-ucus98701-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
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
        valid_iso = self.temp_dir / "preflight-valid.iso"
        create_test_iso(valid_iso)
        preflight_meta = inspect_iso(valid_iso)
        preflight = inspect_compatibility_preflight(
            iso_conflict, metadata=preflight_meta, runtime_root=self.temp_dir
        )
        disc_check = next(check for check in preflight["checks"]
                          if check["code"] == "DISC_SFO")
        self.assertEqual(disc_check["status"], "UNSUPPORTED")

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

    def test_reader_parity_inspect_and_lookup(self) -> None:
        iso_path = self.temp_dir / "reader_test.iso"
        create_test_iso(iso_path, disc_id="UCUS98701", title="Reader Test", volume_id="READER_VOL")

        # Python inspect
        py_meta = inspect_iso(iso_path)
        self.assertEqual(py_meta.volume_id.rstrip("\x00 "), "READER_VOL")

        # Native reader test
        cmd = [str(self.exe_path), "reader_test", str(iso_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        lines = dict(line.split(":", 1) for line in res.stdout.strip().splitlines() if ":" in line)
        self.assertEqual(lines.get("READER"), "OPEN_OK")
        self.assertEqual(lines.get("VOLUME_ID"), "READER_VOL")
        self.assertEqual(lines.get("FILE_SIZE"), str(1024 * 2048))

        sfo_parts = lines.get("LOOKUP_SFO", "").split(":")
        self.assertEqual(sfo_parts[0], "0")
        self.assertEqual(sfo_parts[1], "32")
        self.assertEqual(sfo_parts[3], "0")

        dir_parts = lines.get("LOOKUP_DIR", "").split(":")
        self.assertEqual(dir_parts[0], "0")
        self.assertEqual(dir_parts[1], "34")
        self.assertEqual(dir_parts[3], "1")

        self.assertEqual(lines.get("LOOKUP_MISS"), "-1")

        list_parts = lines.get("LIST_ROOT_0", "").split(":")
        self.assertEqual(list_parts[0], "1")
        self.assertEqual(list_parts[1], "PSP_GAME")
        self.assertEqual(list_parts[2], "34")
        self.assertEqual(list_parts[4], "1")

        self.assertEqual(lines.get("LIST_ROOT_1"), "0")
        self.assertGreater(int(lines.get("READ_SFO", "0")), 0)
        self.assertEqual(lines.get("READ_EOF"), "0")

    def test_reader_invalid_iso_fails_open(self) -> None:
        bad_iso = self.temp_dir / "bad.iso"
        bad_iso.write_bytes(b"garbage data not an iso" * 100)
        cmd = [str(self.exe_path), "reader_test", str(bad_iso)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertIn("READER:OPEN_FAILED", res.stdout)


if __name__ == "__main__":
    unittest.main()
