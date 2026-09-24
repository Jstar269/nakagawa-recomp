#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Record observed live toolchain identities using nk_doctor detection functions."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
from typing import Any

# Ensure tools/ is on sys.path
TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import nk_doctor_checks
import nk_doctor_core
import vulkan_sdk


def _detect_vulkan_version(vk_path: Path) -> str:
    """Determine Vulkan SDK version from directory name or header."""
    if re.match(r"^\d+(?:\.\d+)+$", vk_path.name):
        return vk_path.name
    header_candidates = (
        vk_path / "Include" / "vulkan" / "vulkan_core.h",
        vk_path / "include" / "vulkan" / "vulkan_core.h",
    )
    for header in header_candidates:
        if header.is_file():
            text = header.read_text(encoding="utf-8", errors="replace")
            m_comp = re.search(
                r"#define\s+VK_HEADER_VERSION_COMPLETE\s+VK_MAKE_API_VERSION\(\s*0\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*VK_HEADER_VERSION\s*\)",
                text,
            )
            m_patch = re.search(r"#define\s+VK_HEADER_VERSION\s+(\d+)", text)
            if m_comp and m_patch:
                return f"{m_comp.group(1)}.{m_comp.group(2)}.{m_patch.group(1)}"
            m_ver = re.search(r"#define\s+VK_API_VERSION_1_(\d+)", text)
            if m_ver:
                patch = m_patch.group(1) if m_patch else "0"
                return f"1.{m_ver.group(1)}.{patch}"
    return "unknown"


def record_observed_toolchain(
    msys_path: Path | str | None = None,
    vulkan_sdk_path: Path | str | None = None,
    sdl3_path: Path | str | None = None,
) -> dict[str, Any]:
    """Inspect the live environment using Doctor detection and return observed versions."""
    resolved_msys: Path
    if msys_path is not None:
        resolved_msys = Path(msys_path).resolve()
    else:
        env_msys = os.environ.get("MSYS_PATH")
        if env_msys:
            resolved_msys = Path(env_msys).resolve()
        else:
            resolved_msys = Path(r"C:\msys64\ucrt64\bin")

    # GCC compiler detection
    gcc = nk_doctor_core._find_executable(("gcc.exe", "gcc"), resolved_msys)
    if not gcc:
        raise RuntimeError(f"GCC compiler not found in MSYS2 path: {resolved_msys}")
    rc, gcc_line = nk_doctor_core._run_version([str(gcc), "--version"])
    if rc != 0:
        raise RuntimeError(f"Failed to query gcc version: {gcc_line}")
    m_gcc = re.search(r"\b(\d+\.\d+(?:\.\d+)*)\b", gcc_line)
    gcc_version = m_gcc.group(1) if m_gcc else gcc_line

    # Make tool detection
    make = nk_doctor_core._find_executable(
        ("mingw32-make.exe", "mingw32-make", "make.exe", "make"), resolved_msys
    )
    if not make:
        raise RuntimeError(f"Make not found in MSYS2 path: {resolved_msys}")
    rc, make_line = nk_doctor_core._run_version([str(make), "--version"])
    if rc != 0:
        raise RuntimeError(f"Failed to query make version: {make_line}")
    m_make = re.search(r"\b(\d+\.\d+(?:\.\d+)*)\b", make_line)
    make_version = m_make.group(1) if m_make else make_line

    # Python version
    python_version = sys.version.split()[0]

    # SDL3 provider detection
    sdl_provider = nk_doctor_checks.discover_sdl3_provider(
        explicit=sdl3_path,
        msys_path=resolved_msys,
        vulkan_sdk=vulkan_sdk_path,
    )
    if not sdl_provider.version or sdl_provider.version == "unknown":
        raise RuntimeError("Failed to extract SDL3 version from provider headers")

    # Vulkan SDK detection
    vk_root = vulkan_sdk.discover_vulkan_sdk(explicit=vulkan_sdk_path)
    vk_version = _detect_vulkan_version(vk_root)
    if vk_version == "unknown":
        raise RuntimeError(f"Failed to determine Vulkan SDK version at {vk_root}")

    return {
        "compiler": {
            "version": gcc_version,
            "path": str(gcc),
            "details": gcc_line,
        },
        "make": {
            "version": make_version,
            "path": str(make),
            "details": make_line,
        },
        "python": {
            "version": python_version,
            "path": sys.executable,
            "details": sys.version,
        },
        "sdl3": {
            "version": sdl_provider.version,
            "provider": sdl_provider.provider,
            "root_dir": str(sdl_provider.root_dir),
            "import_lib": str(sdl_provider.import_lib),
            "runtime_dll": str(sdl_provider.runtime_dll) if sdl_provider.runtime_dll else None,
        },
        "vulkan_sdk": {
            "version": vk_version,
            "path": str(vk_root),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Optional file path to write observed toolchain JSON to",
    )
    parser.add_argument(
        "--msys-path",
        type=Path,
        default=None,
        help="MSYS2 UCRT64 bin directory",
    )
    parser.add_argument(
        "--vulkan-sdk",
        type=Path,
        default=None,
        help="Explicit Vulkan SDK directory",
    )
    parser.add_argument(
        "--sdl3-path",
        type=Path,
        default=None,
        help="Explicit SDL3 directory",
    )

    args = parser.parse_args(argv)
    try:
        observed = record_observed_toolchain(
            msys_path=args.msys_path,
            vulkan_sdk_path=args.vulkan_sdk,
            sdl3_path=args.sdl3_path,
        )
    except Exception as exc:
        print(f"Error recording toolchain: {exc}", file=sys.stderr)
        return 1

    payload = json.dumps(observed, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
