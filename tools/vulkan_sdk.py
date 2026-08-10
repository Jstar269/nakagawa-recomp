#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Vulkan SDK validation and discovery shared by the workspace doctor."""

from __future__ import annotations

from pathlib import Path
import os
import re


class VulkanSdkError(RuntimeError):
    """Raised when an explicitly configured or discoverable SDK is unusable."""


_VERSION_RE = re.compile(r"^\d+(?:\.\d+)+$")


def _resolve(path: Path) -> Path:
    try:
        return path.expanduser().resolve(strict=True)
    except OSError:
        return path.expanduser()


def is_usable_vulkan_sdk(path: Path) -> bool:
    """Return whether *path* contains the header and import library the build needs."""

    root = _resolve(path)
    headers = (
        root / "Include" / "vulkan" / "vulkan.h",
        root / "include" / "vulkan" / "vulkan.h",
    )
    libraries = (
        root / "Lib" / "vulkan-1.lib",
        root / "lib" / "vulkan-1.lib",
    )
    return any(candidate.is_file() for candidate in headers) and any(candidate.is_file() for candidate in libraries)


def _version_key(name: str) -> tuple[int, ...] | None:
    if not _VERSION_RE.fullmatch(name):
        return None
    return tuple(int(part) for part in name.split("."))


def discover_vulkan_sdk(
    explicit: Path | str | None = None,
    *,
    environment: str | None = None,
    install_root: Path | str = r"C:\VulkanSDK",
) -> Path:
    """Resolve an SDK in explicit, environment, then newest-valid-install order."""

    explicit_value = str(explicit) if explicit is not None else ""
    if explicit_value.strip():
        candidate = _resolve(Path(explicit_value))
        if is_usable_vulkan_sdk(candidate):
            return candidate
        raise VulkanSdkError(
            f"Explicit -vulkan-sdk path is not usable: {explicit_value}. "
            "It must contain Include/vulkan/vulkan.h and Lib/vulkan-1.lib."
        )

    environment_value = os.environ.get("VULKAN_SDK") if environment is None else environment
    if environment_value and environment_value.strip():
        candidate = _resolve(Path(environment_value))
        if is_usable_vulkan_sdk(candidate):
            return candidate
        raise VulkanSdkError(
            f"VULKAN_SDK points to an unusable Vulkan SDK: {environment_value}. "
            "Clear it, correct it, or pass --vulkan-sdk with a current SDK."
        )

    root = _resolve(Path(install_root))
    candidates: list[tuple[tuple[int, ...], Path]] = []
    try:
        directories = (entry for entry in root.iterdir() if entry.is_dir())
    except OSError:
        directories = ()
    for directory in directories:
        version = _version_key(directory.name)
        if version is not None:
            candidates.append((version, directory))

    for _version, candidate in sorted(candidates, key=lambda item: item[0], reverse=True):
        if is_usable_vulkan_sdk(candidate):
            return _resolve(candidate)

    raise VulkanSdkError(
        "No usable Vulkan SDK found. Pass --vulkan-sdk <path> or set VULKAN_SDK to a current "
        "SDK containing Include/vulkan/vulkan.h and Lib/vulkan-1.lib."
    )
