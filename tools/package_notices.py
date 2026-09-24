#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic third-party notice generation and licensing gate for native packages.

Enumerates bundled binaries, resolves and validates required license notices from
installed toolchain and repository files, emits the THIRD_PARTY_NOTICES bundle and
LGPL relinking materials (RELINK.md), and fails closed if any bundled binary lacks
a license record.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parent.parent

try:
    from title_codegen_plan import PackageRouteError
except ImportError:
    class PackageRouteError(Exception):  # type: ignore[no-redef]
        def __init__(self, code: str, message: str) -> None:
            super().__init__(f"{code}: {message}")
            self.code = code
            self.message = message


KNOWN_RUNTIME_DLLS: dict[str, dict[str, str]] = {
    "sdl3.dll": {
        "name": "SDL3",
        "spdx_id": "Zlib",
        "toolchain_license_rel": "share/licenses/SDL3/LICENSE.txt",
        "pacman_package": "sdl3",
        "upstream_origin": "https://github.com/libsdl-org/SDL",
    },
    "libwinpthread-1.dll": {
        "name": "winpthreads",
        "spdx_id": "MIT AND BSD-3-Clause",
        "toolchain_license_rel": "share/licenses/winpthreads/COPYING",
        "pacman_package": "winpthreads",
        "alt_toolchain_license_rel": "share/licenses/libwinpthread/COPYING",
        "upstream_origin": "https://mingw-w64.org/",
    },
    "libgcc_s_seh-1.dll": {
        "name": "gcc-libs (libgcc)",
        "spdx_id": "GPL-3.0-or-later WITH GCC-exception-3.1",
        "toolchain_license_rel": "share/licenses/gcc-libs/COPYING.RUNTIME",
        "extra_toolchain_license_rels": ["share/licenses/gcc-libs/COPYING3"],
        "pacman_package": "gcc-libs",
        "upstream_origin": "https://gcc.gnu.org/",
    },
    "libstdc++-6.dll": {
        "name": "gcc-libs (libstdc++)",
        "spdx_id": "GPL-3.0-or-later WITH GCC-exception-3.1",
        "toolchain_license_rel": "share/licenses/gcc-libs/COPYING.RUNTIME",
        "extra_toolchain_license_rels": ["share/licenses/gcc-libs/COPYING3"],
        "pacman_package": "gcc-libs",
        "upstream_origin": "https://gcc.gnu.org/",
    },
}

SYSTEM_DLL_PATTERNS = (
    re.compile(r"^api-ms-win-.*\.dll$", re.IGNORECASE),
    re.compile(r"^ext-ms-win-.*\.dll$", re.IGNORECASE),
    re.compile(
        r"^(kernel32|kernelbase|user32|gdi32|shell32|ole32|oleaut32|advapi32|"
        r"setupapi|imm32|version|winmm|comctl32|comdlg32|shlwapi|ws2_32|wsock32|"
        r"dnsapi|iphlpapi|netapi32|userenv|uxtheme|dwmapi|d3d[0-9a-z_]*|dxgi|"
        r"dinput[0-9a-z_]*|xinput[0-9a-z_]*|mfplat|mf|mfreadwrite|mfuuid|"
        r"vulkan-1|ntdll|msvcrt|ucrtbase)\.dll$",
        re.IGNORECASE,
    ),
)


def installed_package_version(toolchain_root: Path | None, package: str | None) -> str:
    """Return the installed MSYS2 package version for `package`, or "unknown".

    Reads `<msys root>/var/lib/pacman/local/<prefix>-<package>-<version>/desc` for the
    toolchain's own package prefix (for example mingw-w64-ucrt-x86_64-).
    """
    if toolchain_root is None or not package:
        return "unknown"
    local_db = toolchain_root.parent / "var" / "lib" / "pacman" / "local"
    if not local_db.is_dir():
        return "unknown"
    wanted = re.compile(rf"^mingw-w64-[a-z0-9_]+-x86_64-{re.escape(package)}$")
    for entry in sorted(local_db.iterdir()):
        desc = entry / "desc"
        if not desc.is_file():
            continue
        fields = desc.read_text(encoding="utf-8", errors="replace").split("\n\n")
        values = {}
        for field in fields:
            lines = field.strip().splitlines()
            if len(lines) >= 2 and lines[0].startswith("%"):
                values[lines[0]] = lines[1].strip()
        if wanted.match(values.get("%NAME%", "")):
            return values.get("%VERSION%", "unknown")
    return "unknown"


def is_system_dll(name: str) -> bool:
    """Check if a DLL name is a standard Windows system DLL."""
    clean = name.strip()
    return any(pat.fullmatch(clean) is not None for pat in SYSTEM_DLL_PATTERNS)


def resolve_toolchain_root() -> Path | None:
    """Discover the local compiler toolchain root (e.g. C:/msys64/ucrt64)."""
    def is_toolchain_root(candidate: Path) -> bool:
        # A compiler toolchain root carries its packages' license texts; a bare gcc on PATH
        # (for example one bundled with another application) does not qualify.
        return (candidate / "bin" / "gcc.exe").is_file() and (candidate / "share" / "licenses").is_dir()

    candidates: list[Path] = []
    mingw_env = os.environ.get("MINGW_PREFIX")
    if mingw_env:
        candidates.append(Path(mingw_env))
    gcc_path = shutil.which("gcc")
    if gcc_path:
        gcc_bin = Path(gcc_path).resolve().parent
        if gcc_bin.name.lower() == "bin":
            candidates.append(gcc_bin.parent)
    candidates.append(Path("C:/msys64/ucrt64"))
    for candidate in candidates:
        if is_toolchain_root(candidate):
            return candidate.resolve()
    return None


def parse_pe_imports(binary_path: Path) -> list[str]:
    """Parse PE import table in pure Python as a fallback when objdump is unavailable."""
    try:
        data = binary_path.read_bytes()
    except OSError:
        return []
    if len(data) < 64 or data[:2] != b"MZ":
        return []
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        return []
    opt_magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    if opt_magic == 0x10B:  # PE32
        import_rva = struct.unpack_from("<I", data, pe_offset + 24 + 96 + 8)[0]
        num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
        sec_offset = pe_offset + 24 + 224
    elif opt_magic == 0x20B:  # PE32+
        import_rva = struct.unpack_from("<I", data, pe_offset + 24 + 112 + 8)[0]
        num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
        sec_offset = pe_offset + 24 + 240
    else:
        return []
    if import_rva == 0:
        return []
    sections: list[tuple[int, int, int]] = []
    for i in range(num_sections):
        s_data = data[sec_offset + i * 40 : sec_offset + (i + 1) * 40]
        if len(s_data) < 24:
            break
        vsize, va, rsize, roff = struct.unpack_from("<IIII", s_data, 8)
        sections.append((va, max(vsize, rsize), roff))

    def rva_to_offset(rva: int) -> int | None:
        for va, size, roff in sections:
            if va <= rva < va + size:
                return roff + (rva - va)
        return None

    desc_offset = rva_to_offset(import_rva)
    if desc_offset is None:
        return []
    dlls: list[str] = []
    while desc_offset + 20 <= len(data):
        _orig_first_thunk, _td, _fwd, name_rva, _ft = struct.unpack_from("<IIIII", data, desc_offset)
        if name_rva == 0:
            break
        name_off = rva_to_offset(name_rva)
        if name_off is not None and name_off < len(data):
            end = data.find(b"\0", name_off)
            if end != -1:
                dll_str = data[name_off:end].decode("ascii", errors="ignore").strip()
                if dll_str:
                    dlls.append(dll_str)
        desc_offset += 20
    return dlls


def get_binary_imports(binary_path: Path, toolchain_root: Path | None = None) -> list[str]:
    """Deterministically enumerate DLL names imported by a PE binary using objdump -p or PE fallback."""
    objdump = shutil.which("objdump")
    if not objdump and toolchain_root:
        candidate = toolchain_root / "bin" / "objdump.exe"
        if candidate.is_file():
            objdump = str(candidate)

    if objdump:
        try:
            res = subprocess.run(
                [objdump, "-p", str(binary_path)],
                capture_output=True,
                text=True,
                check=True,
            )
            dlls: list[str] = []
            for line in res.stdout.splitlines():
                stripped = line.strip()
                if stripped.startswith("DLL Name:"):
                    dll_name = stripped.split(":", 1)[1].strip()
                    if dll_name:
                        dlls.append(dll_name)
            if dlls:
                return sorted(set(dlls))
        except (subprocess.SubprocessError, OSError):
            pass

    return sorted(set(parse_pe_imports(binary_path)))


def make_relative_path(path: Path, toolchain_root: Path | None, repo_root: Path) -> str:
    """Convert an absolute path to a toolchain-relative or repo-relative POSIX path.

    Never emits absolute local machine paths or user profiles into distribution artifacts.
    """
    resolved = path.resolve()
    if toolchain_root is not None:
        try:
            return resolved.relative_to(toolchain_root.resolve()).as_posix()
        except ValueError:
            pass
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        pass
    return path.name


def atrac3p_object_names(repo_root: Path = ROOT) -> list[str]:
    """Return the ATRAC3+ object paths the Makefile builds, derived from ATRAC3P_SRCS."""
    makefile = repo_root / "Makefile"
    try:
        text = makefile.read_text(encoding="utf-8")
    except OSError:
        return []
    match = re.search(r"^ATRAC3P_SRCS\s*:?=\s*((?:.*\\\n)*.*)$", text, re.MULTILINE)
    if not match:
        return []
    sources = match.group(1).replace("\\\n", " ").split()
    objects = [
        "$(BUILD_DIR)/atrac3p_" + src[len("src/rt/atrac3p/"):-2] + ".o"
        for src in sources
        if src.startswith("src/rt/atrac3p/") and src.endswith(".c")
    ]
    objects.append("$(BUILD_DIR)/atrac3p_bridge.o")
    return objects


def render_relink_guide(repo_root: Path = ROOT) -> str:
    """Generate the RELINK.md instructions explaining how to relink the runtime against modified ATRAC3+."""
    objects = atrac3p_object_names(repo_root)
    object_lines = "\n".join(f"- `{obj}`" for obj in objects) or "- (see `ATRAC3P_SRCS` in the Makefile)"
    return f"""# RELINK.md — Relinking Against Modified FFmpeg ATRAC3+ Objects

Nakagawa Recomp incorporates a subset of the FFmpeg ATRAC3+ audio decoder (`src/rt/atrac3p/`), licensed under the GNU Lesser General Public License version 2.1 or later (LGPL-2.1-or-later).

Under LGPL v2.1 Section 6 you may modify the ATRAC3+ decoder source code and relink the recompiled application executable with your modified library objects.

## 1. ATRAC3+ Source Files

The decoder subset lives in `src/rt/atrac3p/` (import record: `src/rt/atrac3p/PROVENANCE.md`). The bridge interface is `src/rt/atrac3p_bridge.c` and `src/rt/atrac3p_bridge.h`.

## 2. Decoder Object Files

The Makefile (`ATRAC3P_SRCS`) builds these objects in `$(BUILD_DIR)`:

{object_lines}

## 3. How to Relink

1. **Modify the source** inside `src/rt/atrac3p/`.
2. **Rebuild the runtime objects** (this includes the ATRAC3+ objects):

   ```powershell
   $env:Path = "C:\\msys64\\ucrt64\\bin;$env:Path"
   mingw32-make runtime-objects GAME_NAME=<game>
   ```

3. **Relink the application**:

   ```powershell
   mingw32-make compile GAME_NAME=<game>
   ```

   The `compile` target links the generated game translation units, the runtime objects and the rebuilt ATRAC3+ objects into `<game>.exe`.
"""


def generate_package_notices(
    package_dir: Path,
    *,
    repo_root: Path = ROOT,
    toolchain_root: Path | None = None,
    custom_license_map: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Generate THIRD_PARTY_NOTICES bundle, copy verbatim license texts, and enforce licensing gates.

    Raises:
        PackageRouteError("PACKAGE_LICENSE_RECORD_MISSING", ...): if any bundled binary lacks a license.
        PackageRouteError("PACKAGE_LICENSE_TEXT_MISSING", ...): if required license text is missing.
    """
    package_dir = package_dir.resolve()
    if toolchain_root is None:
        toolchain_root = resolve_toolchain_root()

    # Discover all bundled binaries (.exe and .dll)
    binaries: list[Path] = sorted(
        [
            p for p in package_dir.iterdir()
            if p.is_file() and p.suffix.lower() in {".exe", ".dll"}
        ],
        key=lambda p: p.name.lower(),
    )

    # Check imports of all binaries to ensure any dependent toolchain runtime DLLs are staged if available
    if toolchain_root:
        bin_dir = toolchain_root / "bin"
        if bin_dir.is_dir():
            for binary in list(binaries):
                imports = get_binary_imports(binary, toolchain_root)
                for imp in imports:
                    imp_lower = imp.lower()
                    if imp_lower in KNOWN_RUNTIME_DLLS:
                        dst = package_dir / imp
                        if not dst.is_file():
                            src = bin_dir / imp
                            if src.is_file():
                                shutil.copyfile(src, dst)
                                binaries.append(dst)

    binaries = sorted(set(binaries), key=lambda p: p.name.lower())

    # Build component license records
    components: list[dict[str, Any]] = []
    license_files_to_copy: list[tuple[str, Path, str]] = []  # (target_filename, src_path, display_name)

    # ATRAC3+ subset notice (compiled into the recomp runtime executable)
    has_exe = any(p.suffix.lower() == ".exe" for p in binaries)
    if has_exe or (repo_root / "src" / "rt" / "atrac3p").is_dir():
        atrac3p_lic_path = repo_root / "src" / "rt" / "atrac3p" / "LICENSE.LGPLv2.1.txt"
        if not atrac3p_lic_path.is_file():
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                "LICENSE_TEXT_MISSING: FFmpeg ATRAC3+ subset LGPL license text is missing from "
                f"the source tree ({make_relative_path(atrac3p_lic_path, toolchain_root, repo_root)})",
            )
        rel_atrac3p = make_relative_path(atrac3p_lic_path, toolchain_root, repo_root)
        components.append({
            "name": "FFmpeg ATRAC3+ subset",
            "version": "n4.4",
            "spdx_id": "LGPL-2.1-or-later",
            "source_path": rel_atrac3p,
            "license_file": "FFmpeg-ATRAC3P.txt",
            "relink_doc": "RELINK.md",
            "upstream_origin": "https://github.com/FFmpeg/FFmpeg",
            "disposition": "statically_linked_subset",
        })
        license_files_to_copy.append(("FFmpeg-ATRAC3P.txt", atrac3p_lic_path, "FFmpeg ATRAC3+ subset"))

    # Process bundled binaries
    for binary in binaries:
        bin_name = binary.name
        bin_lower = bin_name.lower()

        if binary.suffix.lower() == ".exe":
            # Recompiled executable: governed by Nakagawa Recomp project license and LGPL relink obligations
            proj_license = repo_root / "LICENSE"
            if proj_license.is_file():
                rel_proj = make_relative_path(proj_license, toolchain_root, repo_root)
                components.append({
                    "name": f"Nakagawa Recomp Runtime ({bin_name})",
                    "binary": bin_name,
                    "spdx_id": "GPL-3.0-or-later",
                    "source_path": rel_proj,
                    "license_file": "Nakagawa-Recomp-LICENSE.txt",
                })
                license_files_to_copy.append(("Nakagawa-Recomp-LICENSE.txt", proj_license, "Nakagawa Recomp"))
            continue

        # For DLLs, match known runtime DLLs or custom mapping
        record = None
        if custom_license_map and bin_lower in custom_license_map:
            record = custom_license_map[bin_lower]
        elif bin_lower in KNOWN_RUNTIME_DLLS:
            record = KNOWN_RUNTIME_DLLS[bin_lower]

        if record is None:
            raise PackageRouteError(
                "PACKAGE_LICENSE_RECORD_MISSING",
                f"bundled binary {bin_name!r} lacks a license record; packaging refused",
            )

        # Locate license text file
        lic_src: Path | None = None
        if "license_path" in record and Path(record["license_path"]).is_file():
            lic_src = Path(record["license_path"])
        elif toolchain_root:
            cand1 = toolchain_root / record["toolchain_license_rel"]
            if cand1.is_file():
                lic_src = cand1
            elif "alt_toolchain_license_rel" in record:
                cand2 = toolchain_root / record["alt_toolchain_license_rel"]
                if cand2.is_file():
                    lic_src = cand2

        if lic_src is None or not lic_src.is_file():
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: license text for bundled binary {bin_name!r} "
                f"({record.get('name')}) was not found in toolchain or local source",
            )

        clean_slug = re.sub(r"[^A-Za-z0-9._-]", "_", record["name"]).strip("_")
        target_lic_name = f"{clean_slug}.txt"
        rel_src = make_relative_path(lic_src, toolchain_root, repo_root)

        components.append({
            "name": record["name"],
            "binary": bin_name,
            "version": record.get("version")
            or installed_package_version(toolchain_root, record.get("pacman_package")),
            "spdx_id": record["spdx_id"],
            "source_path": rel_src,
            "license_file": target_lic_name,
            "upstream_origin": record.get("upstream_origin", "NOASSERTION"),
        })
        license_files_to_copy.append((target_lic_name, lic_src, record["name"]))
        for extra_rel in record.get("extra_toolchain_license_rels", []):
            extra_src = toolchain_root / extra_rel if toolchain_root else None
            if extra_src is None or not extra_src.is_file():
                raise PackageRouteError(
                    "PACKAGE_LICENSE_TEXT_MISSING",
                    f"LICENSE_TEXT_MISSING: license text {extra_rel!r} for bundled binary "
                    f"{bin_name!r} ({record.get('name')}) was not found in the toolchain",
                )
            extra_name = f"{clean_slug}-{Path(extra_rel).name}.txt"
            components[-1].setdefault("additional_license_files", []).append(extra_name)
            license_files_to_copy.append((extra_name, extra_src, f"{record['name']} ({Path(extra_rel).name})"))

    # Materialize THIRD_PARTY_NOTICES directory
    notices_dir = package_dir / "THIRD_PARTY_NOTICES"
    notices_dir.mkdir(parents=True, exist_ok=True)

    copied_texts: dict[str, str] = {}
    for target_name, src_file, display_name in license_files_to_copy:
        text = src_file.read_text(encoding="utf-8", errors="replace").strip()
        if not text:
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: license text file for {display_name} is empty: {src_file}",
            )
        dest_file = notices_dir / target_name
        dest_file.write_text(text + "\n", encoding="utf-8", newline="\n")
        copied_texts[display_name] = text

    # Emit RELINK.md into package root and THIRD_PARTY_NOTICES
    relink_content = render_relink_guide(repo_root)
    (package_dir / "RELINK.md").write_text(relink_content, encoding="utf-8", newline="\n")
    (notices_dir / "RELINK.md").write_text(relink_content, encoding="utf-8", newline="\n")

    # Emit THIRD_PARTY_NOTICES/index.json
    index_data = {
        "schema_version": 1,
        "bundle_type": "native_package_third_party_notices",
        "components": components,
    }
    index_path = notices_dir / "index.json"
    index_path.write_text(json.dumps(index_data, indent=2) + "\n", encoding="utf-8", newline="\n")

    # Emit root THIRD_PARTY_NOTICES.txt
    combined_lines = [
        "================================================================================",
        "                       THIRD-PARTY SOFTWARE NOTICES",
        "================================================================================",
        "",
        "This distribution contains third-party software components licensed under",
        "open-source licenses. The complete text of each applicable notice and license",
        "is set forth below. See THIRD_PARTY_NOTICES/index.json for the machine-readable",
        "index and RELINK.md for LGPL relinking documentation.",
        "",
    ]
    for comp in components:
        comp_name = comp["name"]
        lic_file = notices_dir / comp["license_file"]
        lic_text = lic_file.read_text(encoding="utf-8")
        combined_lines.extend([
            "--------------------------------------------------------------------------------",
            f"Component: {comp_name}",
            f"SPDX License: {comp['spdx_id']}",
            f"Source Path: {comp['source_path']}",
        ])
        if "binary" in comp:
            combined_lines.append(f"Binary: {comp['binary']}")
        if comp.get("version") and comp["version"] != "unknown":
            combined_lines.append(f"Version: {comp['version']}")
        if "relink_doc" in comp:
            combined_lines.append(f"Relink Guide: {comp['relink_doc']}")
        combined_lines.extend([
            "--------------------------------------------------------------------------------",
            "",
            lic_text.strip(),
            "",
            "",
        ])
        for extra_file in comp.get("additional_license_files", []):
            extra_text = (notices_dir / extra_file).read_text(encoding="utf-8")
            combined_lines.extend([extra_text.strip(), "", ""])

    (package_dir / "THIRD_PARTY_NOTICES.txt").write_text(
        "\n".join(combined_lines), encoding="utf-8", newline="\n"
    )

    return index_data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_dir", type=Path, help="Directory containing the native package")
    args = parser.parse_args(argv)

    if not args.package_dir.is_dir():
        print(f"PACKAGE_INVALID_PATH: {args.package_dir} is not a directory", file=sys.stderr)
        return 2

    try:
        generate_package_notices(args.package_dir)
        print(f"Generated third-party notices bundle in {args.package_dir}")
        return 0
    except PackageRouteError as exc:
        print(f"{exc.code}: {exc.message}", file=sys.stderr)
        return 2
    except Exception as exc:
        print(f"PACKAGE_BUILD_FAILED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
