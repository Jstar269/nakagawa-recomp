#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

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

    def test_invalid_environment_fails_with_actionable_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(VulkanSdkError, "VULKAN_SDK points to an unusable"):
                discover_vulkan_sdk(environment=str(Path(tmp) / "missing"), install_root=tmp)


if __name__ == "__main__":
    unittest.main()
