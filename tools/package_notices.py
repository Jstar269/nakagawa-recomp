#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic third-party notice generation and licensing gate for native packages.

Enumerates bundled binaries, resolves and validates required license notices from
``assets/third_party_components.json`` (the single machine-readable source of truth)
with the local toolchain as a fallback source of the same texts, emits the
THIRD_PARTY_NOTICES bundle and LGPL relinking materials (RELINK.md), and fails
closed if any bundled binary lacks a license record.
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

#: Machine-readable source of truth for every third-party component that can enter
#: a release artifact. The native package, the dashboard standalone output, the
#: SBOM and the release gate all read this one file.
COMPONENTS_IDENTITY = "assets/third_party_components.json"
COMPONENTS_PATH = ROOT / COMPONENTS_IDENTITY

try:
    from title_codegen_plan import PackageRouteError
except ImportError:
    class PackageRouteError(Exception):  # type: ignore[no-redef]
        def __init__(self, code: str, message: str) -> None:
            super().__init__(f"{code}: {message}")
            self.code = code
            self.message = message


def load_component_inventory(path: Path | None = None) -> dict[str, Any]:
    """Return the third-party component inventory, failing closed when unusable.

    The inventory is the authority for every license record, so a missing or
    malformed file is a named packaging failure rather than a silent fallback to
    an empty record set that would let an unrecorded binary through.
    """
    source = path or COMPONENTS_PATH
    if not source.is_file():
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_MISSING: third-party component inventory "
            f"{COMPONENTS_IDENTITY} is missing from the source tree",
        )
    try:
        data = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_UNREADABLE: {COMPONENTS_IDENTITY} could not be parsed: {exc}",
        ) from exc
    dlls = data.get("native", {}).get("dlls")
    if not isinstance(dlls, dict) or not dlls:
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_INCOMPLETE: {COMPONENTS_IDENTITY} declares no native.dlls records",
        )
    return data


def notice_file_name(record: dict[str, Any]) -> str:
    """Return the notice filename for a component record.

    The inventory names it explicitly so the emitted filename stays stable when a
    display name is reworded; a record without one falls back to a slug.
    """
    declared = record.get("notice_file")
    if declared:
        return str(declared)
    return re.sub(r"[^A-Za-z0-9._-]", "_", record["name"]).strip("_") + ".txt"


def component_license_sources(
    record: dict[str, Any], toolchain_root: Path | None, repo_root: Path
) -> list[tuple[Path, str]]:
    """Return the (path, display label) license texts for one component record.

    The in-tree text under ``third_party/licenses/`` is authoritative because it
    travels with the source; the local toolchain copy is the fallback so a host
    whose toolchain is newer than the checked-in text still resolves one.
    """
    sources: list[tuple[Path, str]] = []
    for entry in record.get("license_texts", []):
        in_tree = repo_root / entry["file"]
        if in_tree.is_file():
            sources.append((in_tree, entry["file"]))
            continue
        toolchain_rel = entry.get("toolchain_license_rel")
        if toolchain_root and toolchain_rel:
            candidate = toolchain_root / toolchain_rel
            if candidate.is_file():
                sources.append((candidate, toolchain_rel))
    return sources


def known_runtime_dlls(repo_root: Path = ROOT) -> dict[str, dict[str, Any]]:
    """Map every lowercased bundleable DLL name to its inventory record.

    Includes ``host_resolved_components`` so a loader a user places beside the
    package still gets a named notice rather than an unexplained refusal.
    """
    inventory = load_component_inventory()
    records: dict[str, dict[str, Any]] = {}
    for dll_name, record in inventory["native"]["dlls"].items():
        records[dll_name.lower()] = record
    for record in inventory["native"].get("host_resolved_components", []):
        dll = record.get("dll")
        if dll:
            records[dll.lower()] = record
    return records


def packaged_dll_names() -> set[str]:
    """Return the lowercased DLL names the packaging routes may copy into a package.

    Deliberately excludes ``host_resolved_components``: the Vulkan loader is
    resolved from the host and this repository does not redistribute it
    (NOTICE.md), so it is recorded for attribution but never staged.
    """
    return {
        name.lower()
        for name, record in load_component_inventory()["native"]["dlls"].items()
        if record.get("disposition") == "copied_into_package"
    }


#: Retained as a module attribute so existing importers keep working; the values
#: now come from assets/third_party_components.json rather than a second literal.
KNOWN_RUNTIME_DLLS: dict[str, dict[str, Any]] = known_runtime_dlls()

SYSTEM_DLL_PATTERNS = tuple(
    re.compile(pattern, re.IGNORECASE)
    for pattern in load_component_inventory()["native"]["system_dll_patterns"]
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


PROJECT_REPOSITORY_URL = "https://github.com/Jstar269/nakagawa-recomp"


def _git_text(repo_root: Path, *args: str) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), *args],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def source_reference(repo_root: Path = ROOT) -> dict[str, Any]:
    """The exact source a package was built from (#421).

    The whole program is GPL-3.0-or-later and the FFmpeg ATRAC3+ subset is
    LGPL-2.1-or-later, statically linked. The maintainer's decision for #421 is
    that the complete corresponding source is the relink mechanism, so every
    package names the repository, commit and (when built from one) tag it came
    from, and says so plainly when the working tree carried local changes.
    Without git, the commit is recorded as unknown rather than guessed.
    """
    commit = _git_text(repo_root, "rev-parse", "HEAD")
    tag = _git_text(repo_root, "describe", "--tags", "--exact-match", "HEAD") if commit else None
    status = _git_text(repo_root, "status", "--porcelain", "--untracked-files=no") if commit else None
    return {
        "repository": PROJECT_REPOSITORY_URL,
        "commit": commit or None,
        "tag": tag or None,
        "local_changes": bool(status) if status is not None else None,
    }


def render_source_notice(reference: dict[str, Any]) -> str:
    """SOURCE.txt: where the complete corresponding source of this build is."""
    commit = reference.get("commit") or "unknown (built without git metadata)"
    lines = [
        "COMPLETE CORRESPONDING SOURCE",
        "",
        "This package is Nakagawa Recomp, distributed under the GNU General Public",
        "License version 3 or later. It statically links a subset of the FFmpeg",
        "ATRAC3+ decoder, which is licensed under the GNU Lesser General Public",
        "License version 2.1 or later. The complete corresponding source of this",
        "build, including that subset, is available from:",
        "",
        f"  Repository: {reference.get('repository', PROJECT_REPOSITORY_URL)}",
        f"  Commit:     {commit}",
    ]
    if reference.get("tag"):
        lines.append(f"  Tag:        {reference['tag']}")
    if reference.get("local_changes"):
        lines.extend([
            "",
            "This build was made from a working tree with local changes to tracked",
            "files; its source is the commit above plus those changes.",
        ])
    lines.extend([
        "",
        "You may modify that source, including the ATRAC3+ subset, rebuild, and",
        "relink the program; RELINK.md describes the steps.",
    ])
    return "\n".join(lines) + "\n"


def render_relink_guide(repo_root: Path = ROOT) -> str:
    """Generate the RELINK.md instructions explaining how to relink the runtime against modified ATRAC3+."""
    objects = atrac3p_object_names(repo_root)
    object_lines = "\n".join(f"- `{obj}`" for obj in objects) or "- (see `ATRAC3P_SRCS` in the Makefile)"
    return f"""# RELINK.md — Relinking Against Modified FFmpeg ATRAC3+ Objects

Nakagawa Recomp incorporates a subset of the FFmpeg ATRAC3+ audio decoder (`src/rt/atrac3p/`), licensed under the GNU Lesser General Public License version 2.1 or later (LGPL-2.1-or-later).

Under LGPL v2.1 Section 6 you may modify the ATRAC3+ decoder source code and relink the recompiled application executable with your modified library objects.

The whole program is licensed under the GNU General Public License version 3 or later and its complete corresponding source is published, so you can rebuild and relink every part of it, not only the decoder. `SOURCE.txt` in this package names the exact repository commit this build came from.

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
            staged_names = packaged_dll_names()
            for binary in list(binaries):
                imports = get_binary_imports(binary, toolchain_root)
                for imp in imports:
                    imp_lower = imp.lower()
                    if imp_lower in staged_names:
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

    # Linked/host-resolved components, driven by the inventory rather than a second
    # literal. The LGPL subset ships inside every runtime executable, so it is
    # named whenever the package contains one.
    inventory = load_component_inventory()
    for linked in inventory["native"].get("linked_components", []):
        sources = component_license_sources(linked, toolchain_root, repo_root)
        if not sources:
            declared = [entry["file"] for entry in linked.get("license_texts", [])]
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: {linked['name']} license text is missing from the "
                f"source tree; {COMPONENTS_IDENTITY} declares {declared}",
            )
        slug = notice_file_name(linked)
        component: dict[str, Any] = {
            "name": linked["name"],
            "spdx_id": linked["spdx_id"],
            "source_path": make_relative_path(sources[0][0], toolchain_root, repo_root),
            "license_file": slug,
            "upstream_origin": linked.get("upstream_origin", "NOASSERTION"),
            "disposition": linked.get("disposition", "linked"),
        }
        if linked.get("upstream_revision"):
            component["version"] = linked["upstream_revision"]
        if linked.get("relink_doc"):
            component["relink_doc"] = linked["relink_doc"]
        components.append(component)
        license_files_to_copy.append((slug, sources[0][0], linked["name"]))
        for extra_src, extra_display in sources[1:]:
            extra_name = f"{Path(slug).stem}-{Path(extra_display).name}"
            component.setdefault("additional_license_files", []).append(extra_name)
            license_files_to_copy.append(
                (extra_name, extra_src, f"{linked['name']} ({Path(extra_display).name})")
            )
        if linked.get("object_glob"):
            component["relink_objects"] = linked["object_glob"]

    # Process bundled binaries
    for binary in binaries:
        bin_name = binary.name
        bin_lower = bin_name.lower()

        if binary.suffix.lower() == ".exe":
            # Recompiled executable: governed by the project license recorded for
            # the package output, and by the LGPL relink obligations above.
            runtime_record = inventory["native"].get("project_output")
            if runtime_record is None:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_RECORD_MISSING",
                    f"LICENSE_RECORD_SOURCE_INCOMPLETE: {COMPONENTS_IDENTITY} declares no "
                    f"native.project_output record for the packaged executable",
                )
            runtime_sources = component_license_sources(runtime_record, toolchain_root, repo_root)
            if not runtime_sources:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_TEXT_MISSING",
                    f"LICENSE_TEXT_MISSING: project license text is missing for the packaged "
                    f"executable {bin_name!r}",
                )
            components.append({
                "name": f"{runtime_record['name']} ({bin_name})",
                "binary": bin_name,
                "spdx_id": runtime_record["spdx_id"],
                "source_path": make_relative_path(runtime_sources[0][0], toolchain_root, repo_root),
                "license_file": runtime_record.get("notice_file", "Nakagawa-Recomp-LICENSE.txt"),
                "upstream_origin": runtime_record.get("upstream_origin", "NOASSERTION"),
                "disposition": runtime_record.get("disposition", "project_output"),
            })
            license_files_to_copy.append((
                runtime_record.get("notice_file", "Nakagawa-Recomp-LICENSE.txt"),
                runtime_sources[0][0],
                runtime_record["name"],
            ))
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

        # Locate the license text files. A caller-supplied license_path wins; every
        # other record resolves through the inventory's license_texts, preferring the
        # in-tree copy so notices generation does not depend on a host toolchain.
        sources: list[tuple[Path, str]] = []
        if "license_path" in record and Path(record["license_path"]).is_file():
            sources.append((Path(record["license_path"]), record["license_path"]))
        else:
            sources = component_license_sources(record, toolchain_root, repo_root)

        if not sources:
            declared = [entry["file"] for entry in record.get("license_texts", [])]
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: license text for bundled binary {bin_name!r} "
                f"({record.get('name')}) was not found in the source tree or the "
                f"local toolchain; {COMPONENTS_IDENTITY} declares {declared}",
            )

        clean_slug = notice_file_name(record)
        target_lic_name = clean_slug
        rel_src = make_relative_path(sources[0][0], toolchain_root, repo_root)

        components.append({
            "name": record["name"],
            "binary": bin_name,
            "version": record.get("version")
            or installed_package_version(toolchain_root, record.get("pacman_package")),
            "spdx_id": record["spdx_id"],
            "source_path": rel_src,
            "license_file": target_lic_name,
            "upstream_origin": record.get("upstream_origin", "NOASSERTION"),
            "disposition": record.get("disposition", "bundled_binary"),
        })
        license_files_to_copy.append((target_lic_name, sources[0][0], record["name"]))
        for extra_src, extra_display in sources[1:]:
            extra_name = f"{Path(clean_slug).stem}-{Path(extra_display).name}"
            components[-1].setdefault("additional_license_files", []).append(extra_name)
            license_files_to_copy.append(
                (extra_name, extra_src, f"{record['name']} ({Path(extra_display).name})")
            )

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

    # Emit SOURCE.txt: the exact source of this build (#421 decision).
    reference = source_reference(repo_root)
    source_content = render_source_notice(reference)
    (package_dir / "SOURCE.txt").write_text(source_content, encoding="utf-8", newline="\n")
    (notices_dir / "SOURCE.txt").write_text(source_content, encoding="utf-8", newline="\n")

    # Emit RELINK.md into package root and THIRD_PARTY_NOTICES
    relink_content = render_relink_guide(repo_root)
    (package_dir / "RELINK.md").write_text(relink_content, encoding="utf-8", newline="\n")
    (notices_dir / "RELINK.md").write_text(relink_content, encoding="utf-8", newline="\n")

    # Emit THIRD_PARTY_NOTICES/index.json
    index_data = {
        "schema_version": 1,
        "bundle_type": "native_package_third_party_notices",
        "source": reference,
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
        "index, SOURCE.txt for where this build's complete source is, and RELINK.md",
        "for LGPL relinking documentation.",
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
