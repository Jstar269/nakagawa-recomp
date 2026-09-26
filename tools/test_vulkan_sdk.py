#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

from __future__ import annotations

from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from vulkan_sdk import VulkanSdkError, discover_vulkan_sdk, is_usable_vulkan_sdk


class VulkanSdkDiscoveryTests(unittest.TestCase):
    def make_sdk(self, root: Path, name: str, *, complete: bool = True) -> Path:
        sdk = root / name
        (sdk / "Include" / "vulkan").mkdir(parents=True)
        (sdk / "Include" / "vulkan" / "vulkan.h").write_text("// synthetic header\n", encoding="ascii")
        if complete:
            (sdk / "Lib").mkdir()
            (sdk / "Lib" / "vulkan-1.lib").write_bytes(b"synthetic import library")
        return sdk

    def test_explicit_override_wins_over_environment_and_scan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            explicit = self.make_sdk(root, "explicit")
            environment = self.make_sdk(root, "environment")
            self.make_sdk(root, "1.9.0.0")
            self.assertEqual(
                discover_vulkan_sdk(explicit, environment=str(environment), install_root=root),
                explicit.resolve(),
            )

    def test_environment_wins_over_scan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            environment = self.make_sdk(root, "environment")
            self.make_sdk(root, "9.0.0.0")
            self.assertEqual(
                discover_vulkan_sdk(environment=str(environment), install_root=root),
                environment.resolve(),
            )

    def test_scan_uses_newest_valid_numeric_version(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_sdk(root, "1.10.0.0", complete=False)
            self.make_sdk(root, "1.9.0.0")
            self.make_sdk(root, "not-a-version")
            self.make_sdk(root, "1.8.0.0")
            self.assertEqual(
                discover_vulkan_sdk(environment="", install_root=root),
                (root / "1.9.0.0").resolve(),
            )

    def test_incomplete_and_malformed_installations_are_not_usable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            incomplete = self.make_sdk(root, "1.10.0.0", complete=False)
            self.make_sdk(root, "garbage")
            self.assertFalse(is_usable_vulkan_sdk(incomplete))
            with self.assertRaisesRegex(VulkanSdkError, "No usable Vulkan SDK"):
                discover_vulkan_sdk(environment="", install_root=root)

    def test_msys2_ucrt64_vulkan_import_archive_is_usable(self) -> None:
        """The pinned UCRT64 loader package provides a GCC import archive, not .lib."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "ucrt64"
            (root / "include" / "vulkan").mkdir(parents=True)
            (root / "include" / "vulkan" / "vulkan.h").write_text("// fixture\n", encoding="ascii")
            (root / "lib").mkdir()
            (root / "lib" / "libvulkan-1.dll.a").write_bytes(b"synthetic import archive")
            self.assertTrue(is_usable_vulkan_sdk(root))
            self.assertEqual(discover_vulkan_sdk(root), root.resolve())

    def test_invalid_environment_fails_with_actionable_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(VulkanSdkError, "VULKAN_SDK points to an unusable"):
                discover_vulkan_sdk(environment=str(Path(tmp) / "missing"), install_root=tmp)

    def test_missing_install_root_yields_no_candidates_without_traceback(self) -> None:
        """A missing scan root must not raise FileNotFoundError.

        The scan iterates the root directory inside a guarded try/except OSError.
        Before the fix the generator was constructed inside the guard and
        iterated outside it, so on a non-existent root the FileNotFoundError
        escaped as a traceback while the caller still saw exit 0. The scan must
        instead degrade to zero candidates and raise the normal VulkanSdkError.
        """
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "does-not-exist"
            self.assertFalse(missing.is_dir())
            with self.assertRaises(VulkanSdkError) as ctx:
                discover_vulkan_sdk(environment="", install_root=missing)
            self.assertNotIsInstance(ctx.exception.__cause__, FileNotFoundError)
            self.assertNotIsInstance(ctx.exception.__cause__, OSError)
            self.assertIn("No usable Vulkan SDK", str(ctx.exception))


class VulkanSdkMakefileWiringTests(unittest.TestCase):
    """The player target must consume the shared VULKAN_SDK resolution, not a
    machine-pinned install path (TD-33).

    Discovery order is explicit override, then VULKAN_SDK, then the newest
    usable install under C:\\VulkanSDK. The Makefile resolves once into
    VULKAN_SDK and every consumer -- CFLAGS, LDFLAGS, and the player's
    PLAYER_VULKAN_INC/LIB -- must derive from that one variable. When nothing
    usable resolved, the player target must fail with a message naming
    VULKAN_SDK rather than a hardcoded fallback or a confusing compiler error.
    """

    @staticmethod
    def make_sdk(root: Path, name: str) -> Path:
        sdk = root / name
        (sdk / "Include" / "vulkan").mkdir(parents=True)
        (sdk / "Include" / "vulkan" / "vulkan.h").write_text("x", encoding="ascii")
        (sdk / "Lib").mkdir()
        (sdk / "Lib" / "vulkan-1.lib").write_bytes(b"x")
        return sdk

    def test_player_has_no_machine_pinned_sdk_path(self) -> None:
        """No *compiler flag* may name a machine's SDK install.

        The fail-closed error hint is allowed to mention the default
        discovery root C:/VulkanSDK in prose; what is forbidden is a
        PLAYER_VULKAN_INC/LIB (or CFLAGS/LDFLAGS) value that points the
        compiler at a pinned version directory.
        """
        makefile = (Path(__file__).resolve().parents[1] / "Makefile").read_text(encoding="utf-8")
        for line in makefile.splitlines():
            text = line.strip()
            if text.startswith("-I") or text.startswith("-L"):
                self.assertNotIn("VulkanSDK/1.", text, line)
                self.assertNotRegex(text, r"VulkanSDK/(\d+\.){2,}\d+", line)

    def test_player_flags_derive_from_vulkan_sdk(self) -> None:
        # The Vulkan include/library group is platform-conditional: on Windows
        # it is spelled out as -I$(VULKAN_SDK)/Include, on Linux it is the
        # VULKAN_INC_FLAGS group (empty by default, -I$(VULKAN_SDK)/include
        # only when the caller names an explicit VULKAN_SDK). The invariant
        # under test is that the group derives from the shared VULKAN_SDK
        # resolution rather than a machine-pinned version directory, so the
        # assertion accepts both forms and rejects a literal version path.
        makefile = (Path(__file__).resolve().parents[1] / "Makefile").read_text(encoding="utf-8")
        lines = makefile.splitlines()
        for line in lines:
            if re.match(r"\s*PLAYER_VULKAN_(INC|LIB)\s+:?=", line):
                value = line.split("=", 1)[1].strip()
                self.assertTrue(
                    value == ""
                    or "$(VULKAN_SDK)" in line
                    or "$(VULKAN_INC_FLAGS)" in line
                    or "$(VULKAN_LIB_FLAGS)" in line
                    or "VULKAN_ERROR_HINT" in line,
                    line,
                )
        includes = next(
            line for line in lines if line.startswith("PLAYER_INCLUDES :=")
        )
        self.assertIn("$(PLAYER_VULKAN_INC)", includes)

    def test_explicit_vulkan_sdk_override_reaches_the_player_compile_flags(self) -> None:
        # Deliberately a dry run (`make -n`). What is under test is that an explicit
        # VULKAN_SDK reaches the player's include and library flags -- not that a player
        # binary links, which additionally needs SDL3 and so fails on a bare CI runner for
        # reasons that have nothing to do with SDK resolution. Asserting on the printed
        # recipe tests the actual unit and is portable; skipping when SDL3 is absent would
        # leave this asserting nothing exactly where it matters.
        make = shutil.which("mingw32-make") or shutil.which("make")
        if not make:
            self.skipTest("GNU Make is required")
        with tempfile.TemporaryDirectory(prefix="td33-vk-override-") as tmp:
            sdk = self.make_sdk(Path(tmp), "9.9.9.9")
            env = dict(os.environ)
            env["VULKAN_SDK"] = str(sdk)
            msys_bin = os.environ.get("MSYS_PATH") or r"C:\msys64\ucrt64\bin"
            py_bin = str(Path(sys.executable).parent)
            env["PATH"] = os.pathsep.join(
                [py_bin, msys_bin, env.get("PATH", "")]
            )
            proc = subprocess.run(
                [make, "--no-print-directory", "-n", "CC=gcc", "player",
                 "PLAYER_EXE=build/td33-override-player.exe"],
                capture_output=True, text=True, env=env, check=False, cwd=str(ROOT),
            )
            blob = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, blob)
            root = str(sdk).replace("\\", "/")
            # On Windows the SDK supplies both the include and the import
            # library search path. On Linux the headers live under /usr/include
            # and only the library search path is emitted, so the -I half is
            # asserted only on Windows.
            self.assertIn(f"-L{root}", blob)
            if os.name == "nt":
                self.assertIn(f"-I{root}", blob)


if __name__ == "__main__":
    unittest.main()
