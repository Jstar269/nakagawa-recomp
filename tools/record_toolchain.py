#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Record observed live toolchain identities using nk_doctor detection functions."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

# Ensure tools/ is on sys.path
TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import nk_doctor_checks
import nk_doctor_core
import vulkan_sdk


def _sha256_file(path: Path | None) -> str | None:
    """Return a file's SHA-256 when it is a readable regular file."""
    if path is None or not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _git_output(repo_root: Path, *args: str) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=repo_root, capture_output=True, text=True, check=False,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        raise RuntimeError(f"Cannot record candidate Git identity ({' '.join(args)}): {detail}")
    return proc.stdout.strip()


def _candidate_identity(repo_root: Path) -> dict[str, Any]:
    """Bind the observation to the checkout and release manifest it describes."""
    root = repo_root.resolve()
    git_root = Path(_git_output(root, "rev-parse", "--show-toplevel")).resolve()
    if git_root != root:
        raise RuntimeError(f"Toolchain recorder repository root mismatch: {git_root}")
    commit = _git_output(root, "rev-parse", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit):
        raise RuntimeError(f"Cannot record an exact candidate commit: {commit!r}")
    status = _git_output(root, "status", "--porcelain=v1", "--untracked-files=all")
    manifest_path = root / "assets" / "release_manifest.json"
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes)
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Cannot read release manifest for candidate identity: {exc}") from exc
    version = manifest.get("version") if isinstance(manifest, dict) else None
    if not isinstance(version, str) or not version.strip():
        raise RuntimeError("Release manifest has no non-empty root version")
    return {
        "commit": commit,
        "working_tree_clean": not bool(status),
        "manifest_version": version,
        "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
    }


def _record_optional_glslc(vulkan_root: Path) -> dict[str, Any]:
    """Record the optional shader compiler when present; it is not a normal build input."""
    glslc = nk_doctor_core._find_executable(("glslc.exe", "glslc"), vulkan_root / "Bin")
    if not glslc:
        return {"available": False, "required_for_standard_release_build": False}
    rc, details = nk_doctor_core._run_version([str(glslc), "--version"])
    if rc != 0:
        raise RuntimeError(f"Failed to query glslc version: {details}")
    return {
        "available": True,
        "required_for_standard_release_build": False,
        "path": str(glslc),
        "details": details,
        "sha256": _sha256_file(glslc),
    }


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
    repo_root: Path | str | None = None,
    require_clean_tree: bool = False,
) -> dict[str, Any]:
    """Inspect the live environment and bind identities to the current source checkout."""
    root = Path(repo_root) if repo_root is not None else TOOLS_DIR.parent
    candidate = _candidate_identity(root)
    if require_clean_tree and not candidate["working_tree_clean"]:
        raise RuntimeError(
            f"Candidate checkout {candidate['commit']} has tracked or untracked changes"
        )
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

    # The Windows native player and its supporting selftests also compile C++.
    cxx = nk_doctor_core._find_executable(("g++.exe", "g++"), resolved_msys)
    if not cxx:
        raise RuntimeError(f"G++ compiler not found in MSYS2 path: {resolved_msys}")
    rc, cxx_line = nk_doctor_core._run_version([str(cxx), "--version"])
    if rc != 0:
        raise RuntimeError(f"Failed to query g++ version: {cxx_line}")
    m_cxx = re.search(r"\b(\d+\.\d+(?:\.\d+)*)\b", cxx_line)
    cxx_version = m_cxx.group(1) if m_cxx else cxx_line

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
        "candidate": candidate,
        "compiler": {
            "version": gcc_version,
            "path": str(gcc),
            "details": gcc_line,
            "sha256": _sha256_file(gcc),
            "cxx": {
                "version": cxx_version,
                "path": str(cxx),
                "details": cxx_line,
                "sha256": _sha256_file(cxx),
            },
        },
        "make": {
            "version": make_version,
            "path": str(make),
            "details": make_line,
            "sha256": _sha256_file(make),
        },
        "python": {
            "version": python_version,
            "path": sys.executable,
            "details": sys.version,
            "sha256": _sha256_file(Path(sys.executable)),
        },
        "sdl3": {
            "version": sdl_provider.version,
            "provider": sdl_provider.provider,
            "root_dir": str(sdl_provider.root_dir),
            "import_lib": str(sdl_provider.import_lib),
            "import_lib_sha256": _sha256_file(sdl_provider.import_lib),
            "runtime_dll": str(sdl_provider.runtime_dll) if sdl_provider.runtime_dll else None,
            "runtime_dll_sha256": _sha256_file(sdl_provider.runtime_dll),
        },
        "vulkan_sdk": {
            "version": vk_version,
            "path": str(vk_root),
        },
        "shader_compiler": _record_optional_glslc(vk_root),
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
    parser.add_argument(
        "--require-clean-tree",
        action="store_true",
        help="Fail unless the candidate checkout has no tracked or untracked changes",
    )

    args = parser.parse_args(argv)
    try:
        observed = record_observed_toolchain(
            msys_path=args.msys_path,
            vulkan_sdk_path=args.vulkan_sdk,
            sdl3_path=args.sdl3_path,
            require_clean_tree=args.require_clean_tree,
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
