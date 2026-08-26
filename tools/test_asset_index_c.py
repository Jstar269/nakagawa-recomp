# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Host-neutral regression for the dynamic extracted-asset index (issue #223)."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "asset_index_selftest.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestAssetIndexSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="asset_index_")
        cls.exe = os.path.join(cls.tmp, "asset_index_selftest.exe")
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
            raise AssertionError("asset_index_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_short_and_long_host_paths_have_identical_lookup(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("asset index selftest: OK", result.stdout)

    def test_synthetic_filesystem_tree_crosses_legacy_limits(self):
        """The same source-owned files enumerate under short and >512-byte roots.

        Windows receives an explicit ``\\\\?\\`` prefix so this is a real
        wide-path filesystem check rather than a test that silently skips the
        platform whose path handling is being repaired.
        """
        with tempfile.TemporaryDirectory(prefix="asset_tree_") as tmp:
            base = Path(tmp)
            if os.name == "nt":
                base = Path("\\\\?\\" + os.path.abspath(os.fspath(base)))
                self.assertTrue(os.fspath(base).startswith("\\\\?\\"))
            short_root = base / "short"
            long_root = base / ("long_" + "x" * 32)
            relative = Path("locale") / "archive.xb.d" / "data" / "menu" / "text"
            for root in (short_root, long_root):
                target = root / relative
                target.mkdir(parents=True)
                (target / "common.to").write_bytes(b"common")
                (target / "other.to").write_bytes(b"other")
            # Add enough nested components to cross both historical limits while
            # keeping every individual component legal on common filesystems.
            deep_relative = Path()
            for i in range(28):
                deep_relative = deep_relative / (f"segment_{i:02d}_" + "y" * 16)
            deep = long_root / deep_relative
            deep.mkdir(parents=True)
            (deep / "deep.to").write_bytes(b"deep")
            short_file = short_root / relative / "common.to"
            long_file = long_root / deep_relative / "deep.to"
            self.assertLess(len(os.fspath(short_file)), 260)
            self.assertGreater(len(os.fspath(long_file)), 512)
            if os.name == "nt":
                # The production PGF/data paths use _wfopen.  Exercise the
                # same UCRT entry point against the >512-character fixture.
                import ctypes

                ucrt = ctypes.CDLL("ucrtbase.dll")
                wfopen = ucrt._wfopen
                wfopen.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
                wfopen.restype = ctypes.c_void_p
                fclose = ucrt.fclose
                fclose.argtypes = [ctypes.c_void_p]
                fclose.restype = ctypes.c_int
                stream = wfopen(os.fspath(long_file), "rb")
                self.assertTrue(stream)
                self.assertEqual(fclose(stream), 0)

            def inventory(root):
                return {
                    path.relative_to(root).as_posix(): path.read_bytes()
                    for path in root.rglob("*")
                    if path.is_file()
                }

            short_inventory = inventory(short_root)
            long_inventory = inventory(long_root)
            expected_common = {
                "locale/archive.xb.d/data/menu/text/common.to": b"common",
                "locale/archive.xb.d/data/menu/text/other.to": b"other",
            }
            self.assertEqual(short_inventory, expected_common)
            self.assertEqual(
                {key: value for key, value in long_inventory.items() if key in expected_common},
                expected_common,
            )
            self.assertEqual(long_inventory[(deep_relative / "deep.to").as_posix()], b"deep")

    def test_production_font_policy_has_distinct_configured_and_absent_roots(self):
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        self.assertIn("font_load: SR_FONTDIR is configured but is not a valid absolute path", hle)
        self.assertIn("font_load: no SR_FONTDIR and executable font root could not be resolved", hle)
        self.assertIn("pgf_open_w", hle)
        self.assertNotIn('pgf_open("font/', hle)

    def test_production_data_root_is_wide_and_executable_anchored(self):
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        self.assertIn("sr_wide_module_data_root(&root_wide)", hle)
        self.assertIn("host_data: SR_DATAROOT is configured but is not a valid absolute path", hle)
        self.assertIn("host_data: executable-relative data root could not be resolved", hle)
        self.assertIn("sr_wide_env_alloc(L\"SR_DATAROOT\"", hle)
        self.assertIn("sr_wide_configured_root_wide_alloc(configured_root", hle)
        self.assertIn("sr_utf8_env_alloc(L\"SR_FSDIR\"", hle)
        self.assertIn("FindFirstFileW", hle)
        self.assertIn("FindNextFileW", hle)
        self.assertIn("data_walk_push", hle)
        self.assertIn("data_root_validate", hle)
        self.assertIn("sr_stream_size_u32", hle)
        self.assertIn("CreateFileW", hle)
        self.assertIn("host_data: indexed file readability check failed", hle)
        self.assertIn("host_data: indexed asset open failed", hle)
        self.assertIn("GetFullPathNameW", hle)
        self.assertNotIn("data_walk(child_host", hle)
        self.assertNotIn("FindFirstFileA", hle)
        self.assertNotIn("FindNextFileA", hle)

    def test_guest_key_localization_guard_bounds_short_paths(self):
        """Localization parsing must prove the fixed-width prefix exists first.

        ``guest_cstr`` initializes only through the terminator; indexing the
        remainder of a short path would inspect uninitialized stack bytes.
        """
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        self.assertIn(
            "if (guest_key_length >= 8u && _strnicmp(p, \"data_\", 5) == 0",
            hle,
        )

    def test_cold_census_preparation_precedes_guest_execution(self):
        """The cold extracted-data census must be prepared BEFORE guest execution.

        Pins, by source shape (PR #108 reconstruction):
        1. driver.c calls sr_host_data_prepare() after entry validation and
           BEFORE gui_init / sched_init / any direct fn(&s) invocation;
        2. host_data_lookup consumes TERMINAL route states only -- it must not
           begin, resume, or await a census (no lazy construction), so a guest
           HLE lookup can never start the cold walk on the scheduler thread;
        3. the historical lazy trigger inside host_data_lookup stays gone.
        """
        driver = (ROOT / "src" / "rt" / "driver.c").read_text(encoding="utf-8")
        prepare_idx = driver.find("sr_host_data_prepare()")
        self.assertNotEqual(prepare_idx, -1,
                            "driver.c must call sr_host_data_prepare()")
        gui_idx = driver.find("gui_init(SR_APP_TITLE)")
        sched_idx = driver.find("sched_init(&s)")
        fn_idx = driver.find("fn(&s);")
        boot_begin = driver.find("BOOT_EVENT phase=index_prepare_begin")
        self.assertLess(boot_begin, prepare_idx,
                        "preparation is announced before it runs")
        for name, idx in (("gui_init", gui_idx), ("sched_init", sched_idx), ("fn(&s)", fn_idx)):
            self.assertNotEqual(idx, -1, f"driver.c anchor {name} missing")
            self.assertLess(prepare_idx, idx,
                            f"preparation must precede {name} (guest execution seam)")

        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        lookup_idx = hle.find("static const SrAssetIndexEntry *host_data_lookup")
        self.assertNotEqual(lookup_idx, -1)
        lookup_end = hle.find("\n}\n", lookup_idx)
        lookup_code = hle[lookup_idx:lookup_end]
        self.assertNotIn("sr_host_data_prepare(", lookup_code,
                         "host_data_lookup must never build or resume a census")
        self.assertNotIn("data_walk(", lookup_code,
                         "host_data_lookup must never enumerate")
        self.assertIn("SR_DATA_STATE_READY", lookup_code,
                      "host_data_lookup consumes terminal state only")
        self.assertIn("lookup refused before preparation reached a",
                      hle,
                      "a non-terminal observation fails closed with one bounded diagnostic")


if __name__ == "__main__":
    unittest.main()
