#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Workspace, toolchain, input, runtime, and repository checks for hst_doctor."""

from __future__ import annotations

import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys

from hst_doctor_core import (
    EXPECTED_DISC_ID,
    EXPECTED_VFPU_FILES,
    PRIVATE_EXTENSIONS,
    PRIVATE_PREFIXES,
    Report,
    _bounded_nonempty_directory,
    _find_executable,
    _parse_elf,
    _parse_psp_header,
    _run_version,
    _scan_disc_id,
    _validate_iso,
    _validate_pe_x64,
)
from shader_embed import verify as verify_shader_provenance
from vulkan_sdk import VulkanSdkError, discover_vulkan_sdk


def _probe_powershell() -> tuple[Path | None, str | None, str | None, str | None]:
    executable = shutil.which("pwsh")
    if not executable:
        return None, None, None, "pwsh was not found on PATH"
    command = "$PSVersionTable | ConvertTo-Json -Compress"
    try:
        proc = subprocess.run(
            [executable, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True,
            text=True,
            check=False,
            timeout=8,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return Path(executable), None, None, str(exc)
    if proc.returncode != 0:
        return Path(executable), None, None, (proc.stderr or proc.stdout).strip() or f"exit {proc.returncode}"
    try:
        payload = json.loads(proc.stdout)
        version = payload["PSVersion"]
        version_text = ".".join(
            str(version[key])
            for key in ("Major", "Minor", "Patch", "PreReleaseLabel")
            if version.get(key) not in (None, "")
        )
        return Path(executable), str(payload.get("PSEdition", "")), version_text, None
    except (AttributeError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        return Path(executable), None, None, f"invalid pwsh version output: {exc}"


def check_powershell(report: Report) -> None:
    executable, edition, version_text, error = _probe_powershell()
    if error:
        report.fail(
            "POWERSHELL_VERSION",
            "PowerShell 7.6+ (`pwsh`) is required",
            path=executable,
            detail=error,
            remediation="Install the current PowerShell 7 LTS line and ensure `pwsh` is on PATH.",
        )
        return
    assert executable is not None and edition is not None and version_text is not None
    try:
        major, minor = (int(part) for part in version_text.split(".", 2)[:2])
    except ValueError:
        major, minor = -1, -1
    metadata = {"edition": edition, "version": version_text}
    if edition != "Core" or major != 7 or minor < 6:
        report.fail(
            "POWERSHELL_VERSION",
            "PowerShell 7.6+ (`pwsh`) is required",
            path=executable,
            detail=f"detected {edition or 'unknown'} {version_text or 'unknown'}",
            remediation="Install the current PowerShell 7 LTS line and invoke scripts with `pwsh`.",
            metadata=metadata,
        )
    else:
        report.pass_(
            "POWERSHELL_VERSION",
            "PowerShell 7.6+ is available",
            path=executable,
            detail=f"{edition} {version_text}",
            metadata=metadata,
        )


def _windows_version_info() -> tuple[int | None, int | None]:
    if os.name != "nt":
        return None, None
    try:
        version = sys.getwindowsversion()
        return int(version.build), int(version.product_type)
    except (AttributeError, OSError, TypeError, ValueError):
        return None, None


def check_platform(report: Report) -> None:
    is_windows = os.name == "nt"
    if is_windows:
        report.pass_(
            "HOST_WINDOWS",
            "Windows host detected",
            metadata={"platform": platform.platform(), "machine": platform.machine()},
        )
    else:
        report.fail(
            "HOST_WINDOWS",
            "The complete Nakagawa HST build/runtime is Windows-only",
            detail=platform.platform(),
            remediation="Run Build/Run diagnostics on Windows 11 x64.",
        )
    build, product_type = _windows_version_info()
    if is_windows and build is not None and product_type == 1 and build >= 22000:
        report.pass_(
            "HOST_WINDOWS_11",
            "Windows 11 is the supported host platform",
            detail=f"Windows build {build}, workstation product type {product_type}",
            metadata={"build": build, "product_type": product_type},
        )
    else:
        report.fail(
            "HOST_WINDOWS_11",
            "Windows 11 x64 is the supported host platform",
            detail=(
                f"Windows build {build}, product type {product_type}"
                if is_windows
                else platform.platform()
            ),
            remediation="Use a current Windows 11 x64 development machine; Windows 10 is not a supported/tested target.",
            metadata={"build": build, "product_type": product_type}
            if build is not None or product_type is not None
            else {},
        )
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        report.pass_("HOST_X64", "x86-64 host architecture detected", detail=platform.machine())
    else:
        report.fail(
            "HOST_X64",
            "Host architecture is not x86-64",
            detail=platform.machine(),
            remediation="Use a supported Windows x64 environment.",
        )
    if (3, 14) <= sys.version_info[:2] < (3, 15):
        report.pass_("PYTHON_VERSION", "Python version is supported", detail=sys.version.split()[0])
    else:
        report.fail(
            "PYTHON_VERSION",
            "Python 3.14.x is required",
            detail=sys.version.split()[0],
            remediation="Install CPython 3.14.x and make `python` resolve to it.",
        )
    check_powershell(report)


def _shader_provenance_errors(root: Path) -> list[str]:
    shader_root = root / "src" / "rt" / "gpu_sdl3vk"
    manifest = shader_root / "shader_manifest.json"
    try:
        return verify_shader_provenance(shader_root, manifest)
    except (AttributeError, OSError, TypeError, UnicodeError, ValueError) as exc:
        return [f"shader verification error: {exc}"]


def check_shader_provenance(report: Report, root: Path, vulkan_sdk: Path | None) -> None:
    errors = _shader_provenance_errors(root)
    if not errors:
        report.info("GLSLC", "glslc is not required; checked-in shader provenance is valid")
        return

    shader_root = root / "src" / "rt" / "gpu_sdl3vk"
    report.fail(
        "SHADER_PROVENANCE",
        "Checked-in shader provenance is invalid; regeneration is required",
        path=shader_root / "shader_manifest.json",
        detail="; ".join(errors),
        remediation="Run `python tools/shader_embed.py regenerate --glslc glslc`, then verify the checked-in shader embeddings.",
    )
    glslc_dir = vulkan_sdk / "Bin" if vulkan_sdk else None
    glslc = _find_executable(("glslc.exe", "glslc"), glslc_dir)
    if glslc:
        _rc, glslc_version = _run_version([str(glslc), "--version"])
        report.pass_("GLSLC", "glslc is available for shader regeneration", path=glslc, detail=glslc_version)
    else:
        report.fail(
            "GLSLC",
            "Shader regeneration is required but glslc is unavailable",
            remediation="Install glslc with the current Vulkan SDK or regenerate shader headers on a machine that has it.",
        )


def check_toolchain(report: Report, msys_path: Path, vulkan_sdk: Path | None, root: Path | None = None) -> None:
    root = root or report.root
    if "ucrt64" in str(msys_path).lower():
        report.pass_("MSYS2_UCRT64", "MSYS2 UCRT64 toolchain path selected", path=msys_path)
    else:
        report.fail(
            "MSYS2_UCRT64",
            "MSYS2 UCRT64 is required for the supported native build",
            path=msys_path,
            remediation="Install the current MSYS2 UCRT64 environment and pass its bin directory with --msys-path.",
        )
    tools = {
        "MAKE": ("mingw32-make.exe", "mingw32-make", "make.exe", "make"),
        "GCC": ("gcc.exe", "gcc"),
        "GXX": ("g++.exe", "g++"),
    }
    found: dict[str, Path] = {}
    for code, names in tools.items():
        executable = _find_executable(names, msys_path)
        if executable is None:
            report.fail(
                f"TOOL_{code}",
                f"Required tool {names[0]} was not found",
                remediation=(
                    "Install the MSYS2 UCRT64 gcc/make packages and pass the correct "
                    "-MsysPath/--msys-path when using a non-default installation."
                ),
            )
            continue
        found[code] = executable
        rc, version = _run_version([str(executable), "--version"])
        if rc == 0:
            report.pass_(
                f"TOOL_{code}",
                f"Resolved {code}",
                path=executable,
                detail=version,
            )
        else:
            report.fail(
                f"TOOL_{code}",
                f"Resolved {code} but it did not execute successfully",
                path=executable,
                detail=version,
            )
    for code in ("GCC", "GXX", "MAKE"):
        executable = found.get(code)
        if executable and "ucrt64" not in str(executable).lower():
            report.fail(
                f"TOOL_{code}_UCRT64",
                f"{code} was not resolved from a path containing `ucrt64`",
                path=executable,
                remediation="Confirm that this is the MSYS2 UCRT64 toolchain, not MSVCRT/MinGW32 or another installation.",
            )

    sdl_import_candidates = (
        msys_path.parent / "lib" / "libSDL3.dll.a",
        msys_path.parent / "lib" / "libSDL3.a",
    )
    sdl_import = next((path for path in sdl_import_candidates if path.is_file()), None)
    if sdl_import:
        report.pass_("SDL3_IMPORT", "SDL3 import library is available", path=sdl_import)
    else:
        report.fail(
            "SDL3_IMPORT",
            "SDL3 import library was not found in the UCRT64 prefix",
            remediation="Install mingw-w64-ucrt-x86_64-sdl3 in MSYS2 UCRT64.",
        )

    try:
        vulkan_sdk = discover_vulkan_sdk(explicit=vulkan_sdk)
    except VulkanSdkError as exc:
        report.fail(
            "VULKAN_SDK",
            "No usable Vulkan SDK was discovered",
            detail=str(exc),
            remediation="Pass --vulkan-sdk, set VULKAN_SDK, or install a current valid SDK under C:\\VulkanSDK.",
        )
        vulkan_sdk = None
    else:
        report.pass_("VULKAN_SDK", "Usable Vulkan SDK discovered", path=vulkan_sdk)

    vulkan_header = None
    if vulkan_sdk:
        vulkan_header_candidates = (
            vulkan_sdk / "Include" / "vulkan" / "vulkan.h",
            vulkan_sdk / "include" / "vulkan" / "vulkan.h",
        )
        vulkan_header = next((path for path in vulkan_header_candidates if path.is_file()), None)
        if vulkan_header:
            report.pass_("VULKAN_HEADERS", "Vulkan headers are available", path=vulkan_header)
        else:
            report.fail(
                "VULKAN_HEADERS",
                "Vulkan headers were not found at the configured SDK path",
                path=vulkan_sdk,
                remediation="Install the Vulkan SDK or pass -VulkanSdk/--vulkan-sdk with its actual location.",
            )
    vulkan_lib_candidates: tuple[Path, ...] = ()
    if vulkan_sdk:
        vulkan_lib_candidates = (
            vulkan_sdk / "Lib" / "vulkan-1.lib",
            vulkan_sdk / "lib" / "vulkan-1.lib",
        )
    vulkan_lib_candidates += (msys_path.parent / "lib" / "libvulkan-1.dll.a",)
    vulkan_lib = next((path for path in vulkan_lib_candidates if path.is_file()), None)
    if vulkan_lib:
        report.pass_("VULKAN_IMPORT", "Vulkan loader import library is available", path=vulkan_lib)
    else:
        report.fail(
            "VULKAN_IMPORT",
            "Vulkan loader import library was not found",
            remediation="Install the Vulkan SDK or the MSYS2 UCRT64 Vulkan loader package.",
        )

    check_shader_provenance(report, root, vulkan_sdk)


def _check_elf_file(report: Report, code: str, path: Path, description: str) -> dict[str, int] | None:
    if not path.is_file():
        report.fail(code, f"Missing {description}", path=path)
        return None
    metadata, error = _parse_elf(path)
    if error:
        report.fail(
            code,
            f"Invalid {description}",
            path=path,
            detail=error,
            remediation="Supply a decrypted 32-bit little-endian MIPS ELF produced from your own lawful game copy.",
        )
        return None
    assert metadata is not None
    report.pass_(code, f"Validated {description}", path=path, metadata=metadata)
    return metadata


def discover_iso(root: Path) -> tuple[Path | None, list[Path]]:
    legacy = root / "game.iso"
    candidates: list[Path] = []
    if legacy.is_file():
        candidates.append(legacy)
    iso_dir = root / "place_game_here" / "ISO"
    if iso_dir.is_dir():
        candidates.extend(sorted(path for path in iso_dir.iterdir() if path.is_file() and path.suffix.lower() == ".iso"))
    unique: list[Path] = []
    seen: set[Path] = set()
    for path in candidates:
        try:
            key = path.resolve()
        except OSError:
            key = path.absolute()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    if len(unique) == 1:
        return unique[0], unique
    return None, unique


def check_private_inputs(report: Report, *, need_iso: bool, need_assets: bool) -> None:
    root = report.root
    elf_path = root / "place_game_here" / "EBOOT.elf"
    if not elf_path.is_file() and (root / "eboot.elf").is_file():
        elf_path = root / "eboot.elf"
    elf_meta = _check_elf_file(report, "INPUT_EBOOT_ELF", elf_path, "decrypted EBOOT ELF")

    psp_header_path = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN"
    psp_meta: dict[str, object] | None = None
    if psp_header_path.is_file():
        psp_meta, error = _parse_psp_header(psp_header_path)
        if error:
            report.fail(
                "INPUT_EBOOT_BIN",
                "Invalid original EBOOT.BIN PSP header",
                path=psp_header_path,
                detail=error,
                remediation="Re-extract PSP_GAME/SYSDIR/EBOOT.BIN from the same lawful ISO used by this workspace.",
            )
        else:
            report.pass_("INPUT_EBOOT_BIN", "Validated original EBOOT.BIN PSP header", path=psp_header_path, metadata=psp_meta or {})
    else:
        report.fail("INPUT_EBOOT_BIN", "Missing original PSP_GAME/SYSDIR/EBOOT.BIN", path=psp_header_path)

    if elf_meta and psp_meta:
        expected_segments = int(psp_meta["segment_count"])
        actual_segments = int(elf_meta["load_segments"])
        if expected_segments == actual_segments:
            report.pass_(
                "INPUT_EBOOT_PAIR",
                "EBOOT.elf and EBOOT.BIN agree on load-segment count",
                metadata={"segments": actual_segments},
            )
        else:
            report.fail(
                "INPUT_EBOOT_PAIR",
                "EBOOT.elf and EBOOT.BIN appear to be mismatched",
                detail=f"ELF PT_LOAD count={actual_segments}; PSP header segment count={expected_segments}",
                remediation="Regenerate both files from the same game image/revision.",
            )

    decrypted_dir = root / "place_game_here" / "EXTRACTED" / "decrypted"
    for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
        _check_elf_file(report, f"INPUT_PRX_{name.upper().replace('.', '_')}", decrypted_dir / name, f"decrypted {name}")

    if need_iso:
        selected, candidates = discover_iso(root)
        if not candidates:
            report.fail(
                "INPUT_ISO",
                "No game ISO was found",
                path=root / "place_game_here" / "ISO",
                remediation="Place exactly one lawfully obtained UCUS98701 ISO in place_game_here/ISO/.",
            )
        elif selected is None:
            report.fail(
                "INPUT_ISO",
                "Multiple game ISO candidates were found; selection would be ambiguous",
                detail=", ".join(str(path) for path in candidates),
                remediation="Keep exactly one ISO in place_game_here/ISO/ and remove the legacy game.iso fallback.",
            )
        else:
            metadata, error = _validate_iso(selected)
            if error:
                report.fail("INPUT_ISO", "Invalid ISO9660 game image", path=selected, detail=error)
            else:
                report.pass_("INPUT_ISO", "Validated ISO9660 game image", path=selected, metadata=metadata or {})
                if _scan_disc_id(selected, EXPECTED_DISC_ID):
                    report.pass_("INPUT_DISC_ID", f"Found expected disc ID {EXPECTED_DISC_ID} in the ISO", path=selected)
                else:
                    report.warn(
                        "INPUT_DISC_ID",
                        f"Could not confirm expected disc ID {EXPECTED_DISC_ID} in the first 128 MiB",
                        path=selected,
                        remediation="Confirm that this is the supported US UCUS98701 release before relying on the build.",
                    )

    if need_assets:
        data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
        if not data_root.is_dir():
            report.fail(
                "INPUT_XB_DATA",
                "Missing extracted XB asset tree",
                path=data_root,
                remediation="Run the documented libxb extraction workflow from your own game files.",
            )
        else:
            count, error = _bounded_nonempty_directory(data_root)
            if error:
                report.fail("INPUT_XB_DATA", "Could not scan the extracted XB asset tree", path=data_root, detail=error)
            elif count == 0:
                report.fail(
                    "INPUT_XB_DATA",
                    "The extracted XB asset directory is empty",
                    path=data_root,
                    remediation="Complete extraction; an empty placeholder directory is not a valid runtime input.",
                )
            else:
                report.pass_(
                    "INPUT_XB_DATA",
                    "Extracted XB asset tree is populated",
                    path=data_root,
                    metadata={"files_scanned": count, "scan_capped": count >= 100_000},
                )


def check_vfpu_assets(report: Report) -> None:
    root = report.root / "assets" / "vfpu"
    if not root.is_dir():
        report.fail("VFPU_ROOT", "Missing required VFPU table directory", path=root)
        return
    actual = {path.name: path for path in root.glob("*.dat") if path.is_file()}
    for name, expected_size in EXPECTED_VFPU_FILES.items():
        path = actual.get(name)
        if path is None:
            report.fail("VFPU_FILE", f"Missing required VFPU table {name}", path=root / name)
            continue
        try:
            size = path.stat().st_size
        except OSError as exc:
            report.fail("VFPU_FILE", f"Could not stat VFPU table {name}", path=path, detail=str(exc))
            continue
        if size != expected_size:
            report.fail(
                "VFPU_FILE",
                f"VFPU table {name} has the wrong size",
                path=path,
                detail=f"actual={size}; expected={expected_size}",
                remediation="Restore the tracked table from the repository; do not substitute arbitrary same-name data.",
            )
        else:
            report.pass_("VFPU_FILE", f"VFPU table {name} has the expected size", path=path, metadata={"bytes": size})
    extras = sorted(set(actual) - set(EXPECTED_VFPU_FILES))
    if extras:
        report.warn("VFPU_EXTRA", "Unexpected .dat files are present in assets/vfpu", detail=", ".join(extras))


def check_runtime_dependencies(report: Report, msys_path: Path) -> None:
    root = report.root
    candidates = (
        root / "build" / "hst" / "SDL3.dll",
        root / "SDL3.dll",
        msys_path / "SDL3.dll",
    )
    sdl = next((path for path in candidates if path.is_file()), None)
    if sdl is None:
        report.fail(
            "RUNTIME_SDL3",
            "SDL3.dll was not found in the build, repository root, or configured UCRT64 bin directory",
            remediation="Install mingw-w64-ucrt-x86_64-sdl3; the manager can then copy its matching SDL3.dll.",
        )
    else:
        ok, detail = _validate_pe_x64(sdl)
        if ok:
            report.pass_("RUNTIME_SDL3", "Resolved a 64-bit SDL3.dll", path=sdl, detail=detail)
        else:
            report.fail("RUNTIME_SDL3", "Resolved SDL3.dll is not a valid x86-64 DLL", path=sdl, detail=detail)

    vulkan_candidates = [
        root / "build" / "hst" / "vulkan-1.dll",
        root / "vulkan-1.dll",
        msys_path / "vulkan-1.dll",
    ]
    system_root = os.environ.get("SystemRoot")
    if system_root:
        vulkan_candidates.insert(0, Path(system_root) / "System32" / "vulkan-1.dll")
    vulkan = next((path for path in vulkan_candidates if path.is_file()), None)
    if vulkan is None:
        report.fail(
            "RUNTIME_VULKAN",
            "vulkan-1.dll was not found",
            remediation="Install a Vulkan-capable GPU driver or the MSYS2 UCRT64 Vulkan loader.",
        )
    else:
        ok, detail = _validate_pe_x64(vulkan)
        if ok:
            report.pass_("RUNTIME_VULKAN", "Resolved a 64-bit Vulkan loader", path=vulkan, detail=detail)
        else:
            report.fail("RUNTIME_VULKAN", "Resolved Vulkan loader is not a valid x86-64 DLL", path=vulkan, detail=detail)


def check_build_products(report: Report) -> None:
    build = report.root / "build" / "hst"
    exe = build / "hst.exe"
    image = build / "hst_image.bin"
    if exe.is_file():
        ok, detail = _validate_pe_x64(exe)
        if ok:
            report.pass_("BUILD_EXE", "Validated build/hst/hst.exe", path=exe, detail=detail)
        else:
            report.fail("BUILD_EXE", "build/hst/hst.exe is not a valid x86-64 PE executable", path=exe, detail=detail)
    else:
        report.fail("BUILD_EXE", "Missing build/hst/hst.exe", path=exe, remediation="Run BuildFull or BuildFast first.")
    if image.is_file() and image.stat().st_size > 0:
        report.pass_("BUILD_IMAGE", "Found nonempty hst_image.bin", path=image, metadata={"bytes": image.stat().st_size})
    else:
        report.fail("BUILD_IMAGE", "Missing or empty build/hst/hst_image.bin", path=image, remediation="Run the full code-generation pipeline.")


def check_repository_contract(report: Report) -> None:
    root = report.root
    required = (
        "LICENSE",
        "NOTICE.md",
        "README.md",
        "CONTRIBUTING.md",
        "SECURITY.md",
        "CODE_OF_CONDUCT.md",
        "docs/PUBLICATION_READINESS.md",
    )
    for rel in required:
        path = root / rel
        if path.is_file():
            report.pass_("REPO_REQUIRED", f"Required repository document exists: {rel}", path=path)
        else:
            report.fail("REPO_REQUIRED", f"Required repository document is missing: {rel}", path=path)

    license_path = root / "LICENSE"
    try:
        license_prefix = license_path.read_text(encoding="utf-8", errors="replace")[:1000]
    except OSError:
        license_prefix = ""
    if "Version 3, 29 June 2007" in license_prefix:
        report.pass_("LICENSE_ROOT", "Root LICENSE contains GNU GPL version 3")
    else:
        report.fail("LICENSE_ROOT", "Root LICENSE does not appear to contain the canonical GPLv3 text", path=license_path)

    metadata_paths = {
        "interface/package.json": ("license", "GPL-3.0-or-later"),
        "assets/release_manifest.json": ("license", "GPL-3.0-or-later"),
    }
    for rel, (field_name, expected) in metadata_paths.items():
        path = root / rel
        if not path.is_file():
            report.warn("LICENSE_METADATA", f"License metadata file is absent: {rel}", path=path)
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            report.fail("LICENSE_METADATA", f"Could not parse {rel}", path=path, detail=str(exc))
            continue
        actual = data.get(field_name)
        if actual == expected:
            report.pass_("LICENSE_METADATA", f"{rel} declares {expected}", path=path)
        else:
            report.warn(
                "LICENSE_METADATA",
                f"{rel} is not synchronized with the root GPLv3 project declaration",
                path=path,
                detail=f"{field_name}={actual!r}; expected {expected!r}",
                remediation=(
                    "Complete the coordinated project-metadata/SBOM transition without relabeling "
                    "inherited source-file provenance. Use --strict to make this a release-candidate gate."
                ),
            )

    for rel in ("README.md", "NOTICE.md", "CONTRIBUTING.md"):
        path = root / rel
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "GPL-3.0-or-later" in text or "GPLv3" in text:
            report.pass_("LICENSE_DOC", f"{rel} acknowledges the GPLv3 project-level declaration", path=path)
        else:
            report.warn(
                "LICENSE_DOC",
                f"{rel} does not acknowledge the current GPLv3 project-level declaration",
                path=path,
                remediation=(
                    "Reconcile the project-level declaration while preserving exact inherited "
                    "GPL-2.0-or-later and other third-party provenance notices."
                ),
            )

    notice = root / "NOTICE.md"
    if notice.is_file():
        text = notice.read_text(encoding="utf-8", errors="replace").lower()
        disclaimer_checks = {
            "NOTICE_NO_GAME": ("does not grant rights to the game", "game/firmware rights boundary"),
            "NOTICE_NO_KEYS": ("no decryption keys", "no-key distribution boundary"),
            "NOTICE_NO_AFFILIATION": ("not endorsed", "independence/no-endorsement disclaimer"),
            "NOTICE_PRIVATE_INPUT": ("users must supply their own legally obtained", "lawful user-supplied input requirement"),
            "NOTICE_LEGAL_REVIEW": ("legal review", "unresolved legal-review boundary"),
        }
        for code, (needle, description) in disclaimer_checks.items():
            if needle in text:
                report.pass_(code, f"NOTICE includes {description}")
            else:
                report.fail(code, f"NOTICE is missing the expected {description}")

    git = shutil.which("git")
    if not git or not (root / ".git").exists():
        report.warn("GIT_TRACKED_PRIVATE", "Git tracked-file hygiene was not checked (no local .git checkout)")
        return
    try:
        proc = subprocess.run(
            [git, "ls-files", "-z"],
            cwd=root,
            capture_output=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        report.warn("GIT_TRACKED_PRIVATE", "Could not enumerate tracked files", detail=str(exc))
        return
    if proc.returncode != 0:
        report.warn("GIT_TRACKED_PRIVATE", "git ls-files failed", detail=proc.stderr.decode(errors="replace"))
        return
    tracked = [item.decode("utf-8", errors="replace") for item in proc.stdout.split(b"\0") if item]
    bad: list[str] = []
    for rel in tracked:
        normalized = rel.lstrip("./")
        lower = normalized.lower()
        if any(lower.startswith(prefix.lower()) for prefix in PRIVATE_PREFIXES):
            bad.append(rel)
            continue
        if PurePosixPath(normalized).suffix.lower() in PRIVATE_EXTENSIONS:
            bad.append(rel)
    if bad:
        report.fail(
            "GIT_TRACKED_PRIVATE",
            "Tracked files include private/generated paths or extensions",
            detail=", ".join(sorted(bad)[:30]),
            remediation="Remove the files from Git history/index and run tools/publish_audit.py --tracked-only.",
        )
    else:
        report.pass_("GIT_TRACKED_PRIVATE", "No obvious private game-input paths/extensions are tracked")
