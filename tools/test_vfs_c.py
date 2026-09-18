# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Host-neutral VFS path joining regression test (issue #19).

Proves that:
1. Executable selftest (src/rt/vfs_selftest.c) passes cleanly, exercising all path
   join edge cases (trailing slashes, device prefix stripping, traversal prevention,
   overflow limits).
2. Joining root="fs" and guest="ms0:/PSP/SAVEDATA" produces "fs/PSP/SAVEDATA" or
   "fs\\PSP\\SAVEDATA", never "fsPSP...".
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "vfs_selftest.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestVfsSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="vfsc_")
        cls.exe = os.path.join(cls.tmp, "vfs_selftest.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c11",
                "-O0",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src' / 'rt'}",
                "-o",
                cls.exe,
                str(SELFTEST_C),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("vfs_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_vfs_path_join_invariants_hold(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("vfs selftest: OK", result.stdout)


def _both_endian32(value: int) -> bytes:
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _dir_record(name: bytes, extent_lba: int, size_bytes: int, is_dir: bool) -> bytes:
    rec = bytearray(33 + len(name))
    rec[1] = 0
    rec[2:10] = _both_endian32(extent_lba)
    rec[10:18] = _both_endian32(size_bytes)
    rec[25] = 0x02 if is_dir else 0x00
    rec[28:32] = (1).to_bytes(2, "little") + (1).to_bytes(2, "big")
    rec[32] = len(name)
    rec[33 : 33 + len(name)] = name
    if len(rec) % 2:
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def create_synthetic_vfs_iso(path: Path) -> None:
    sector_size = 2048
    num_sectors = 1024
    data = bytearray(num_sectors * sector_size)

    # Sector 16: PVD
    pvd_off = 16 * sector_size
    data[pvd_off] = 0x01
    data[pvd_off + 1 : pvd_off + 6] = b"CD001"
    data[pvd_off + 6] = 0x01
    vol_id = "VFS_TEST_DISC"
    data[pvd_off + 40 : pvd_off + 40 + len(vol_id)] = vol_id.encode("latin-1")

    # File contents
    eboot_off = 37 * sector_size
    data[eboot_off : eboot_off + 4096] = b"\xAA" * 4096

    data_off = 39 * sector_size
    data[data_off : data_off + 1000] = b"\x55" * 1000

    sfo_off = 32 * sector_size
    data[sfo_off : sfo_off + 256] = b"\x33" * 256

    tail_off = 1023 * sector_size
    data[tail_off : tail_off + 2048] = b"\x77" * 2048

    # Directories
    root_entries = (
        _dir_record(bytes([0]), 33, sector_size, True)
        + _dir_record(bytes([1]), 33, sector_size, True)
        + _dir_record(b"PSP_GAME", 34, sector_size, True)
    )
    data[33 * sector_size : 33 * sector_size + len(root_entries)] = root_entries

    psp_game_entries = (
        _dir_record(bytes([0]), 34, sector_size, True)
        + _dir_record(bytes([1]), 33, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, 256, False)
        + _dir_record(b"SYSDIR", 35, sector_size, True)
        + _dir_record(b"USRDIR", 36, sector_size, True)
    )
    data[34 * sector_size : 34 * sector_size + len(psp_game_entries)] = psp_game_entries

    sysdir_entries = (
        _dir_record(bytes([0]), 35, sector_size, True)
        + _dir_record(bytes([1]), 34, sector_size, True)
        + _dir_record(b"EBOOT.BIN;1", 37, 4096, False)
    )
    data[35 * sector_size : 35 * sector_size + len(sysdir_entries)] = sysdir_entries

    usrdir_entries = (
        _dir_record(bytes([0]), 36, sector_size, True)
        + _dir_record(bytes([1]), 34, sector_size, True)
        + _dir_record(b"DATA.BIN;1", 39, 1000, False)
        + _dir_record(b"TAIL.BIN;1", 1023, 2048, False)
    )
    data[36 * sector_size : 36 * sector_size + len(usrdir_entries)] = usrdir_entries

    data[pvd_off + 156 : pvd_off + 156 + 34] = _dir_record(bytes([0]), 33, sector_size, True)[:34]
    path.write_bytes(data)


ISO_HARNESS_C = r"""#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "iso.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static DWORD WINAPI worker_func(LPVOID arg) {
    (void)arg;
    uint8_t buf[256];
    for (int i = 0; i < 500; i++) {
        int got = iso_read(32, 0, buf, 256);
        if (got != 256 || buf[0] != 0x33 || buf[255] != 0x33) return 1;
        got = iso_read(37, 100, buf, 200);
        if (got != 200 || buf[0] != 0xAA || buf[199] != 0xAA) return 1;
    }
    return 0;
}
static int run_concurrency(void) {
    HANDLE th[8];
    for (int i = 0; i < 8; i++) {
        th[i] = CreateThread(NULL, 0, worker_func, NULL, 0, NULL);
        if (!th[i]) return 1;
    }
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    DWORD code;
    for (int i = 0; i < 8; i++) {
        GetExitCodeThread(th[i], &code);
        CloseHandle(th[i]);
        if (code != 0) return 1;
    }
    return 0;
}
#else
#include <pthread.h>
static void *worker_func(void *arg) {
    (void)arg;
    uint8_t buf[256];
    for (int i = 0; i < 500; i++) {
        int got = iso_read(32, 0, buf, 256);
        if (got != 256 || buf[0] != 0x33 || buf[255] != 0x33) return (void *)(intptr_t)1;
        got = iso_read(37, 100, buf, 200);
        if (got != 200 || buf[0] != 0xAA || buf[199] != 0xAA) return (void *)(intptr_t)1;
    }
    return NULL;
}
static int run_concurrency(void) {
    pthread_t th[8];
    for (int i = 0; i < 8; i++) {
        if (pthread_create(&th[i], NULL, worker_func, NULL) != 0) return 1;
    }
    void *res;
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], &res);
        if (res != NULL) return 1;
    }
    return 0;
}
#endif

static void set_env_iso(const char *val) {
#if defined(_WIN32) || defined(_WIN64)
    char buf[1024];
    snprintf(buf, sizeof(buf), "PSP_ISO=%s", val ? val : "");
    _putenv(buf);
    SetEnvironmentVariableA("PSP_ISO", val ? val : "");
#else
    if (val && val[0]) setenv("PSP_ISO", val, 1);
    else unsetenv("PSP_ISO");
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <iso_path>\n", argv[0]);
        return 1;
    }
    const char *iso_path = argv[1];

    /* Test 1: Unset / empty PSP_ISO -> iso_init fails */
    set_env_iso("");
    if (iso_init() == 0) {
        fprintf(stderr, "FAIL: iso_init should fail when PSP_ISO is empty\n");
        return 1;
    }

    /* Test 2: Invalid file -> iso_init fails */
    set_env_iso("nonexistent_file_path.iso");
    if (iso_init() == 0) {
        fprintf(stderr, "FAIL: iso_init should fail with nonexistent file\n");
        return 1;
    }

    /* Test 3: Valid file -> iso_init succeeds */
    set_env_iso(iso_path);
    if (iso_init() != 0) {
        fprintf(stderr, "FAIL: iso_init failed for valid ISO\n");
        return 1;
    }

    /* Test 4: iso_lookup */
    uint32_t lba = 0, sz = 0;
    if (iso_lookup("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) != 0 || lba != 37 || sz != 4096) {
        fprintf(stderr, "FAIL: lookup EBOOT.BIN got lba=%u sz=%u\n", (unsigned)lba, (unsigned)sz);
        return 1;
    }
    if (iso_lookup("disc0:/psp_game/sysdir/eboot.bin", &lba, &sz) != 0 || lba != 37 || sz != 4096) {
        fprintf(stderr, "FAIL: case-insensitive lookup failed\n");
        return 1;
    }
    if (iso_lookup("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN;1", &lba, &sz) != 0 || lba != 37 || sz != 4096) {
        fprintf(stderr, "FAIL: lookup with ;1 failed\n");
        return 1;
    }
    if (iso_lookup("umd0:/PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) != 0 ||
        iso_lookup("umd:PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) != 0 ||
        iso_lookup("./PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) != 0 ||
        iso_lookup("PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) != 0) {
        fprintf(stderr, "FAIL: lookup with alternative prefix failed\n");
        return 1;
    }
    if (iso_lookup("disc0:/PSP_GAME/SYSDIR/MISSING.PRX", &lba, &sz) == 0) {
        fprintf(stderr, "FAIL: lookup missing file should fail\n");
        return 1;
    }
    if (iso_lookup("disc0:/PSP_GAME", &lba, &sz) == 0 ||
        iso_lookup("disc0:/PSP_GAME/", &lba, &sz) == 0 ||
        iso_lookup("disc0:/", &lba, &sz) == 0) {
        fprintf(stderr, "FAIL: directory lookup should fail\n");
        return 1;
    }
    if (iso_lookup("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN/", &lba, &sz) == 0) {
        fprintf(stderr, "FAIL: trailing slash on file lookup should fail\n");
        return 1;
    }
    if (iso_lookup("disc0:/PSP_GAME/../PSP_GAME/SYSDIR/EBOOT.BIN", &lba, &sz) == 0) {
        fprintf(stderr, "FAIL: path traversal should fail\n");
        return 1;
    }

    /* Test 5: iso_physical_lba */
    if (iso_physical_lba(37) != 37 || iso_physical_lba(1023) != 1023) {
        fprintf(stderr, "FAIL: iso_physical_lba mismatch\n");
        return 1;
    }

    /* Test 6: iso_read & seek & EOF & partial read */
    uint8_t read_buf[4096];
    int got = iso_read(37, 0, read_buf, 4096);
    if (got != 4096) {
        fprintf(stderr, "FAIL: iso_read got %d, expected 4096\n", got);
        return 1;
    }
    for (int i = 0; i < 4096; i++) {
        if (read_buf[i] != 0xAA) {
            fprintf(stderr, "FAIL: iso_read byte mismatch at %d\n", i);
            return 1;
        }
    }
    got = iso_read(37, 2048, read_buf, 512);
    if (got != 512 || read_buf[0] != 0xAA || read_buf[511] != 0xAA) {
        fprintf(stderr, "FAIL: offset read failed\n");
        return 1;
    }
    if (iso_read(37, 0, read_buf, 0) != 0) {
        fprintf(stderr, "FAIL: zero length read should return 0\n");
        return 1;
    }
    got = iso_read(1024, 0, read_buf, 100);
    if (got != 0) {
        fprintf(stderr, "FAIL: read at/past EOF should return 0, got %d\n", got);
        return 1;
    }
    got = iso_read(1023, 2000, read_buf, 100);
    if (got != 48) {
        fprintf(stderr, "FAIL: partial read at EOF should return 48, got %d\n", got);
        return 1;
    }
    for (int i = 0; i < 48; i++) {
        if (read_buf[i] != 0x77) {
            fprintf(stderr, "FAIL: partial read content mismatch at %d\n", i);
            return 1;
        }
    }

    /* Test 7: iso_list */
    IsoDirEntry de;
    int r = iso_list("disc0:/", 0, &de);
    if (r != 1 || strcmp(de.name, "PSP_GAME") != 0 || !de.is_dir || de.lba != 34) {
        fprintf(stderr, "FAIL: root iso_list index 0: r=%d name='%s' is_dir=%d lba=%u\n",
                r, de.name, de.is_dir, (unsigned)de.lba);
        return 1;
    }
    if (iso_list("disc0:/", 1, &de) != 0) {
        fprintf(stderr, "FAIL: root iso_list index 1 should return 0 (EOF)\n");
        return 1;
    }
    if (iso_list("disc0:/PSP_GAME", 0, &de) != 1 || strcmp(de.name, "PARAM.SFO") != 0 || de.is_dir || de.size != 256 || de.lba != 32) {
        fprintf(stderr, "FAIL: PSP_GAME entry 0 mismatch\n");
        return 1;
    }
    if (iso_list("disc0:/PSP_GAME", 1, &de) != 1 || strcmp(de.name, "SYSDIR") != 0 || !de.is_dir || de.lba != 35) {
        fprintf(stderr, "FAIL: PSP_GAME entry 1 mismatch\n");
        return 1;
    }
    if (iso_list("disc0:/PSP_GAME", 2, &de) != 1 || strcmp(de.name, "USRDIR") != 0 || !de.is_dir || de.lba != 36) {
        fprintf(stderr, "FAIL: PSP_GAME entry 2 mismatch\n");
        return 1;
    }
    if (iso_list("disc0:/PSP_GAME", 3, &de) != 0) {
        fprintf(stderr, "FAIL: PSP_GAME entry 3 should return 0\n");
        return 1;
    }
    if (iso_list("disc0:/PSP_GAME/PARAM.SFO", 0, &de) != -1) {
        fprintf(stderr, "FAIL: iso_list on regular file should return -1\n");
        return 1;
    }
    if (iso_list("disc0:/NONEXISTENT_DIR", 0, &de) != -1) {
        fprintf(stderr, "FAIL: iso_list on nonexistent dir should return -1\n");
        return 1;
    }

    /* Test 8: Multithreaded concurrent reads */
    if (run_concurrency() != 0) {
        fprintf(stderr, "FAIL: multithreaded concurrent read test failed\n");
        return 1;
    }

    printf("iso_vfs_test: OK\n");
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestPublicIsoVfsC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="isovfsc_")
        cls.iso_path = Path(cls.tmp) / "vfs_test.iso"
        create_synthetic_vfs_iso(cls.iso_path)

        cls.test_c = Path(cls.tmp) / "iso_test_harness.c"
        cls.test_c.write_text(ISO_HARNESS_C, encoding="utf-8")

        cls.exe = os.path.join(cls.tmp, "iso_vfs_test.exe")
        cmd = [
            CC,
            "-std=c11",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'src' / 'rt'}",
            "-o",
            cls.exe,
            str(cls.test_c),
            str(ROOT / "src" / "rt" / "iso_public.c"),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise AssertionError("iso_test_harness did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_public_iso_vfs_contract(self):
        result = subprocess.run([self.exe, str(self.iso_path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("iso_vfs_test: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
