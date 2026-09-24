# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Unit tests for package third-party notice generation, licensing gates, and relink materials."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import package_notices
from title_codegen_plan import PackageRouteError


class TestPackageNotices(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.pkg_dir = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_packaging_fails_when_bundled_binary_lacks_license_record(self) -> None:
        """Gate: packaging fails with named error when any bundled binary lacks a license record."""
        # Create a synthetic package with a dummy unlicensed DLL
        (self.pkg_dir / "unlicensed_component.dll").write_bytes(b"MZ\0\0dummy")

        with self.assertRaises(PackageRouteError) as ctx:
            package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")
        self.assertIn("unlicensed_component.dll", str(ctx.exception))
        self.assertIn("lacks a license record", str(ctx.exception))

    def test_packaging_fails_when_bundled_unlicensed_dll_alongside_licensed_binary(self) -> None:
        """Gate: even if valid binaries exist, an extra unlicensed binary fails closed."""
        (self.pkg_dir / "game.exe").write_bytes(b"MZ\0\0dummy-exe")
        (self.pkg_dir / "SDL3.dll").write_bytes(b"MZ\0\0dummy-sdl3")
        (self.pkg_dir / "rogue.dll").write_bytes(b"MZ\0\0dummy-rogue")

        with self.assertRaises(PackageRouteError) as ctx:
            package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")
        self.assertIn("rogue.dll", str(ctx.exception))

    def test_packaging_fails_when_lgpl_text_missing(self) -> None:
        """Gate: fails with LICENSE_TEXT_MISSING when ATRAC3+ LGPL text is missing."""
        (self.pkg_dir / "game.exe").write_bytes(b"MZ\0\0dummy-exe")
        with tempfile.TemporaryDirectory() as empty_repo:
            fake_repo = Path(empty_repo)
            (fake_repo / "LICENSE").write_text("Project GPL", encoding="utf-8")
            (fake_repo / "src" / "rt" / "atrac3p").mkdir(parents=True)
            # Deliberately do not create LICENSE.LGPLv2.1.txt

            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.generate_package_notices(self.pkg_dir, repo_root=fake_repo)

            self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
            self.assertIn("LICENSE_TEXT_MISSING", str(ctx.exception))
            self.assertIn("FFmpeg ATRAC3+", str(ctx.exception))

    def test_notices_bundle_generation_with_synthetic_toolchain(self) -> None:
        """Complete bundle generation produces index.json, THIRD_PARTY_NOTICES.txt, RELINK.md, and licenses."""
        with tempfile.TemporaryDirectory() as tc_dir:
            toolchain_root = Path(tc_dir)
            # Set up mock toolchain license files
            sdl_lic = toolchain_root / "share" / "licenses" / "SDL3"
            sdl_lic.mkdir(parents=True)
            (sdl_lic / "LICENSE.txt").write_text("Zlib License for SDL3\nVerbatim notice text", encoding="utf-8")

            pthread_lic = toolchain_root / "share" / "licenses" / "winpthreads"
            pthread_lic.mkdir(parents=True)
            (pthread_lic / "COPYING").write_text("winpthreads license text", encoding="utf-8")

            # Synthetic package contents
            (self.pkg_dir / "mygame.exe").write_bytes(b"MZ\0\0exe")
            (self.pkg_dir / "SDL3.dll").write_bytes(b"MZ\0\0sdl")
            (self.pkg_dir / "libwinpthread-1.dll").write_bytes(b"MZ\0\0pthread")

            result = package_notices.generate_package_notices(
                self.pkg_dir,
                repo_root=ROOT,
                toolchain_root=toolchain_root,
            )

            # Check return structure
            self.assertEqual(result["schema_version"], 1)
            self.assertEqual(result["bundle_type"], "native_package_third_party_notices")
            comp_names = [c["name"] for c in result["components"]]
            self.assertIn("FFmpeg ATRAC3+ subset", comp_names)
            self.assertIn("SDL3", comp_names)
            self.assertIn("winpthreads", comp_names)

            # Check files created on disk
            notices_dir = self.pkg_dir / "THIRD_PARTY_NOTICES"
            self.assertTrue(notices_dir.is_dir())
            self.assertTrue((notices_dir / "index.json").is_file())
            self.assertTrue((notices_dir / "SDL3.txt").is_file())
            self.assertTrue((notices_dir / "FFmpeg-ATRAC3P.txt").is_file())
            self.assertTrue((notices_dir / "winpthreads.txt").is_file())
            self.assertTrue((notices_dir / "RELINK.md").is_file())

            self.assertTrue((self.pkg_dir / "RELINK.md").is_file())
            self.assertTrue((self.pkg_dir / "THIRD_PARTY_NOTICES.txt").is_file())

            # Check content of index.json
            index_json = json.loads((notices_dir / "index.json").read_text(encoding="utf-8"))
            for comp in index_json["components"]:
                self.assertNotIn(":", comp["source_path"])  # No drive letters / absolute paths
                self.assertNotIn("\\", comp["source_path"])  # POSIX forward slashes
                self.assertTrue((notices_dir / comp["license_file"]).is_file())

            # Check RELINK.md content
            relink_text = (self.pkg_dir / "RELINK.md").read_text(encoding="utf-8")
            self.assertIn("FFmpeg ATRAC3+", relink_text)
            self.assertIn("LGPL-2.1", relink_text)
            self.assertIn("runtime-objects", relink_text)
            self.assertNotIn("atrac3p-objects", relink_text)
            self.assertIn("compile", relink_text)
            self.assertIn("atrac3p_bridge.o", relink_text)
            self.assertIn("atrac3p_libavcodec/atrac3plus.o", relink_text)

            # Check THIRD_PARTY_NOTICES.txt contains verbatim license texts
            combined_text = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
            self.assertIn("Zlib License for SDL3", combined_text)
            self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", combined_text)
            self.assertIn("winpthreads license text", combined_text)

    def test_custom_license_map_resolves_custom_bundled_dll(self) -> None:
        """Custom license map allows explicit declaration of third-party DLLs."""
        custom_lic = self.pkg_dir / "custom_lic.txt"
        custom_lic.write_text("Custom Apache-2.0 License Text", encoding="utf-8")

        (self.pkg_dir / "custom_math.dll").write_bytes(b"MZ\0\0math")
        custom_map = {
            "custom_math.dll": {
                "name": "Custom Math Library",
                "spdx_id": "Apache-2.0",
                "license_path": str(custom_lic),
                "version": "1.0.0",
            }
        }

        result = package_notices.generate_package_notices(
            self.pkg_dir,
            repo_root=ROOT,
            custom_license_map=custom_map,
        )
        comp_names = [c["name"] for c in result["components"]]
        self.assertIn("Custom Math Library", comp_names)
        self.assertTrue((self.pkg_dir / "THIRD_PARTY_NOTICES" / "Custom_Math_Library.txt").is_file())

    def test_system_dll_detection(self) -> None:
        """Standard Windows system DLLs are recognized and excluded from package third-party bundles."""
        self.assertTrue(package_notices.is_system_dll("kernel32.dll"))
        self.assertTrue(package_notices.is_system_dll("KERNEL32.DLL"))
        self.assertTrue(package_notices.is_system_dll("user32.dll"))
        self.assertTrue(package_notices.is_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))
        self.assertTrue(package_notices.is_system_dll("ext-ms-win-gdi-draw-l1-1-0.dll"))
        self.assertTrue(package_notices.is_system_dll("vulkan-1.dll"))
        self.assertTrue(package_notices.is_system_dll("mfplat.dll"))

        self.assertFalse(package_notices.is_system_dll("SDL3.dll"))
        self.assertFalse(package_notices.is_system_dll("libwinpthread-1.dll"))
        self.assertFalse(package_notices.is_system_dll("libgcc_s_seh-1.dll"))
        self.assertFalse(package_notices.is_system_dll("custom.dll"))

    def test_relink_objects_follow_makefile_sources(self) -> None:
        """RELINK.md lists exactly the ATRAC3+ objects the Makefile builds, plus the bridge."""
        objects = package_notices.atrac3p_object_names(ROOT)
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertGreater(len(objects), 1)
        for obj in objects[:-1]:
            source = "src/rt/atrac3p/" + obj[len("$(BUILD_DIR)/atrac3p_"):-2] + ".c"
            self.assertIn(source, makefile)
        self.assertEqual(objects[-1], "$(BUILD_DIR)/atrac3p_bridge.o")

    def test_installed_package_version_reads_pacman_database(self) -> None:
        """Component versions come from the toolchain's package database, never a literal."""
        with tempfile.TemporaryDirectory() as msys_dir:
            toolchain_root = Path(msys_dir) / "ucrt64"
            toolchain_root.mkdir()
            local_db = Path(msys_dir) / "var" / "lib" / "pacman" / "local"
            entry = local_db / "mingw-w64-ucrt-x86_64-sdl3-9.8.7-1"
            entry.mkdir(parents=True)
            (entry / "desc").write_text(
                "%NAME%\nmingw-w64-ucrt-x86_64-sdl3\n\n%VERSION%\n9.8.7-1\n", encoding="utf-8"
            )
            self.assertEqual(package_notices.installed_package_version(toolchain_root, "sdl3"), "9.8.7-1")
            self.assertEqual(package_notices.installed_package_version(toolchain_root, "sdl3_ttf"), "unknown")
            self.assertEqual(package_notices.installed_package_version(None, "sdl3"), "unknown")

    def test_gcc_runtime_ships_gplv3_with_exception(self) -> None:
        """GCC runtime DLLs carry both the GPLv3 text and the runtime library exception."""
        with tempfile.TemporaryDirectory() as tc_dir:
            toolchain_root = Path(tc_dir)
            gcc_lic = toolchain_root / "share" / "licenses" / "gcc-libs"
            gcc_lic.mkdir(parents=True)
            (gcc_lic / "COPYING.RUNTIME").write_text("synthetic runtime exception text", encoding="utf-8")
            (self.pkg_dir / "libgcc_s_seh-1.dll").write_bytes(b"MZ\0\0gcc")
            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.generate_package_notices(
                    self.pkg_dir, repo_root=ROOT, toolchain_root=toolchain_root
                )
            self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
            (gcc_lic / "COPYING3").write_text("synthetic GPLv3 text", encoding="utf-8")
            result = package_notices.generate_package_notices(
                self.pkg_dir, repo_root=ROOT, toolchain_root=toolchain_root
            )
            gcc = next(c for c in result["components"] if c.get("binary") == "libgcc_s_seh-1.dll")
            self.assertEqual(len(gcc["additional_license_files"]), 1)
            combined = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
            self.assertIn("synthetic runtime exception text", combined)
            self.assertIn("synthetic GPLv3 text", combined)

    def test_toolchain_root_requires_license_directory(self) -> None:
        """A gcc without packaged license texts is not accepted as the toolchain root."""
        with tempfile.TemporaryDirectory() as tc_dir:
            bare = Path(tc_dir) / "bare"
            (bare / "bin").mkdir(parents=True)
            (bare / "bin" / "gcc.exe").write_bytes(b"MZ")
            with mock.patch.dict(package_notices.os.environ, {"MINGW_PREFIX": str(bare)}), \
                    mock.patch.object(package_notices.shutil, "which", return_value=None):
                root = package_notices.resolve_toolchain_root()
                self.assertNotEqual(root, bare.resolve())
            (bare / "share" / "licenses").mkdir(parents=True)
            with mock.patch.dict(package_notices.os.environ, {"MINGW_PREFIX": str(bare)}):
                self.assertEqual(package_notices.resolve_toolchain_root(), bare.resolve())



if __name__ == "__main__":
    unittest.main()
