#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Workspace, toolchain, input, runtime, and repository checks for nk_doctor.

Canonical successor to hst_doctor_checks.py (which is now a deprecated forwarding
wrapper that re-exports from this module).
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import uuid

from nk_doctor_core import (
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
    # The contract is a minimum supported PowerShell release, not a hard
    # maximum on the major version. Future Core releases remain compatible
    # unless their version is below the 7.6 floor.
    if edition != "Core" or (major, minor) < (7, 6):
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
            "The complete Nakagawa Recomp build/runtime is Windows-only",
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


def _resolve_disc_id(manifest: Path | str | dict[str, object] | None) -> str | None:
    if manifest is None:
        return None
    if isinstance(manifest, dict):
        disc = manifest.get("disc")
        if isinstance(disc, dict) and "id" in disc:
            return str(disc["id"])
        return None
    path = Path(manifest)
    if not path.is_file():
        return None
    try:
        import title_manifest
        data = title_manifest.load_manifest(path)
        disc = data.get("disc")
        if isinstance(disc, dict) and "id" in disc:
            return str(disc["id"])
    except Exception:
        pass
    return None


@dataclass(frozen=True)
class TitleDiagnosticContext:
    """Manifest-derived paths and requirements used by the workspace doctor.

    The manager plan is the authority for which title is being built.  The
    doctor cannot safely infer that title from an HST-shaped directory, so the
    caller supplies this small, path-only projection of the selected manifest.
    No private bytes are loaded while constructing it.
    """

    kind: str
    game_name: str
    game_elf_candidates: tuple[Path, ...]
    module_dir: Path | None
    psp_header_path: Path | None
    data_root: Path | None
    disc_image: Path | None
    required_modules: tuple[str, ...]
    requires_game_elf: bool
    requires_module_dir: bool
    requires_psp_header: bool
    requires_iso: bool
    requires_assets: bool
    allow_legacy_layout: bool


_BUILD_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$", re.IGNORECASE)
_MANIFEST_PATH_RE = re.compile(r"^[A-Za-z0-9._-]+(?:/[A-Za-z0-9._-]+)*$")
_MODULE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


def _safe_manifest_path(root: Path, value: object) -> Path | None:
    """Resolve one manifest-relative path without accepting traversal syntax."""
    if not isinstance(value, str) or not _MANIFEST_PATH_RE.fullmatch(value):
        return None
    return root.joinpath(*value.split("/"))


def _manifest_mapping(
    root: Path,
    manifest: Path | str | dict[str, object] | None,
) -> dict[str, object] | None:
    if isinstance(manifest, dict):
        return manifest
    if manifest is None:
        return None
    path = Path(manifest)
    if not path.is_absolute():
        path = root / path
    try:
        import title_manifest

        value = title_manifest.load_manifest(path)
    except Exception:
        return None
    return value if isinstance(value, dict) else None


def _safe_game_name(data: dict[str, object], override: str | None) -> str:
    candidates: list[object] = []
    if override:
        candidates.append(override)
    candidates.append(data.get("game_name"))
    title_id = data.get("id")
    if isinstance(title_id, str) and title_id:
        candidates.append(re.sub(r"-v\d+$", "", title_id).replace(" ", "_"))
    candidates.append("recomp")
    for candidate in candidates:
        if isinstance(candidate, str) and _BUILD_NAME_RE.fullmatch(candidate):
            return candidate
    return "recomp"


def _legacy_title_diagnostic_context(root: Path) -> TitleDiagnosticContext:
    """Return the pre-Phase-3 HST layout for direct legacy callers."""
    return TitleDiagnosticContext(
        kind="retail",
        game_name="hst",
        game_elf_candidates=(
            root / "place_game_here" / "EBOOT.elf",
            root / "eboot.elf",
        ),
        module_dir=root / "place_game_here" / "EXTRACTED" / "decrypted",
        psp_header_path=root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN",
        data_root=root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted",
        disc_image=None,
        required_modules=("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"),
        requires_game_elf=True,
        requires_module_dir=True,
        requires_psp_header=True,
        requires_iso=True,
        requires_assets=True,
        allow_legacy_layout=True,
    )


def title_diagnostic_context(
    root: Path,
    manifest: Path | str | dict[str, object] | None = None,
    *,
    game_name: str | None = None,
    legacy_default: bool = False,
) -> TitleDiagnosticContext:
    """Project a selected manifest into the paths the doctor must inspect.

    ``legacy_default`` is used only by the deprecated HST entry point and by
    the direct legacy helper API.  The canonical ``nk_doctor`` entry point
    supplies the public synthetic manifest when no title was selected, so it
    never silently falls back to HST paths.
    """
    data = _manifest_mapping(root, manifest)
    if data is None and manifest is None and legacy_default:
        return _legacy_title_diagnostic_context(root)

    if data is None:
        data = {}
    raw_kind = data.get("kind")
    kind = raw_kind if raw_kind in {"retail", "homebrew", "synthetic"} else "synthetic"
    effective_name = _safe_game_name(data, game_name)
    filesystem = data.get("filesystem")
    filesystem = filesystem if isinstance(filesystem, dict) else {}

    declared_data_root = _safe_manifest_path(root, filesystem.get("data_root"))
    declared_module_dir = _safe_manifest_path(root, filesystem.get("module_dir"))
    declared_psp_header = _safe_manifest_path(root, filesystem.get("psp_header"))
    declared_disc_image = _safe_manifest_path(root, filesystem.get("disc_image"))
    allow_legacy = kind == "retail"

    data_root = declared_data_root
    if data_root is None and allow_legacy:
        data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
    module_dir = declared_module_dir
    if module_dir is None and allow_legacy:
        module_dir = root / "place_game_here" / "EXTRACTED" / "decrypted"
    psp_header = declared_psp_header
    if psp_header is None and allow_legacy:
        psp_header = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN"

    executable = data.get("executable")
    executable = executable if isinstance(executable, dict) else {}
    if allow_legacy:
        elf_candidates = (
            root / "place_game_here" / "EBOOT.elf",
            root / "eboot.elf",
        )
    else:
        candidates: list[Path] = []
        declared_executable = _safe_manifest_path(root, executable.get("path"))
        if declared_executable is not None:
            candidates.append(declared_executable)
        candidates.extend(
            (
                root / "build" / "fixtures" / f"{effective_name}.elf",
                root / "fixtures" / f"{effective_name}.elf",
                root / "eboot.elf",
            )
        )
        elf_candidates = tuple(candidates)

    required_modules: list[str] = []
    requires_module_dir = False
    modules = data.get("modules")
    if isinstance(modules, list):
        for module in modules:
            if not isinstance(module, dict):
                continue
            role = module.get("role")
            if role == "guest-prx":
                requires_module_dir = True
                name = module.get("name")
                if module.get("required") is True and isinstance(name, str) and _MODULE_NAME_RE.fullmatch(name):
                    required_modules.append(name)
    if allow_legacy and legacy_default and not required_modules:
        # A direct legacy call may provide the old minimal manifest used by
        # compatibility tests.  The real HST manifest carries its own module
        # declarations; this fallback preserves the old wrapper contract only.
        requires_module_dir = True
        required_modules = ["libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"]

    requires_psp_header = executable.get("bss_metadata_source") == "psp-header"
    if allow_legacy and legacy_default and "bss_metadata_source" not in executable:
        requires_psp_header = True
    requires_iso = kind == "retail" or declared_disc_image is not None
    requires_assets = data_root is not None
    return TitleDiagnosticContext(
        kind=kind,
        game_name=effective_name,
        game_elf_candidates=elf_candidates,
        module_dir=module_dir,
        psp_header_path=psp_header,
        data_root=data_root,
        disc_image=declared_disc_image,
        required_modules=tuple(required_modules),
        requires_game_elf=True,
        requires_module_dir=requires_module_dir,
        requires_psp_header=requires_psp_header,
        requires_iso=requires_iso,
        requires_assets=requires_assets,
        allow_legacy_layout=allow_legacy,
    )


def check_private_inputs(
    report: Report,
    *,
    need_iso: bool,
    need_assets: bool,
    title_manifest: Path | str | dict[str, object] | None = None,
    expected_disc_id: str | None = None,
    title_context: TitleDiagnosticContext | None = None,
) -> None:
    root = report.root
    context_was_supplied = title_context is not None
    context = title_context or title_diagnostic_context(
        root,
        title_manifest,
        legacy_default=True,
    )
    if expected_disc_id is None and title_manifest is not None:
        expected_disc_id = _resolve_disc_id(title_manifest)
    if expected_disc_id is None and os.environ.get("TITLE_MANIFEST"):
        env_manifest = Path(os.environ["TITLE_MANIFEST"])
        if env_manifest.is_file():
            expected_disc_id = _resolve_disc_id(env_manifest)
    if expected_disc_id is None and context.allow_legacy_layout:
        local_hst = root / "assets" / "titles" / "hst-ucus98701.json"
        if local_hst.is_file():
            expected_disc_id = _resolve_disc_id(local_hst)

    elf_meta: dict[str, int] | None = None
    if context.requires_game_elf:
        elf_candidates = context.game_elf_candidates or (root / "eboot.elf",)
        elf_path = next((path for path in elf_candidates if path.is_file()), elf_candidates[0])
        elf_meta = _check_elf_file(report, "INPUT_EBOOT_ELF", elf_path, "decrypted title EBOOT ELF")

    psp_meta: dict[str, object] | None = None
    if context.requires_psp_header:
        psp_header_path = context.psp_header_path or (root / "EBOOT.BIN")
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
                "Title EBOOT ELF and PSP header agree on load-segment count",
                metadata={"segments": actual_segments},
            )
        else:
            report.fail(
                "INPUT_EBOOT_PAIR",
                "Title EBOOT ELF and PSP header appear to be mismatched",
                detail=f"ELF PT_LOAD count={actual_segments}; PSP header segment count={expected_segments}",
                remediation="Regenerate both files from the same game image/revision.",
            )

    if context.requires_module_dir:
        module_dir = context.module_dir or (root / "modules")
        if not module_dir.is_dir():
            if context.allow_legacy_layout and context.required_modules:
                # Keep the legacy report's per-module diagnostics stable while
                # using the same derived directory for the actual checks.
                for name in context.required_modules:
                    _check_elf_file(
                        report,
                        f"INPUT_PRX_{name.upper().replace('.', '_')}",
                        module_dir / name,
                        f"decrypted {name}",
                    )
            else:
                report.fail("INPUT_MODULE_DIR", "Missing declared title module directory", path=module_dir)
        else:
            for name in context.required_modules:
                _check_elf_file(
                    report,
                    f"INPUT_PRX_{name.upper().replace('.', '_')}",
                    module_dir / name,
                    f"decrypted {name}",
                )

    if need_iso and context.requires_iso:
        if context.disc_image is not None:
            selected = context.disc_image if context.disc_image.is_file() else None
            candidates = [context.disc_image] if selected is not None else []
        elif context.allow_legacy_layout:
            selected, candidates = discover_iso(root)
        else:
            selected, candidates = None, []
        if not candidates:
            remediation = (
                f"Place exactly one lawfully obtained {expected_disc_id} ISO in place_game_here/ISO/."
                if expected_disc_id
                else "Place exactly one lawfully obtained game ISO in place_game_here/ISO/."
            )
            report.fail(
                "INPUT_ISO",
                "No game ISO was found",
                path=root / "place_game_here" / "ISO",
                remediation=remediation,
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
                if expected_disc_id is not None:
                    if _scan_disc_id(selected, expected_disc_id):
                        report.pass_("INPUT_DISC_ID", f"Found expected disc ID {expected_disc_id} in the ISO", path=selected)
                    else:
                        report.warn(
                            "INPUT_DISC_ID",
                            f"Could not confirm expected disc ID {expected_disc_id} in the first 128 MiB",
                            path=selected,
                            remediation=f"Confirm that this is the supported {expected_disc_id} release before relying on the build.",
                        )
                else:
                    report.info(
                        "INPUT_DISC_ID",
                        "No title manifest with disc ID supplied; skipping disc ID confirmation",
                        path=selected,
                )

    if need_assets and context.requires_assets:
        sr_dataroot = None if context_was_supplied and context.data_root is not None else os.environ.get("SR_DATAROOT")
        if sr_dataroot:
            data_path = Path(sr_dataroot)
            if not data_path.is_absolute():
                report.fail(
                    "INPUT_SR_DATAROOT",
                    f"SR_DATAROOT is not an absolute path: {sr_dataroot}",
                    path=data_path,
                    remediation="Provide an absolute directory path for SR_DATAROOT.",
                )
            elif not data_path.is_dir():
                report.fail(
                    "INPUT_SR_DATAROOT",
                    f"Configured SR_DATAROOT directory was not found: {sr_dataroot}",
                    path=data_path,
                    remediation="Create or specify an existing directory containing extracted game assets.",
                )
            else:
                count, error = _bounded_nonempty_directory(data_path)
                if error:
                    report.fail("INPUT_SR_DATAROOT", "Could not scan the SR_DATAROOT asset tree", path=data_path, detail=error)
                elif count == 0:
                    report.fail("INPUT_SR_DATAROOT", "The configured SR_DATAROOT directory is empty", path=data_path)
                else:
                    report.pass_(
                        "INPUT_SR_DATAROOT",
                        "Configured SR_DATAROOT asset tree is populated",
                        path=data_path,
                        metadata={"files_scanned": count, "scan_capped": count >= 100_000},
                    )
        else:
            data_root = context.data_root
            if data_root is None:
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


def check_save_root(report: Report, root: Path) -> None:
    env_save = os.environ.get("SR_MEMSTICK")
    if env_save:
        save_path = Path(env_save)
        if not save_path.is_absolute():
            report.fail(
                "SAVE_ROOT",
                f"SR_MEMSTICK is not an absolute path: {env_save}",
                path=save_path,
                remediation="Provide an absolute directory path for SR_MEMSTICK.",
            )
            return
        if save_path.is_file():
            report.fail(
                "SAVE_ROOT",
                f"SR_MEMSTICK points to a regular file, expected directory: {env_save}",
                path=save_path,
                remediation="Point SR_MEMSTICK to a folder for save data storage.",
            )
            return
        save_root = save_path
    else:
        save_root = root / "memstick"

    probe_name = f".doctor_probe_{os.getpid()}_{uuid.uuid4().hex}.tmp"
    if save_root.is_dir():
        probe_file = save_root / probe_name
        try:
            probe_file.write_bytes(b"probe")
            report.pass_("SAVE_ROOT", "Save/memstick directory exists and is writable", path=save_root)
        except OSError as exc:
            report.fail("SAVE_ROOT", f"Save/memstick directory is not writable: {exc}", path=save_root)
        finally:
            try:
                if probe_file.exists():
                    probe_file.unlink()
            except OSError:
                pass
    elif save_root.parent.is_dir():
        probe_file = save_root.parent / probe_name
        try:
            probe_file.write_bytes(b"probe")
            report.pass_("SAVE_ROOT", "Save/memstick directory will be created on demand", path=save_root)
        except OSError as exc:
            report.fail("SAVE_ROOT", f"Save/memstick parent directory is not writable: {exc}", path=save_root.parent)
        finally:
            try:
                if probe_file.exists():
                    probe_file.unlink()
            except OSError:
                pass
    else:
        report.fail(
            "SAVE_ROOT",
            f"Save/memstick parent directory does not exist: {save_root.parent}",
            path=save_root,
            remediation="Create the parent directory or configure SR_MEMSTICK to a valid location.",
        )


def check_build_profile(report: Report, root: Path, game_name: str = "hst") -> None:
    profile_file = root / "build" / game_name / "runtime_profile.json"
    if profile_file.is_file():
        try:
            data = json.loads(profile_file.read_text(encoding="utf-8"))
            raw_entries: list[str] = []
            sections = data.get("sections")
            if isinstance(sections, dict):
                for sec in sections.values():
                    if isinstance(sec, dict):
                        ent = sec.get("entries")
                        if isinstance(ent, list):
                            raw_entries.extend(str(item) for item in ent)
                        elif isinstance(ent, dict):
                            raw_entries.extend(f"{k}={v}" for k, v in ent.items())
            ent = data.get("entries")
            if isinstance(ent, list):
                raw_entries.extend(str(item) for item in ent)
            elif isinstance(ent, dict):
                raw_entries.extend(f"{k}={v}" for k, v in ent.items())

            all_flags = " ".join(raw_entries)
            is_public_safe = "SR_PUBLIC_SAFE" in all_flags or "PUBLIC_SAFE=1" in all_flags
            if is_public_safe:
                report.info(
                    "BUILD_PROFILE",
                    "Selected build profile: public-safe (PUBLIC_SAFE=1; public profile with excluded backends stubbed)",
                    path=profile_file,
                    metadata={"profile": "public_safe", "public_safe": 1},
                )
            else:
                report.info(
                    "BUILD_PROFILE",
                    "Selected build profile: full (PUBLIC_SAFE=0; all built backends enabled)",
                    path=profile_file,
                    metadata={"profile": "full", "public_safe": 0},
                )
        except (OSError, json.JSONDecodeError):
            report.info("BUILD_PROFILE", "Runtime build profile recorded (unparsed)", path=profile_file)
    else:
        report.info("BUILD_PROFILE", "No runtime build profile recorded yet (unbuilt)")


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


def check_runtime_dependencies(report: Report, msys_path: Path, game_name: str = "hst") -> None:
    root = report.root
    candidates = (
        root / "build" / game_name / "SDL3.dll",
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
        root / "build" / game_name / "vulkan-1.dll",
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


def check_build_products(report: Report, game_name: str = "hst") -> None:
    build = report.root / "build" / game_name
    exe = build / f"{game_name}.exe"
    image = build / f"{game_name}_image.bin"
    if exe.is_file():
        ok, detail = _validate_pe_x64(exe)
        if ok:
            report.pass_("BUILD_EXE", f"Validated build/{game_name}/{game_name}.exe", path=exe, detail=detail)
        else:
            report.fail("BUILD_EXE", f"build/{game_name}/{game_name}.exe is not a valid x86-64 PE executable", path=exe, detail=detail)
    else:
        report.fail("BUILD_EXE", f"Missing build/{game_name}/{game_name}.exe", path=exe, remediation="Run BuildFull or BuildFast first.")
    if image.is_file() and image.stat().st_size > 0:
        report.pass_("BUILD_IMAGE", f"Found nonempty {game_name}_image.bin", path=image, metadata={"bytes": image.stat().st_size})
    else:
        report.fail("BUILD_IMAGE", f"Missing or empty build/{game_name}/{game_name}_image.bin", path=image, remediation="Run the full code-generation pipeline.")


class _GitConfigError(Exception):
    """The configuration could not be inspected, as distinct from being unset."""


def _git_config(root: Path, scope: str, key: str, *, as_bool: bool = False) -> str | None:
    """Read one git config value. None means *unset*; never means *unknown*.

    `git config --get` exits 1 for an unset key and uses other nonzero codes for
    real failures -- git missing, the repository refused as unsafe, a malformed
    config. Collapsing those into None would let a failed lookup report the same
    clean PASS as a genuinely clean checkout, which is the opposite of what this
    check exists to do, so anything but 0 or 1 raises.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "config", scope,
             *(("--type=bool",) if as_bool else ()), "--get", key],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise _GitConfigError(f"could not run git config: {error}") from error
    if proc.returncode == 1:
        return None
    if proc.returncode != 0:
        detail = (proc.stderr or "").strip().splitlines()
        raise _GitConfigError(detail[0] if detail else f"git config exited {proc.returncode}")
    # Exit 0 means the key exists, even when its value is empty. An empty
    # [user] entry is still an override and still breaks commits, so it must not
    # collapse into the same None that means "no key at all".
    return proc.stdout.strip()


_IDENTITY_SECTIONS = ("user", "author", "committer")


def _git_config_values(root: Path, scope: str, key: str) -> list[tuple[str, str]]:
    """Every value for *key* in *scope*, as (origin, value) in Git's own order.

    Three details matter and each was a real defect:

    * ``--includes`` is required. Without it an ``include.path`` directive in
      ``.git/config`` hides an identity that Git nonetheless resolves and
      authors commits with, so the check reported a clean PASS against a
      checkout that was actively re-authoring.
    * ``--get-all`` is required. ``--get`` collapses a multi-valued key to its
      last value, which reads as a single setting that one ``--unset`` would
      clear -- and ``--unset`` refuses a multi-valued key outright.
    * ``--show-origin`` says which file each value came from, which is what
      makes remediation safe: a value inside an included file cannot be removed
      through this scope, so the remedy must name the include rather than emit
      a command Git would reject.

    An empty list means unset. Anything other than exit 0 or 1 raises, so a
    failed lookup can never read as a clean checkout.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "config", scope,
             "--includes", "--show-origin", "--get-all", key],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise _GitConfigError(f"could not run git config: {error}") from error
    if proc.returncode == 1:
        return []
    if proc.returncode != 0:
        detail = (proc.stderr or "").strip().splitlines()
        raise _GitConfigError(detail[0] if detail else f"git config exited {proc.returncode}")

    values: list[tuple[str, str]] = []
    for line in proc.stdout.split("\n"):
        if not line:
            continue
        origin_field, separator, value = line.partition("\t")
        if not separator:
            continue
        # Origins are reported as "<type>:<name>", e.g. "file:.git/config".
        _, _, origin = origin_field.partition(":")
        values.append((origin, value.rstrip("\r")))
    return values


def _scope_config_file(root: Path, scope: str) -> str | None:
    """Absolute path of the file *scope* itself writes, for origin comparison."""
    relative = "config.worktree" if scope == "--worktree" else "config"
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--git-path", relative],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    return os.path.abspath(os.path.join(str(root), proc.stdout.strip()))


def check_agent_identity(report: Report) -> None:
    """Repository commit identity must come from the host, not from this checkout.

    Automated sessions have written a repository-local identity here, which
    silently re-authors every later commit in this checkout and in every
    worktree sharing it. AGENTS.md section 3 already forbids inventing a
    contributor identity; this makes the leftover visible rather than trusting
    each tool to have honoured the contract.

    Git resolves a commit's identity from ``user.*``, and lets ``author.*`` and
    ``committer.*`` override it per role, in the per-worktree file, the
    repository-local file, or anything either of those includes. All of that is
    inspected, because a check that looked only at ``user.*`` in only
    ``--local`` passed a checkout whose ``.git/config`` re-authored every commit.
    """
    root = report.root
    # (scope, key) -> (effective value, every origin, how many values)
    found: dict[tuple[str, str], tuple[str, list[str], int]] = {}
    own_files: dict[str, str | None] = {}
    try:
        worktree_scope = _git_config(
            root, "--local", "extensions.worktreeConfig", as_bool=True) == "true"
        for scope in ("--worktree", "--local"):
            # Without extensions.worktreeConfig, `git config --worktree` is
            # documented to behave exactly as --local. Reading it as its own
            # scope would report every .git/config value twice and emit a
            # `--worktree --unset-all` remedy that Git refuses.
            if scope == "--worktree" and not worktree_scope:
                continue
            own_files[scope] = _scope_config_file(root, scope)
            for section in _IDENTITY_SECTIONS:
                for name in ("name", "email"):
                    key = f"{section}.{name}"
                    values = _git_config_values(root, scope, key)
                    if not values:
                        continue
                    origins: list[str] = []
                    for origin, _value in values:
                        if origin not in origins:
                            origins.append(origin)
                    found[(scope, key)] = (values[-1][1], origins, len(values))
        global_name = _git_config(root, "--global", "user.name")
        global_email = _git_config(root, "--global", "user.email")
    except _GitConfigError as error:
        report.warn(
            "GIT_IDENTITY",
            "Could not inspect the commit identity, so a stray repository-local "
            f"override cannot be ruled out: {error}",
        )
        return

    if not found:
        report.pass_(
            "GIT_IDENTITY",
            "No repository-local commit identity override; the global identity applies",
        )
        return

    def _display(value: str) -> str:
        return "(empty)" if value == "" else value

    shown = ", ".join(
        f"{scope.lstrip('-')}:{key}={_display(value)}"
        + (f" ({count} values)" if count > 1 else "")
        for (scope, key), (value, _origins, count) in found.items())

    # Only a complete, single-valued user.name/user.email pair can be "the same
    # as the global identity". An author.* or committer.* key has no global
    # counterpart being duplicated, and a partial pair still changes behaviour,
    # so both fall through to the stronger wording.
    matches = ({(key, value) for (_scope, key), (value, _o, count) in found.items()
                if count == 1} == {("user.name", global_name), ("user.email", global_email)})
    detail = ("currently the same as the global identity, but it pins this checkout "
              "to that value if the global one ever changes"
              if matches else
              "overrides the global identity and will author or commit as itself "
              "in this checkout and every worktree sharing it")

    # Remediation must clear every populated key in every scope that carries
    # one: clearing only the highest-precedence scope leaves the next one
    # immediately effective. Keys are cleared individually rather than by
    # section, so an unrelated user.signingkey survives, and with --unset-all,
    # because --unset refuses a key holding more than one value.
    commands: list[str] = []
    includes: list[str] = []
    for (scope, key), (_value, origins, _count) in found.items():
        own = own_files.get(scope)
        for origin in origins:
            absolute = os.path.abspath(os.path.join(str(root), origin))
            if own and absolute == own:
                command = f"git config {scope} --unset-all {key}"
                if command not in commands:
                    commands.append(command)
                continue
            # A value inside an included file cannot be removed through this
            # scope, so name the include rather than emit a command Git would
            # reject. Report it relative to the repository when it lives there;
            # an include from elsewhere on the host is described without its
            # path, because this summary gets pasted into issues and logs.
            try:
                relative = os.path.relpath(absolute, str(root))
            except ValueError:
                relative = ""
            label = (relative if relative and not relative.startswith("..")
                     else "a config file outside the repository")
            if label not in includes:
                includes.append(label)

    remedy = ""
    if commands:
        remedy = (" If an automated session set this, clear it with '"
                  + " && ".join(commands) + "'.")
    if includes:
        remedy += (" Some values come from an included config ("
                   + ", ".join(includes)
                   + "), which this scope cannot unset: remove the include.path"
                   " directive or edit that file. Run 'git config --show-origin"
                   " --get-all user.email' to list the exact locations.")
    report.warn("GIT_IDENTITY",
                f"Repository commit identity is set here ({shown}) and {detail}.{remedy}")


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
            "NOTICE_NO_GAME": (
                (
                    "does not grant rights to the game",
                    "no game executable",
                    "excludes all game content",
                    "public source boundary",
                ),
                "game/firmware rights boundary",
            ),
            "NOTICE_NO_KEYS": (
                ("no decryption keys", "private inputs, keys", "ships no keys", "no key"),
                "no-key distribution boundary",
            ),
            "NOTICE_NO_AFFILIATION": (
                ("not endorsed", "not affiliated with or endorsed by"),
                "independence/no-endorsement disclaimer",
            ),
            "NOTICE_PRIVATE_INPUT": (
                (
                    "users must supply their own legally obtained",
                    "users must supply any lawful external inputs",
                    "user-supplied",
                ),
                "lawful user-supplied input requirement",
            ),
            "NOTICE_LEGAL_REVIEW": (
                ("legal review", "not legal advice", "qualified review"),
                "unresolved legal-review boundary",
            ),
        }
        for code, (needles, description) in disclaimer_checks.items():
            if any(n in text for n in needles):
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
