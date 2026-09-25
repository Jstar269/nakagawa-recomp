#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Workspace, toolchain, input, runtime, and repository checks for nk_doctor."""

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

from nk_core import package_cache
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


# Proven minimum PowerShell floor (issue #337). A static AST inventory of every
# tracked .ps1 shows no language or cmdlet feature newer than the automatic
# $IsWindows variable (PowerShell 6.0): no ternary, null-coalescing/null-
# conditional, pipeline-chain, -Parallel, -AsHashtable, Join-Path
# -AdditionalChildPath, Test-Json, Get-Error, clean-block, or -ProgressAction
# usage exists. 7.4 is the oldest release line Microsoft still supports (LTS,
# end of support 2026-11-10), and 337's non-goals exclude EOL lines and Windows
# PowerShell 5.1, so the enforced floor is 7.4 rather than the static 6.0
# maximum. Runtime evidence (2026-09-24): every PowerShell-backed test passed on
# 7.4.20, 7.5.11 and 7.6.6. 7.4 and 7.5 both reach end of support 2026-11-10;
# raise this floor to (7, 6) then.
MINIMUM_POWERSHELL: tuple[int, int] = (7, 4)
MINIMUM_POWERSHELL_TEXT = ".".join(str(part) for part in MINIMUM_POWERSHELL)


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
            f"PowerShell {MINIMUM_POWERSHELL_TEXT}+ (`pwsh`) is required",
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
    # unless their version is below the proven floor (see MINIMUM_POWERSHELL).
    if edition != "Core" or (major, minor) < MINIMUM_POWERSHELL:
        report.fail(
            "POWERSHELL_VERSION",
            f"PowerShell {MINIMUM_POWERSHELL_TEXT}+ (`pwsh`) is required",
            path=executable,
            detail=f"detected {edition or 'unknown'} {version_text or 'unknown'}",
            remediation="Install the current PowerShell 7 LTS line and invoke scripts with `pwsh`.",
            metadata=metadata,
        )
    else:
        report.pass_(
            "POWERSHELL_VERSION",
            f"PowerShell {MINIMUM_POWERSHELL_TEXT}+ is available",
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


DEEPEST_EXPECTED_BUILD_PATH = Path("build") / "synthetic" / "vfpu_oracle" / "nakagawa.stdout.txt"


def query_windows_long_paths_enabled() -> bool | None:
    """Read Windows LongPathsEnabled policy from the registry.

    Returns True if enabled, False if disabled or key/value missing,
    or None on non-Windows platforms. Read-only; never mutates the registry.
    """
    if os.name != "nt":
        return None
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\FileSystem",
            0,
            winreg.KEY_READ,
        ) as key:
            val, _ = winreg.QueryValueEx(key, "LongPathsEnabled")
            return bool(val)
    except (OSError, ValueError):
        return False


def check_long_paths(
    report: Report,
    root: Path | None = None,
    deepest_rel: Path | str | None = None,
) -> None:
    """Advisory diagnostic for Windows MAX_PATH (260) hazards and LongPathsEnabled policy."""
    resolved_root = (root or report.root).resolve()
    rel_path = Path(deepest_rel) if deepest_rel is not None else DEEPEST_EXPECTED_BUILD_PATH
    expected_deepest = resolved_root / rel_path
    total_len = len(str(expected_deepest))
    exceeds_260 = total_len > 260
    is_windows = (platform.system() == "Windows") or (os.name == "nt")

    if not is_windows:
        report.pass_(
            "LONG_PATHS",
            f"Expected build path length ({total_len} chars) within filesystem limits",
            detail=f"Non-Windows platform; deepest expected path: {expected_deepest}",
            metadata={"long_paths_enabled": None, "total_len": total_len, "deepest_path": str(expected_deepest), "exceeds_260": exceeds_260},
        )
        return

    long_paths_enabled = query_windows_long_paths_enabled()

    if exceeds_260 and not long_paths_enabled:
        report.warn(
            "LONG_PATHS",
            f"Deepest expected build path ({total_len} chars) exceeds 260 characters and Windows LongPathsEnabled policy is disabled",
            path=expected_deepest,
            detail=f"LongPathsEnabled={long_paths_enabled}, deepest expected path length={total_len} (limit 260): {expected_deepest}",
            remediation="Enable LongPathsEnabled in HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem or move the repository to a shorter path (for example directly under the drive root).",
            metadata={"long_paths_enabled": long_paths_enabled, "total_len": total_len, "deepest_path": str(expected_deepest), "exceeds_260": True, "max_path": 260},
        )
    elif exceeds_260 and long_paths_enabled:
        report.warn(
            "LONG_PATHS",
            f"Deepest expected build path ({total_len} chars) exceeds 260 characters",
            path=expected_deepest,
            detail=f"LongPathsEnabled={long_paths_enabled}, deepest expected path length={total_len} (limit 260): {expected_deepest}",
            remediation="Windows LongPathsEnabled is enabled, but legacy Win32 tools without long-path manifests may still fail. Consider using a shorter repository root.",
            metadata={"long_paths_enabled": long_paths_enabled, "total_len": total_len, "deepest_path": str(expected_deepest), "exceeds_260": True, "max_path": 260},
        )
    elif not long_paths_enabled:
        report.warn(
            "LONG_PATHS",
            f"Windows LongPathsEnabled policy is disabled (deepest expected build path: {total_len}/260 chars)",
            path=expected_deepest,
            detail=f"LongPathsEnabled={long_paths_enabled}, deepest expected path length={total_len} (limit 260): {expected_deepest}",
            remediation="Enable LongPathsEnabled in HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem to avoid MAX_PATH issues in deep directories.",
            metadata={"long_paths_enabled": long_paths_enabled, "total_len": total_len, "deepest_path": str(expected_deepest), "exceeds_260": False, "max_path": 260},
        )
    else:
        report.pass_(
            "LONG_PATHS",
            "Repository path length within 260 characters and Windows LongPathsEnabled is enabled",
            path=expected_deepest,
            detail=f"LongPathsEnabled={long_paths_enabled}, deepest expected path length={total_len} (limit 260): {expected_deepest}",
            metadata={"long_paths_enabled": long_paths_enabled, "total_len": total_len, "deepest_path": str(expected_deepest), "exceeds_260": False, "max_path": 260},
        )


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


class Sdl3ProviderError(RuntimeError):
    """Raised when SDL3 discovery fails or encounters an invalid/mixed provider."""


@dataclass
class Sdl3Provider:
    provider: str
    root_dir: Path
    include_dir: Path
    import_lib: Path
    runtime_dll: Path | None
    version: str
    arch: str
    is_supported: bool


def extract_sdl3_version(include_dir: Path) -> str | None:
    """Parse SDL3 version from SDL_version.h."""
    candidates = (
        include_dir / "SDL3" / "SDL_version.h",
        include_dir / "SDL_version.h",
    )
    for version_h in candidates:
        if version_h.is_file():
            try:
                content = version_h.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            major = re.search(r"#define\s+SDL_MAJOR_VERSION\s+(\d+)", content)
            minor = re.search(r"#define\s+SDL_MINOR_VERSION\s+(\d+)", content)
            micro = re.search(r"#define\s+SDL_MICRO_VERSION\s+(\d+)", content)
            if major and minor and micro:
                return f"{major.group(1)}.{minor.group(1)}.{micro.group(1)}"
    return None


def discover_sdl3_provider(
    explicit: Path | str | None = None,
    *,
    msys_path: Path | str | None = None,
    vulkan_sdk: Path | str | None = None,
    environment: str | None = None,
) -> Sdl3Provider:
    """Discover and validate the SDL3 dependency provider.

    Precedence order:
    1. Explicit path (via argument or SDL3_DIR/SDL3_PATH environment variable).
    2. Documented platform provider:
       - Windows: MSYS2 UCRT64 toolchain prefix (mingw-w64-ucrt-x86_64-sdl3).
       - Linux: pkg-config or system /usr prefix.

    Incidental SDL3 copies (such as those shipped in Vulkan SDK) are detected
    and rejected when the documented provider is absent.
    """
    explicit_val = str(explicit).strip() if explicit is not None else ""
    if not explicit_val:
        env_val = os.environ.get("SDL3_DIR") or os.environ.get("SDL3_PATH") if environment is None else environment
        explicit_val = str(env_val).strip() if env_val else ""

    if explicit_val:
        cand = Path(explicit_val).resolve()
        if not cand.is_dir():
            raise Sdl3ProviderError(
                f"Explicit SDL3 directory does not exist: {explicit_val}. "
                "Install with: pacman -S mingw-w64-ucrt-x86_64-sdl3 (or sudo apt install libsdl3-dev on Linux)."
            )
        inc_dirs = [cand / "include", cand / "Include", cand]
        found_inc = None
        for idir in inc_dirs:
            if (idir / "SDL3" / "SDL.h").is_file() or (idir / "SDL.h").is_file():
                found_inc = idir
                break
        if not found_inc:
            raise Sdl3ProviderError(
                f"Explicit SDL3 path is missing headers: {explicit_val}. "
                "Expected SDL3/SDL.h under include/ or root. "
                "Install with: pacman -S mingw-w64-ucrt-x86_64-sdl3 (or sudo apt install libsdl3-dev on Linux)."
            )

        lib_dirs = [cand / "lib", cand / "Lib", cand / "lib" / "x64", cand / "Lib" / "x64", cand]
        lib_names = ("libSDL3.dll.a", "libSDL3.a", "SDL3.lib", "libSDL3.so")
        found_lib = None
        for ldir in lib_dirs:
            for lname in lib_names:
                p = ldir / lname
                if p.is_file():
                    found_lib = p
                    break
            if found_lib:
                break
        if not found_lib:
            raise Sdl3ProviderError(
                f"Explicit SDL3 path is missing import library: {explicit_val}. "
                "Expected libSDL3.dll.a, libSDL3.a, or SDL3.lib under lib/ or root. "
                "Install with: pacman -S mingw-w64-ucrt-x86_64-sdl3 (or sudo apt install libsdl3-dev on Linux)."
            )

        dll_dirs = [cand / "bin", cand / "Bin", cand / "lib", cand / "lib" / "x64", cand]
        found_dll = None
        for ddir in dll_dirs:
            p = ddir / "SDL3.dll"
            if p.is_file():
                found_dll = p
                break

        version = extract_sdl3_version(found_inc) or "unknown"
        arch = "unknown"
        if found_dll:
            ok, pe_info = _validate_pe_x64(found_dll)
            if ok:
                arch = "x86_64"
            else:
                arch = pe_info

        prov_name = "explicit"
        if "ucrt64" in str(cand).lower() or (cand / "lib" / "libSDL3.dll.a").is_file():
            prov_name = "msys2_ucrt64"
        elif "vulkan" in str(cand).lower():
            prov_name = "vulkan_sdk_explicit"

        return Sdl3Provider(
            provider=prov_name,
            root_dir=cand,
            include_dir=found_inc,
            import_lib=found_lib,
            runtime_dll=found_dll,
            version=version,
            arch=arch,
            is_supported=True,
        )

    is_windows = (platform.system() == "Windows") or (os.name == "nt")
    if is_windows:
        if msys_path is not None:
            resolved_msys = Path(msys_path).resolve()
        else:
            gcc_path = shutil.which("gcc")
            if gcc_path and "ucrt64" in str(gcc_path).lower():
                resolved_msys = Path(gcc_path).resolve().parent
            elif os.environ.get("MSYS_PATH"):
                resolved_msys = Path(os.environ["MSYS_PATH"]).resolve()
            else:
                resolved_msys = Path(r"C:\msys64\ucrt64\bin")

        msys_root = resolved_msys.parent if resolved_msys.name.lower() == "bin" else resolved_msys
        msys_bin = msys_root / "bin"
        msys_inc = msys_root / "include"
        msys_lib = msys_root / "lib"

        has_headers = (msys_inc / "SDL3" / "SDL.h").is_file()
        import_lib_candidates = (
            msys_lib / "libSDL3.dll.a",
            msys_lib / "libSDL3.a",
        )
        found_import_lib = next((p for p in import_lib_candidates if p.is_file()), None)
        runtime_dll = msys_bin / "SDL3.dll"
        has_dll = runtime_dll.is_file()

        vulkan_incidental = False
        vk_path = None
        if vulkan_sdk:
            vk_path = Path(vulkan_sdk).resolve()
        else:
            try:
                vk_path = discover_vulkan_sdk()
            except (VulkanSdkError, RuntimeError):
                vk_path = None

        if vk_path and vk_path.is_dir():
            vk_candidates = (
                vk_path / "Lib" / "SDL3.lib",
                vk_path / "lib" / "SDL3.lib",
                vk_path / "Include" / "SDL3",
                vk_path / "include" / "SDL3",
            )
            vulkan_incidental = any(p.exists() for p in vk_candidates)

        if not (has_headers and found_import_lib and has_dll):
            missing = []
            if not has_headers:
                missing.append("headers (include/SDL3/SDL.h)")
            if not found_import_lib:
                missing.append("import library (lib/libSDL3.dll.a)")
            if not has_dll:
                missing.append("runtime DLL (bin/SDL3.dll)")
            missing_desc = ", ".join(missing)

            incidental_note = ""
            if vulkan_incidental:
                incidental_note = (
                    f" (an incidental copy exists in Vulkan SDK at {vk_path}, "
                    "but Vulkan SDK is not a supported SDL3 provider)"
                )

            raise Sdl3ProviderError(
                f"SDL3 dependency is missing from the supported MSYS2 UCRT64 toolchain{incidental_note}: "
                f"missing {missing_desc} in {msys_root}. "
                "Install with: pacman -S mingw-w64-ucrt-x86_64-sdl3"
            )

        ok_pe, pe_detail = _validate_pe_x64(runtime_dll)
        if not ok_pe:
            raise Sdl3ProviderError(
                f"Resolved SDL3.dll in {runtime_dll} is not a valid x86-64 PE: {pe_detail}"
            )

        version = extract_sdl3_version(msys_inc) or "unknown"
        return Sdl3Provider(
            provider="msys2_ucrt64",
            root_dir=msys_root,
            include_dir=msys_inc,
            import_lib=found_import_lib,
            runtime_dll=runtime_dll,
            version=version,
            arch="x86_64",
            is_supported=True,
        )

    pkg_config = shutil.which("pkg-config")
    if pkg_config:
        try:
            res = subprocess.run(
                [pkg_config, "--cflags", "--libs", "sdl3"],
                capture_output=True,
                text=True,
                check=False,
                timeout=5,
            )
            if res.returncode == 0:
                mod_ver = subprocess.run(
                    [pkg_config, "--modversion", "sdl3"],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=5,
                ).stdout.strip()
                prefix = subprocess.run(
                    [pkg_config, "--variable=prefix", "sdl3"],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=5,
                ).stdout.strip()
                inc_dir = subprocess.run(
                    [pkg_config, "--variable=includedir", "sdl3"],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=5,
                ).stdout.strip()
                lib_dir = subprocess.run(
                    [pkg_config, "--variable=libdir", "sdl3"],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=5,
                ).stdout.strip()
                root_path = Path(prefix) if prefix else Path("/usr")
                include_path = Path(inc_dir) if inc_dir else root_path / "include"
                import_lib = Path(lib_dir) / "libSDL3.so" if lib_dir else root_path / "lib" / "libSDL3.so"
                return Sdl3Provider(
                    provider="pkg_config",
                    root_dir=root_path,
                    include_dir=include_path,
                    import_lib=import_lib,
                    runtime_dll=None,
                    version=mod_ver or "unknown",
                    arch=platform.machine() or "x86_64",
                    is_supported=True,
                )
        except (OSError, subprocess.TimeoutExpired):
            pass

    sys_includes = (Path("/usr/include"), Path("/usr/local/include"))
    found_sys_inc = next((p for p in sys_includes if (p / "SDL3" / "SDL.h").is_file()), None)
    sys_libs = (
        Path("/usr/lib/x86_64-linux-gnu/libSDL3.so"),
        Path("/usr/lib64/libSDL3.so"),
        Path("/usr/lib/libSDL3.so"),
        Path("/usr/local/lib/libSDL3.so"),
    )
    found_sys_lib = next((p for p in sys_libs if p.is_file()), None)
    if found_sys_inc and found_sys_lib:
        version = extract_sdl3_version(found_sys_inc) or "unknown"
        return Sdl3Provider(
            provider="system",
            root_dir=found_sys_inc.parent,
            include_dir=found_sys_inc,
            import_lib=found_sys_lib,
            runtime_dll=None,
            version=version,
            arch=platform.machine() or "x86_64",
            is_supported=True,
        )

    raise Sdl3ProviderError(
        "SDL3 dependency is missing. Install with: sudo apt install libsdl3-dev (or distribution equivalent)."
    )


_DEFAULT_SYSTEM_INCLUDE_DIRS = frozenset(("/usr/include", "/usr/local/include"))


def _make_escape(value: str) -> str:
    return value.replace("$", "$$").replace("#", "\\#").replace("\n", " ")


def _pkg_config_flags(flag: str) -> str:
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        return ""
    try:
        res = subprocess.run([pkg_config, flag, "sdl3"], capture_output=True,
                             text=True, check=False, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return res.stdout.strip() if res.returncode == 0 else ""


def sdl3_make_fragment(explicit: str | None = None, compiler: str | None = None) -> str:
    """Every SDL3 Make variable from one discovery pass.

    The Makefile evaluates this once per parse; separate queries cost one
    interpreter start each. System default include/library directories are
    never emitted as -I/-L: forcing -I/usr/include reorders GCC's own header
    search and breaks #include_next in the C++ reference build.
    """
    exp = explicit if explicit and explicit.strip() else None
    values = {"SDL3_DIR": "", "SDL3_ERROR": "", "SDL3_PROVIDER": "none",
              "SDL3_VERSION": "none", "SDL3_DLL": "",
              "SDL3_INC_FLAGS": "", "SDL3_LDFLAGS": ""}
    try:
        p = discover_sdl3_provider(explicit=exp)
    except Sdl3ProviderError as exc:
        values["SDL3_ERROR"] = str(exc)
    else:
        values["SDL3_DIR"] = p.root_dir.as_posix()
        values["SDL3_PROVIDER"] = p.provider
        values["SDL3_VERSION"] = p.version
        values["SDL3_DLL"] = p.runtime_dll.as_posix() if p.runtime_dll else ""
        mismatch = _compiler_toolchain_mismatch(p, compiler)
        if mismatch:
            # Adding this provider's -I/-L to a different MinGW links its C runtime
            # against the wrong one. Stop the SDL3-linking targets with the remedy.
            values["SDL3_ERROR"] = mismatch
        elif p.provider == "pkg_config":
            values["SDL3_INC_FLAGS"] = _pkg_config_flags("--cflags")
            values["SDL3_LDFLAGS"] = " ".join(
                f for f in _pkg_config_flags("--libs-only-L").split())
        else:
            inc = p.include_dir.as_posix()
            lib = p.import_lib.parent.as_posix()
            if inc not in _DEFAULT_SYSTEM_INCLUDE_DIRS:
                values["SDL3_INC_FLAGS"] = f"-I{inc}"
            if not lib.startswith(("/usr/lib", "/lib")):
                values["SDL3_LDFLAGS"] = f"-L{lib}"
    return "".join(f"{k} := {_make_escape(v)}\n" for k, v in values.items())


def _compiler_toolchain_mismatch(provider: "Sdl3Provider", compiler: str | None) -> str:
    """Name a compiler that does not belong to the MSYS2 UCRT64 SDL3 provider.

    Only the MSYS2 provider is tied to one toolchain; other providers are left alone.
    """
    if provider.provider != "msys2_ucrt64" or not compiler:
        return ""
    resolved = shutil.which(compiler)
    if not resolved:
        return ""
    toolchain_bin = (provider.root_dir / "bin").resolve()
    try:
        in_toolchain = Path(resolved).resolve().parent == toolchain_bin
    except OSError:
        in_toolchain = False
    if in_toolchain:
        return ""
    return (
        f"SDL3 dependency is missing from the compiler's toolchain: {compiler} resolves to "
        f"{Path(resolved).as_posix()}, not the MSYS2 UCRT64 toolchain that provides SDL3 at "
        f"{provider.root_dir.as_posix()}. Put {toolchain_bin.as_posix()} first on PATH "
        "(see docs/SETUP.md)."
    )


def write_sdl3_make_fragment(path: str, explicit: str | None = None,
                             compiler: str | None = None) -> None:
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = sdl3_make_fragment(explicit, compiler)
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")


def query_sdl3_make(explicit: str | None = None) -> str:
    """Helper for Makefile to discover SDL3 root directory in a single call."""
    exp = explicit if explicit and explicit.strip() else None
    try:
        provider = discover_sdl3_provider(explicit=exp)
        return provider.root_dir.as_posix()
    except Sdl3ProviderError:
        return ""


def query_sdl3_error(explicit: str | None = None) -> str:
    """Helper for Makefile to query actionable SDL3 failure message."""
    exp = explicit if explicit and explicit.strip() else None
    try:
        discover_sdl3_provider(explicit=exp)
        return ""
    except Sdl3ProviderError as exc:
        return str(exc)


def query_sdl3_info(var: str, explicit: str | None = None) -> str:
    """Query a specific attribute of the resolved SDL3 provider."""
    exp = explicit if explicit and explicit.strip() else None
    try:
        p = discover_sdl3_provider(explicit=exp)
        if var == "inc":
            return p.include_dir.as_posix()
        elif var == "lib_dir":
            return p.import_lib.parent.as_posix()
        elif var == "lib":
            return p.import_lib.as_posix()
        elif var == "dll":
            return p.runtime_dll.as_posix() if p.runtime_dll else ""
        elif var == "version":
            return p.version
        elif var == "provider":
            return p.provider
        elif var == "arch":
            return p.arch
        return ""
    except Sdl3ProviderError:
        return ""


def check_toolchain(
    report: Report,
    msys_path: Path,
    vulkan_sdk: Path | None,
    root: Path | None = None,
    sdl3_path: Path | None = None,
) -> None:
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

    try:
        sdl_provider = discover_sdl3_provider(
            explicit=sdl3_path,
            msys_path=msys_path,
            vulkan_sdk=vulkan_sdk,
        )
    except Sdl3ProviderError as exc:
        report.fail(
            "SDL3_PROVIDER",
            "No usable SDL3 provider discovered",
            detail=str(exc),
            remediation="Install mingw-w64-ucrt-x86_64-sdl3 in MSYS2 UCRT64, or pass --sdl3-path with a complete SDL3 distribution.",
        )
        report.fail(
            "SDL3_IMPORT",
            "SDL3 import library was not found in the supported provider",
            detail=str(exc),
            remediation="Install mingw-w64-ucrt-x86_64-sdl3 in MSYS2 UCRT64.",
        )
        sdl_provider = None
    else:
        report.pass_(
            "SDL3_PROVIDER",
            f"Supported SDL3 provider selected ({sdl_provider.provider})",
            path=sdl_provider.root_dir,
            metadata={
                "provider": sdl_provider.provider,
                "version": sdl_provider.version,
                "arch": sdl_provider.arch,
                "include_dir": str(sdl_provider.include_dir),
                "import_lib": str(sdl_provider.import_lib),
                "runtime_dll": str(sdl_provider.runtime_dll) if sdl_provider.runtime_dll else None,
            },
        )
        report.pass_(
            "SDL3_HEADERS",
            f"SDL3 headers are available (version {sdl_provider.version})",
            path=sdl_provider.include_dir,
        )
        report.pass_(
            "SDL3_IMPORT",
            "SDL3 import library is available",
            path=sdl_provider.import_lib,
            detail=f"{sdl_provider.import_lib.name} ({sdl_provider.provider})",
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

    if vulkan_sdk:
        vk_path = Path(vulkan_sdk).resolve()
        vk_sdl = [
            vk_path / "Lib" / "SDL3.lib",
            vk_path / "lib" / "SDL3.lib",
            vk_path / "Include" / "SDL3",
            vk_path / "include" / "SDL3",
        ]
        if any(p.exists() for p in vk_sdl):
            report.info(
                "SDL3_INCIDENTAL_VULKAN",
                "Vulkan SDK contains incidental SDL3 files; build explicitly isolates documented MSYS2 provider",
                path=vk_path,
            )


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

    The manager plan is the authority for which title is being built. The
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


def title_diagnostic_context(
    root: Path,
    manifest: Path | str | dict[str, object] | None = None,
    *,
    game_name: str | None = None,
) -> TitleDiagnosticContext:
    """Project a selected manifest into the paths the doctor must inspect."""
    data = _manifest_mapping(root, manifest)
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

    data_root = declared_data_root
    module_dir = declared_module_dir
    psp_header = declared_psp_header

    executable = data.get("executable")
    executable = executable if isinstance(executable, dict) else {}
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
    requires_psp_header = executable.get("bss_metadata_source") == "psp-header"
    requires_iso = kind == "retail" or declared_disc_image is not None
    requires_assets = kind == "retail" or data_root is not None
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
    context = title_context or title_diagnostic_context(root, title_manifest)
    if expected_disc_id is None and title_manifest is not None:
        expected_disc_id = _resolve_disc_id(title_manifest)
    if expected_disc_id is None and os.environ.get("TITLE_MANIFEST"):
        env_manifest = Path(os.environ["TITLE_MANIFEST"])
        if env_manifest.is_file():
            expected_disc_id = _resolve_disc_id(env_manifest)

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
            for name in context.required_modules:
                _check_elf_file(
                    report,
                    f"INPUT_PRX_{name.upper().replace('.', '_')}",
                    module_dir / name,
                    f"decrypted {name}",
                )
        else:
            for name in context.required_modules:
                _check_elf_file(
                    report,
                    f"INPUT_PRX_{name.upper().replace('.', '_')}",
                    module_dir / name,
                    f"decrypted {name}",
                )

    if need_iso and context.requires_iso:
        selected = context.disc_image
        if selected is None:
            report.fail(
                "INPUT_ISO",
                "The selected title manifest does not declare filesystem.disc_image",
                remediation="Declare the lawful local ISO path in the title manifest.",
            )
        elif not selected.is_file():
            report.fail(
                "INPUT_ISO",
                "The title manifest's declared game ISO was not found",
                path=selected,
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
        sr_dataroot = os.environ.get("SR_DATAROOT")
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
                report.fail(
                    "INPUT_XB_DATA",
                    "The selected title manifest does not declare filesystem.data_root",
                    remediation="Declare the extracted asset directory in the title manifest.",
                )
            elif not data_root.is_dir():
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


def check_build_profile(report: Report, root: Path, game_name: str = "recomp") -> None:
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


def check_runtime_dependencies(report: Report, msys_path: Path, game_name: str = "recomp") -> None:
    root = report.root
    sdl_dll = None
    try:
        provider = discover_sdl3_provider(msys_path=msys_path)
        sdl_dll = provider.runtime_dll
    except Sdl3ProviderError:
        pass

    candidates = [
        root / "build" / game_name / "SDL3.dll",
        root / "SDL3.dll",
    ]
    if sdl_dll and sdl_dll.is_file() and sdl_dll not in candidates:
        candidates.append(sdl_dll)
    candidates.append(msys_path / "SDL3.dll")
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


def check_build_products(report: Report, game_name: str = "recomp") -> None:
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


def check_runtime_package_cache(
    report: Report,
    user_data_root: Path,
    disc_id: str,
    expected_key: dict[str, object] | None = None,
) -> None:
    if re.fullmatch(r"[A-Za-z]{4}[0-9]{5}", disc_id or "") is None:
        report.fail(
            "RUNTIME_PACKAGE_CACHE",
            "Disc ID is not a valid nine-character PSP identity",
            remediation="Re-import the ISO before validating its package cache.",
        )
        return
    package_dir = user_data_root / "packages" / disc_id.upper()
    remediation = f"Run: python tools/nk_cli.py build-package {disc_id.upper()}"
    if not package_dir.is_dir():
        report.info(
            "RUNTIME_PACKAGE_CACHE",
            "Runtime package is missing; no cache entry is launchable",
            path=package_dir,
            remediation=remediation,
        )
        return
    valid, reason = package_cache.validate_package_cache(package_dir)
    if not valid:
        report.fail(
            "RUNTIME_PACKAGE_CACHE",
            "Runtime package cache entry is incomplete or corrupt",
            path=package_dir,
            detail=reason,
            remediation=remediation,
        )
        return
    if expected_key is not None:
        previous_key = package_cache.package_cache_key(package_dir)
        decision = package_cache.compare_cache_keys(previous_key, expected_key)
        if not decision.native_objects_reusable:
            detail = ", ".join(decision.reasons) or "cache key changed"
            report.warn(
                "RUNTIME_PACKAGE_CACHE",
                "Runtime package requires rebuild because its cache key changed",
                path=package_dir,
                detail=detail,
                remediation=remediation,
            )
            return
    report.pass_(
        "RUNTIME_PACKAGE_CACHE",
        "Runtime package completion manifest and cache digests are valid",
        path=package_dir,
    )


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
