#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Headless CLI interface for Nakagawa Recomp title inspection, preparation, and launch."""

from __future__ import annotations

import argparse
from collections import Counter
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from nk_core import (
    PreparationEngine,
    ProgressEvent,
    RuntimeLauncher,
    inspect_iso,
)
from nk_core.iso_inspect import (
    MAX_EXECUTABLE_BYTES,
    IsoInspectionError,
    IsoDirectoryEntry,
    _elf32_mips_usable,
    _lookup_iso_file,
    _read_iso_extent,
    _classify_decrypted_elf_file,
    _has_cfw_or_kernel_only_imports,
    decrypted_module_dir,
    inspect_compatibility_preflight,
    list_iso_directory,
    plan_provisional_module_bindings,
    write_experimental_profile,
)
import title_manifest


ROOT = Path(__file__).resolve().parent.parent
PACKAGE_CACHE_MARKER = ".nk-aot-package-cache-v1"
MAX_GUEST_MODULES = 32
MAX_GUEST_MODULE_BYTES = 256 * 1024 * 1024
MAX_GUEST_MODULE_SET_BYTES = 512 * 1024 * 1024


class PackageBuildError(ValueError):
    """A named, fail-closed package build refusal."""


def default_user_data_root() -> Path:
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or os.environ.get("USERPROFILE")
        if not base:
            raise PackageBuildError("Windows user data directory is unavailable.")
        return Path(base) / "Nakagawa" / "data"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "NakagawaRecomp" / "data"
    xdg = os.environ.get("XDG_DATA_HOME")
    return Path(xdg) / "nakagawa-recomp" if xdg else Path.home() / ".local" / "share" / "nakagawa-recomp"


def _user_data_root(value: Path | None) -> Path:
    root = (value or default_user_data_root()).expanduser().resolve(strict=False)
    if ROOT == root or ROOT in root.parents:
        raise PackageBuildError("Package outputs must stay in per-user data, outside the repository.")
    return root


def _require_child(root: Path, child: Path, label: str) -> Path:
    resolved = child.resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise PackageBuildError(f"{label} escaped the per-user data directory.") from exc
    return resolved


def _write_private_file(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    if os.name != "nt":
        temporary.chmod(0o600)
    os.replace(temporary, path)


def _extract_iso_executable(iso_path: Path, selected: str, destination: Path) -> str:
    if selected not in {"EBOOT.BIN", "BOOT.BIN"}:
        raise PackageBuildError(f"Unsupported selected executable {selected!r}; a plaintext ELF is required (#295).")
    member = ("PSP_GAME", "SYSDIR", selected)
    try:
        file_size = iso_path.stat().st_size
        with iso_path.open("rb") as source:
            extent = _lookup_iso_file(source, file_size, member)
            if extent is None:
                raise PackageBuildError(f"Selected executable {selected} is not reachable in the ISO directory tree.")
            lba, size = extent
            if size <= 0 or size > MAX_EXECUTABLE_BYTES:
                raise PackageBuildError("Selected executable exceeds the supported extraction bound.")
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            temporary = destination.with_name(destination.name + ".tmp")
            digest = hashlib.sha256()
            with temporary.open("wb") as output:
                offset = 0
                while offset < size:
                    count = min(64 * 1024, size - offset)
                    block = _read_iso_extent(source, file_size, lba, size, offset, count)
                    if len(block) != count:
                        raise PackageBuildError("Selected executable could not be extracted completely.")
                    output.write(block)
                    digest.update(block)
                    offset += count
                output.flush()
                os.fsync(output.fileno())
            if os.name != "nt":
                temporary.chmod(0o600)
            os.replace(temporary, destination)
            return digest.hexdigest()
    except IsoInspectionError as exc:
        raise PackageBuildError(f"Selected executable is not a plaintext ELF; decryption support is in the works (#295): {exc}") from exc
    except OSError as exc:
        raise PackageBuildError(f"Could not extract the selected executable to the private cache: {exc}") from exc


def _discover_iso_module_candidates(iso_path: Path, selected: str) -> list[dict]:
    """Find bounded ELF/PRX candidates in the title's own SYSDIR and USRDIR."""
    file_size = iso_path.stat().st_size
    candidates: list[dict] = []
    selected_name = Path(selected).name.casefold()
    module_directories = (
        ("PSP_GAME", "SYSDIR"),
        ("PSP_GAME", "SYSDIR", "PRX"),
        ("PSP_GAME", "USRDIR"),
        ("PSP_GAME", "USRDIR", "PRX"),
    )
    for directory in module_directories:
        entries = list_iso_directory(iso_path, directory) or []
        for entry in entries:
            if entry.is_directory or Path(entry.name).suffix.casefold() not in {".prx", ".elf"}:
                continue
            if entry.name.casefold() in {selected_name, "boot.bin"}:
                continue
            if not title_manifest.FILENAME_RE.fullmatch(entry.name) or \
                    entry.name.endswith(".") or \
                    entry.name.split(".", 1)[0].upper() in title_manifest.WINDOWS_RESERVED:
                kind = "unsupported"
            elif entry.multi_extent or entry.size <= 0 or entry.size > MAX_GUEST_MODULE_BYTES:
                kind = "unsupported"
            else:
                header_size = min(entry.size, 0x64)
                with iso_path.open("rb") as stream:
                    header = _read_iso_extent(
                        stream, file_size, entry.lba, entry.size, 0, header_size
                    )
                    if header.startswith(b"\x7fELF"):
                        if not _elf32_mips_usable(stream, file_size, entry.lba, entry.size):
                            kind = "unsupported"
                        else:
                            module_bytes = _read_iso_extent(
                                stream, file_size, entry.lba, entry.size, 0, entry.size
                            )
                            if len(module_bytes) != entry.size:
                                kind = "unsupported"
                            elif _has_cfw_or_kernel_only_imports(module_bytes):
                                continue
                            else:
                                kind = "plain-elf"
                    elif header.startswith(b"~SCE"):
                        kind = "encrypted-prx"
                    elif header.startswith(b"~PSP"):
                        kind = "encrypted-prx" if _encrypted_prx_header_supported(
                            header
                        ) else "unsupported"
                    else:
                        kind = "unsupported"
            candidates.append({
                "name": entry.name,
                "directory": directory,
                "entry": entry,
                "kind": kind,
            })
            if len(candidates) > MAX_GUEST_MODULES:
                raise PackageBuildError("The ISO contains more than 32 guest-module candidates (#296).")

    names: set[str] = set()
    for candidate in candidates:
        folded = candidate["name"].casefold()
        if folded in names:
            raise PackageBuildError("Guest-module filenames collide across ISO directories (#308).")
        names.add(folded)
    staged_size = sum(
        candidate["entry"].size for candidate in candidates
        if candidate["kind"] == "plain-elf"
    )
    if staged_size > MAX_GUEST_MODULE_SET_BYTES:
        raise PackageBuildError("Guest-module inputs exceed the supported aggregate size (#296).")
    return candidates


def _encrypted_prx_header_supported(header: bytes) -> bool:
    if len(header) < 0x64 or not 1 <= header[0x27] <= 4:
        return False
    sizes = struct.unpack_from("<4I", header, 0x54)[: header[0x27]]
    return all(0 < value <= MAX_EXECUTABLE_BYTES for value in sizes) and \
        sum(sizes) <= MAX_EXECUTABLE_BYTES


def _stage_iso_modules(
    iso_path: Path, user_root: Path, disc_id: str, candidates: list[dict]
) -> Path:
    profile_dir = _require_child(
        user_root, user_root / "experimental" / disc_id, "Experimental profile directory"
    )
    profile_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    module_dir = Path(tempfile.mkdtemp(prefix="module-stage-", dir=profile_dir))
    file_size = iso_path.stat().st_size
    with iso_path.open("rb") as source:
        for candidate in candidates:
            if candidate["kind"] != "plain-elf":
                continue
            entry: IsoDirectoryEntry = candidate["entry"]
            destination = _require_child(
                user_root, module_dir / candidate["name"], "Guest module output"
            )
            temporary = destination.with_name(destination.name + ".tmp")
            try:
                with temporary.open("wb") as output:
                    offset = 0
                    while offset < entry.size:
                        count = min(64 * 1024, entry.size - offset)
                        block = _read_iso_extent(
                            source, file_size, entry.lba, entry.size, offset, count
                        )
                        output.write(block)
                        offset += count
                    output.flush()
                    os.fsync(output.fileno())
                if os.name != "nt":
                    temporary.chmod(0o600)
                os.replace(temporary, destination)
            except (OSError, IsoInspectionError):
                temporary.unlink(missing_ok=True)
                raise
    return module_dir

def _copy_decrypted_elf(source: Path, destination: Path) -> str:
    """Copy one bounded user ELF into the private package cache after validation."""
    try:
        size = source.stat().st_size
        if size <= 0 or size > MAX_EXECUTABLE_BYTES:
            raise PackageBuildError("User-supplied decrypted EBOOT.elf exceeds the supported size bound (#295).")
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        temporary = destination.with_name(destination.name + ".tmp")
        digest = hashlib.sha256()
        copied = 0
        with source.open("rb") as input_stream, temporary.open("wb") as output:
            while copied < size:
                block = input_stream.read(min(64 * 1024, size - copied))
                if not block:
                    raise PackageBuildError("User-supplied decrypted EBOOT.elf changed while being copied (#295).")
                output.write(block)
                digest.update(block)
                copied += len(block)
            if input_stream.read(1):
                raise PackageBuildError("User-supplied decrypted EBOOT.elf changed while being copied (#295).")
            output.flush()
            os.fsync(output.fileno())
        if _classify_decrypted_elf_file(temporary) != "PLAIN_MIPS_ELF32":
            temporary.unlink(missing_ok=True)
            raise PackageBuildError(
                "User-supplied decrypted EBOOT.elf is not a usable MIPS ELF32; "
                "the encrypted executable boundary is in the works (#295)."
            )
        if os.name != "nt":
            temporary.chmod(0o600)
        os.replace(temporary, destination)
        return digest.hexdigest()
    except OSError as exc:
        raise PackageBuildError(f"Could not copy user-supplied decrypted EBOOT.elf: {exc}") from exc


def _find_public_manifest(title_id: str) -> tuple[Path, dict]:
    manifest_root = ROOT / "assets" / "titles"
    for path in sorted(manifest_root.glob("*.json")):
        manifest = title_manifest.load_manifest(path)
        if manifest["id"] == title_id:
            return path, manifest
    raise PackageBuildError(f"No public title manifest matches library identity {title_id!r} (#308).")


def _load_library_entry(user_root: Path, disc_id: str) -> dict:
    library_path = user_root / "library.json"
    try:
        payload = json.loads(library_path.read_text(encoding="utf-8"),
                             object_pairs_hook=title_manifest.no_duplicate_keys)
    except OSError as exc:
        raise PackageBuildError(f"Could not read the per-user library at {library_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise PackageBuildError(f"The per-user library is not valid JSON: {exc}") from exc
    if not isinstance(payload, dict) or type(payload.get("schema_version")) is not int or payload["schema_version"] != 1:
        raise PackageBuildError("The per-user library has an unsupported schema_version.")
    games = payload.get("games")
    if not isinstance(games, list):
        raise PackageBuildError("The per-user library games field must be an array.")
    matches = [game for game in games if isinstance(game, dict) and str(game.get("disc_id", "")).upper() == disc_id]
    if len(matches) != 1:
        raise PackageBuildError(f"Library entry {disc_id} was not found uniquely.")
    entry = matches[0]
    for field in ("title_id", "iso_path"):
        if not isinstance(entry.get(field), str) or not entry[field]:
            raise PackageBuildError(f"Library entry {disc_id} is missing {field}; re-import the ISO before building (#297).")
    if not isinstance(entry.get("selected_executable", ""), str):
        raise PackageBuildError(f"Library entry {disc_id} has an invalid selected_executable (#297).")
    if type(entry.get("is_experimental", False)) is not bool:
        raise PackageBuildError(f"Library entry {disc_id} has an invalid experimental marker.")
    return entry


def _load_entry_manifest(user_root: Path, entry: dict, disc_id: str, selected: str) -> tuple[Path, dict, str | None]:
    title_id = entry["title_id"]
    profile_path = user_root / "experimental" / disc_id / "profile.json"
    if entry.get("is_experimental"):
        if not profile_path.is_file():
            raise PackageBuildError(f"Experimental profile for {disc_id} is missing; re-import the ISO before building (#297).")
        try:
            if profile_path.stat().st_size > 256 * 1024:
                raise PackageBuildError(f"Experimental profile for {disc_id} exceeds the supported size limit.")
            profile = json.loads(profile_path.read_text(encoding="utf-8"),
                                 object_pairs_hook=title_manifest.no_duplicate_keys)
        except (OSError, json.JSONDecodeError) as exc:
            raise PackageBuildError(f"Experimental profile for {disc_id} is unreadable: {exc}") from exc
        identity = profile.get("input_identity") if isinstance(profile, dict) else None
        if not isinstance(identity, dict) or identity.get("disc_id") != disc_id:
            raise PackageBuildError(f"Experimental profile identity is stale for {disc_id}.")
        profile_selected = str(identity.get("selected_executable", "")).rsplit("/", 1)[-1]
        if profile_selected != selected:
            raise PackageBuildError(f"Experimental profile selected executable is stale for {disc_id}.")
        digest = identity.get("executable_sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PackageBuildError(f"Experimental profile has no plaintext executable SHA-256 for {disc_id} (#295).")
        manifest = title_manifest.validate_manifest(profile.get("manifest"))
        if manifest["id"] != title_id or manifest["disc"]["id"] != disc_id:
            raise PackageBuildError(f"Experimental profile title identity does not match {disc_id}.")
        return profile_path, manifest, digest
    manifest_path, manifest = _find_public_manifest(title_id)
    if manifest.get("disc", {}).get("id") != disc_id and disc_id not in manifest.get("disc", {}).get("compatible_revisions", []):
        raise PackageBuildError(f"Public manifest identity does not match library disc {disc_id}.")
    return manifest_path, manifest, None


def _copy_optional_modules(iso_path: Path, manifest: dict, cache_dir: Path,
                           module_dir_arg: Path | None,
                           default_module_dir: Path | None = None) -> Path | None:
    modules = [module for module in manifest.get("modules", [])
               if module.get("role") == "guest-prx" and module.get("required", False)]
    if not modules:
        return None
    output = cache_dir / "modules"
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    for module in modules:
        name = module["name"]
        module_dir = module_dir_arg or default_module_dir
        candidates = (module_dir / name,) if module_dir else ()
        source_path = next((path for path in candidates if path.is_file()), None)
        if source_path is not None and module_dir_arg is None and default_module_dir is not None:
            try:
                source_path.resolve(strict=True).relative_to(
                    default_module_dir.resolve(strict=False)
                )
            except (OSError, ValueError) as exc:
                raise PackageBuildError(
                    f"Required guest PRX {name} escapes the per-title decrypted-module folder (#295)."
                ) from exc
        destination = output / name
        if source_path is not None:
            if source_path.stat().st_size <= 0 or source_path.stat().st_size > MAX_EXECUTABLE_BYTES:
                raise PackageBuildError(
                    f"Required guest PRX {name} exceeds the supported input size (#295); "
                    "broader module intake is in the works."
                )
            try:
                with source_path.open("rb") as module_stream:
                    is_elf = module_stream.read(4) == b"\x7fELF"
            except OSError as exc:
                raise PackageBuildError(f"Could not read required guest PRX {name}: {exc}") from exc
            if not is_elf:
                suggested = module_dir_arg or default_module_dir
                raise PackageBuildError(
                    f"Required guest PRX {name} is not a decrypted ELF; supply decrypted modules "
                    f"at {suggested} (#295). Decrypted module intake is in the works."
                )
            _write_private_file(destination, source_path.read_bytes())
            continue
        extracted = False
        members = []
        guest_path = module.get("guest_path")
        if isinstance(guest_path, str) and ":" in guest_path:
            device, relative = guest_path.split(":", 1)
            guest_components = tuple(component for component in relative.split("/") if component)
            if device.casefold() in {"disc0", "umd0"} and \
                    guest_components[:1] and guest_components[0].casefold() == "psp_game":
                members.append(guest_components)
        members.extend((
            ("PSP_GAME", "SYSDIR", name),
            ("PSP_GAME", "SYSDIR", "PRX", name),
            ("PSP_GAME", "USRDIR", name),
            ("PSP_GAME", "USRDIR", "PRX", name),
        ))
        for member in dict.fromkeys(members):
            try:
                file_size = iso_path.stat().st_size
                with iso_path.open("rb") as stream:
                    extent = _lookup_iso_file(stream, file_size, member)
                    if extent is None:
                        continue
                    lba, size = extent
                    if size <= 0 or size > MAX_EXECUTABLE_BYTES:
                        break
                    temporary = destination.with_name(destination.name + ".tmp")
                    with temporary.open("wb") as output_stream:
                        offset = 0
                        while offset < size:
                            count = min(64 * 1024, size - offset)
                            block = _read_iso_extent(stream, file_size, lba, size, offset, count)
                            if len(block) != count:
                                raise PackageBuildError(f"Could not extract required guest PRX {name} completely.")
                            output_stream.write(block)
                            offset += count
                    with temporary.open("rb") as module_stream:
                        is_elf = module_stream.read(4) == b"\x7fELF"
                    if not is_elf:
                        temporary.unlink(missing_ok=True)
                        suggested = module_dir_arg or default_module_dir
                        raise PackageBuildError(
                            f"Required guest PRX {name} is encrypted or not a plain ELF; "
                            f"supply decrypted modules at {suggested} (#295). "
                            "Decrypted module intake is in the works."
                        )
                    os.replace(temporary, destination)
                    extracted = True
                    break
            except OSError:
                continue
        if not extracted and source_path is None:
            suggested = module_dir_arg or default_module_dir
            raise PackageBuildError(
                f"Required guest PRX {name} is unavailable; supply decrypted modules at "
                f"{suggested} (#295). Decrypted module intake is in the works."
            )
    return output


def _runtime_build_environment() -> dict[str, str]:
    env = os.environ.copy()
    if os.name == "nt":
        ucrt_bin = Path("C:/msys64/ucrt64/bin")
        if ucrt_bin.is_dir():
            env["PATH"] = str(ucrt_bin) + os.pathsep + env.get("PATH", "")
    return env


def _stage_runtime_assets(package_dir: Path) -> None:
    vfpu_source = ROOT / "assets" / "vfpu"
    if vfpu_source.is_dir():
        shutil.copytree(vfpu_source, package_dir / "assets" / "vfpu", dirs_exist_ok=True)
    if os.name == "nt" and not (package_dir / "SDL3.dll").is_file():
        candidates = [Path(os.environ["SDL3_DLL"])] if os.environ.get("SDL3_DLL") else []
        candidates.extend((Path("C:/msys64/ucrt64/bin/SDL3.dll"),))
        found = next((path for path in candidates if path.is_file()), None)
        if found is None:
            raise PackageBuildError("SDL3.dll was not bundled in the package and could not be resolved; install the SDL3 runtime used by #296.")
        shutil.copyfile(found, package_dir / "SDL3.dll")


def cmd_build_package(args: argparse.Namespace, stage_observer=None) -> int:
    disc_id = args.disc_id.upper()
    if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", disc_id):
        sys.stderr.write("Build refused: disc ID must be a nine-character PSP ID.\n")
        return 2
    try:
        user_root = _user_data_root(args.user_data_root)
        entry = _load_library_entry(user_root, disc_id)
        iso_path = Path(entry["iso_path"]).expanduser()
        if not iso_path.is_file():
            raise PackageBuildError(f"The library source ISO is unavailable at {iso_path}.")
        metadata = inspect_iso(iso_path)
        if metadata.disc_id != disc_id:
            raise PackageBuildError(f"ISO identity {metadata.disc_id} no longer matches library entry {disc_id}.")
        preflight = inspect_compatibility_preflight(iso_path, metadata=metadata,
                                                    runtime_root=user_root)
        selected_value = preflight.get("selected_executable")
        uses_decrypted_eboot = selected_value == "EBOOT.elf"
        decrypted_eboot = preflight.get("decrypted_executable") if uses_decrypted_eboot else None
        if preflight.get("modified_dump_cfw_loader") and not uses_decrypted_eboot:
            raise PackageBuildError(
                "This disc image was modified by a custom-firmware patch. The original "
                "executable is EBOOT.OLD (encrypted); supply its decrypted form at "
                "titles/<DISC_ID>/decrypted/EBOOT.elf in user data, or use a clean dump. "
                "This boundary is "
                "in the works (#308)."
            )
        if selected_value is None or (uses_decrypted_eboot and not decrypted_eboot):
            module_dir = decrypted_module_dir(user_root, disc_id)
            folder = str(module_dir) if module_dir is not None else "the per-title decrypted-module folder"
            raise PackageBuildError(
                f"Encrypted executable: supply decrypted modules at {folder} (#295). "
                "Automatic decryption is in the works."
            )
        selected = str(selected_value).upper()
        selected_from_library = entry.get("selected_executable", "")
        library_selected = (
            Path(selected_from_library.replace("\\", "/")).name.upper()
            if selected_from_library else ""
        )
        if uses_decrypted_eboot:
            if not library_selected:
                library_selected = "EBOOT.BIN"
            if library_selected not in {"EBOOT.BIN", "BOOT.BIN"}:
                raise PackageBuildError(
                    f"Library executable selection {library_selected!r} is invalid for a user-supplied EBOOT.elf (#297)."
                )
            manifest_selected = library_selected
        else:
            manifest_selected = selected
        if library_selected not in {"EBOOT.BIN", "BOOT.BIN"} or (
            not uses_decrypted_eboot and library_selected != selected
        ):
            raise PackageBuildError(f"Selected executable changed from library entry {library_selected!r} to {selected!r}; re-import the ISO (#297).")

        manifest_source, manifest, expected_hash = _load_entry_manifest(
            user_root, entry, disc_id, manifest_selected
        )
        cache_dir = _require_child(user_root, user_root / "cache" / "packages" / disc_id,
                                   "Package cache")
        cache_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        marker = cache_dir / PACKAGE_CACHE_MARKER
        if not marker.exists():
            if any(cache_dir.iterdir()):
                raise PackageBuildError("Package cache contains files without a recognized ownership marker; refusing to replace it.")
            marker.write_text("Nakagawa Recomp package cache v1\n", encoding="ascii")
        else:
            if marker.read_text(encoding="ascii") != "Nakagawa Recomp package cache v1\n":
                raise PackageBuildError("Package cache marker is not recognized; refusing to replace it.")
        elf_path = cache_dir / "selected.elf"
        actual_hash = (
            _copy_decrypted_elf(Path(str(decrypted_eboot)), elf_path)
            if uses_decrypted_eboot
            else _extract_iso_executable(iso_path, selected, elf_path)
        )
        if expected_hash and actual_hash != expected_hash:
            raise PackageBuildError("Selected executable SHA-256 differs from the experimental profile; re-import the ISO before rebuilding.")

        manifest_cache_path = cache_dir / "manifest.json"
        if manifest_source == manifest_cache_path:
            manifest_cache_path = cache_dir / "manifest-source.json"
        canonical = title_manifest.canonical_json(manifest).encode("utf-8") + b"\n"
        _write_private_file(manifest_cache_path, canonical)

        psp_header = args.psp_header.expanduser().resolve(strict=True) if args.psp_header else None
        if manifest["executable"].get("bss_metadata_source") == "psp-header" and psp_header is None:
            raise PackageBuildError("This manifest requires a PSP header input; provide --psp-header or use a route that exposes it (#296).")
        cached_header = None
        if psp_header is not None:
            cached_header = cache_dir / "selected.psp"
            shutil.copyfile(psp_header, cached_header)
        module_dir = _copy_optional_modules(
            iso_path,
            manifest,
            cache_dir,
            args.module_dir.expanduser().resolve() if args.module_dir else None,
            decrypted_module_dir(user_root, disc_id),
        )

        build_dir = cache_dir / "package"
        target_dir = user_root / "packages" / disc_id
        _require_child(user_root, target_dir, "Package destination")
        if build_dir.exists():
            shutil.rmtree(build_dir)
        command = [
            sys.executable,
            str(ROOT / "tools" / "title_codegen_plan.py"),
            str(manifest_cache_path),
            "--package",
            "--game-elf",
            str(elf_path),
            "--output-dir",
            str(build_dir),
        ]
        if module_dir is not None:
            command.extend(("--module-dir", str(module_dir)))
        if cached_header is not None:
            command.extend(("--psp-header", str(cached_header)))
        if os.name == "nt":
            print("COMMAND: " + subprocess.list2cmdline(command))
        else:
            import shlex
            print("COMMAND: " + shlex.join(command))
        compile_started = time.perf_counter()
        completed = subprocess.run(command, cwd=ROOT, env=_runtime_build_environment(),
                                   capture_output=True, text=True, check=False)
        if completed.stdout:
            sys.stdout.write(completed.stdout)
        if completed.returncode != 0:
            if stage_observer is not None:
                stage_observer(
                    "compile", "FAIL",
                    int((time.perf_counter() - compile_started) * 1000),
                )
            if completed.stderr:
                sys.stderr.write(completed.stderr)
            if "PUBLIC_SAFE=0 requires the private PGF and PGD backends" in (
                completed.stderr + completed.stdout
            ):
                raise PackageBuildError(
                    "This checkout lacks the production PGF/PGD runtime backends required "
                    "for retail packages. Public-safe packages are limited to synthetic "
                    "fixtures; production package building is in the works (#297)."
                )
            if "PACKAGE_UNSUPPORTED_PATH" in completed.stderr or "PACKAGE_UNSUPPORTED_PATH" in completed.stdout:
                output = completed.stderr + completed.stdout
                for line in output.splitlines():
                    if "PACKAGE_UNSUPPORTED_PATH:" in line:
                        raise PackageBuildError(line.split("PACKAGE_UNSUPPORTED_PATH:", 1)[1].strip())
                raise PackageBuildError(
                    "A build path contains spaces and 8.3 short names are unavailable on this volume; "
                    "set NK_BUILD_ROOT to a folder without spaces (#296)."
                )
            return completed.returncode
        if stage_observer is not None:
            stage_observer(
                "compile", "PASS",
                int((time.perf_counter() - compile_started) * 1000),
            )
        package_started = time.perf_counter()
        _stage_runtime_assets(build_dir)

        package_json = json.loads((build_dir / "package.json").read_text(encoding="utf-8"))
        if package_json.get("title", {}).get("id") != entry["title_id"] or \
                package_json.get("inputs", {}).get("executable", {}).get("sha256") != actual_hash:
            raise PackageBuildError("Generated package identity or input hash differs from the library entry.")
        packages_root = user_root / "packages"
        if packages_root.is_symlink():
            raise PackageBuildError("Package destination root is a symlink; refusing to write outside per-user data.")
        packages_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        packages_root = _require_child(user_root, packages_root, "Package destination root")
        staging = Path(tempfile.mkdtemp(prefix=f".{disc_id}.staging-", dir=packages_root))
        shutil.rmtree(staging)
        shutil.copytree(build_dir, staging)
        _require_child(user_root, staging, "Package staging directory")
        backup = None
        if target_dir.exists():
            if target_dir.is_symlink():
                raise PackageBuildError("Existing package destination is a symlink; refusing to replace it.")
            backup = cache_dir / f"previous-{time.time_ns()}"
            os.replace(target_dir, backup)
        try:
            os.replace(staging, target_dir)
        except OSError:
            if backup is not None and not target_dir.exists():
                os.replace(backup, target_dir)
            raise
        print(f"PACKAGE: {target_dir}")
        if stage_observer is not None:
            stage_observer(
                "build_package", "PASS",
                int((time.perf_counter() - package_started) * 1000),
            )
        return 0
    except (PackageBuildError, OSError, ValueError, KeyError, TypeError) as exc:
        sys.stderr.write(f"Package build refused: {exc}\n")
        return 1


def print_progress(event: ProgressEvent) -> None:
    pct_str = f"{event.percentage:.1f}%" if event.percentage is not None else "..."
    sys.stdout.write(f"[{event.stage.value}] {pct_str} {event.operation} - {event.message}\n")
    sys.stdout.flush()


def cmd_fonts_import(args: argparse.Namespace) -> int:
    try:
        from nk_core.fonts import FontImportError, FontValidationError, import_fonts

        user_data_root = args.user_data_root
        result = import_fonts(args.folder, user_data_root=user_data_root)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            names = ", ".join(sorted(result["files"].keys()))
            print(f"Successfully imported {result['imported_count']} font(s) into {result['cache_dir']}: {names}")
        return 0
    except (FontValidationError, FontImportError, OSError, ValueError) as exc:
        sys.stderr.write(f"Font import error: {exc}\n")
        return 1


def cmd_inspect(args: argparse.Namespace) -> int:
    try:
        meta = inspect_iso(args.iso)
        preflight = inspect_compatibility_preflight(
            args.iso, metadata=meta, runtime_root=Path(args.root)
        )
    except Exception as exc:
        sys.stderr.write(f"Error inspecting ISO: {exc}\n")
        return 1

    payload = {
        "disc_id": meta.disc_id,
        "title": meta.title,
        "version": meta.version,
        "region": meta.region,
        "volume_id": meta.volume_id,
        "size_bytes": meta.size_bytes,
        "supported": meta.is_supported,
        "matched_profile": meta.matched_profile.id if meta.matched_profile else None,
        "compatibility_preflight": preflight,
    }
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"Disc ID:    {meta.disc_id}")
        print(f"Title:      {meta.title}")
        print(f"Region:     {meta.region}")
        print(f"Catalogued: {'YES' if meta.is_supported else 'NO'}")
        if meta.matched_profile:
            print(f"Profile:    {meta.matched_profile.name} ({meta.matched_profile.id})")
        print(f"Executable: {preflight['selected_executable'] or 'none'}")
        print("Compatibility preflight:")
        for check in preflight["checks"]:
            print(f"  {check['status']}: {check['message']}")
    return 0 if meta.is_supported else 2


def cmd_prepare(args: argparse.Namespace) -> int:
    engine = PreparationEngine()
    result = engine.prepare_game(
        args.iso,
        on_progress=print_progress,
        destination_root=Path(args.dest) if args.dest else None,
    )
    if result.success:
        print(f"\nPreparation successful! Manifest written to: {result.manifest_path}")
        return 0
    else:
        sys.stderr.write(f"\nPreparation failed [{result.error_code}]: {result.error_message}\n")
        return 1


def cmd_launch(args: argparse.Namespace) -> int:
    launcher = RuntimeLauncher()
    try:
        cmd, env = launcher.build_launch_plan(
            args.game_dir,
            profile=args.profile,
            fps_cap=args.fps_cap,
            software_render=args.software,
        )
        print("Launch plan prepared:")
        print(f"Executable: {cmd[0]}")
        print(f"PSP_ISO:    {env.get('PSP_ISO')}")
        print(f"DATAROOT:   {env.get('SR_DATAROOT')}")
        print(f"FPS_CAP:    {env.get('SR_FPS_CAP')}")
        return 0
    except Exception as exc:
        sys.stderr.write(f"Launch planning error: {exc}\n")
        return 1


BRINGUP_STAGES = (
    "inspect", "prepare_import", "analyze", "codegen", "compile",
    "build_package", "launch",
)
BRINGUP_SCHEMA_PATH = ROOT / "assets" / "bringup_report.schema.json"


def _schema_at_pointer(schema: dict, pointer: str) -> dict:
    value = schema
    for part in pointer.removeprefix("#/").split("/"):
        value = value[part.replace("~1", "/").replace("~0", "~")]
    return value


def _validate_schema_value(value, schema: dict, root_schema: dict, location: str) -> None:
    reference = schema.get("$ref")
    if reference is not None:
        if not reference.startswith("#/"):
            raise ValueError("external schema references are not supported")
        _validate_schema_value(value, _schema_at_pointer(root_schema, reference), root_schema, location)
        return

    expected = schema.get("type")
    if expected is not None:
        expected_types = expected if isinstance(expected, list) else [expected]
        matches = any(
            (kind == "object" and isinstance(value, dict)) or
            (kind == "array" and isinstance(value, list)) or
            (kind == "string" and isinstance(value, str)) or
            (kind == "integer" and isinstance(value, int) and not isinstance(value, bool)) or
            (kind == "number" and isinstance(value, (int, float)) and not isinstance(value, bool)) or
            (kind == "boolean" and isinstance(value, bool)) or
            (kind == "null" and value is None)
            for kind in expected_types
        )
        if not matches:
            raise ValueError(f"{location} has the wrong JSON type")
    if "const" in schema and value != schema["const"]:
        raise ValueError(f"{location} does not match its schema constant")
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(f"{location} is outside its schema enum")
    if isinstance(value, str) and "pattern" in schema and not re.fullmatch(schema["pattern"], value):
        raise ValueError(f"{location} is outside its schema pattern")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            raise ValueError(f"{location} is below its schema minimum")
    if isinstance(value, list):
        if schema.get("uniqueItems") and len(value) != len({json.dumps(item, sort_keys=True) for item in value}):
            raise ValueError(f"{location} contains duplicate items")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(value):
                _validate_schema_value(item, item_schema, root_schema, f"{location}[{index}]")
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        missing = [name for name in required if name not in value]
        if missing:
            raise ValueError(f"{location} is missing a required field")
        property_names = schema.get("propertyNames")
        for name in value:
            if property_names is not None:
                _validate_schema_value(name, property_names, root_schema, f"{location} property name")
            if name in properties:
                _validate_schema_value(value[name], properties[name], root_schema, f"{location}.{name}")
            elif schema.get("additionalProperties") is False:
                raise ValueError(f"{location} contains a non-whitelisted field")
            elif isinstance(schema.get("additionalProperties"), dict):
                _validate_schema_value(
                    value[name], schema["additionalProperties"], root_schema, f"{location}.{name}"
                )


def validate_bringup_report(report: dict) -> None:
    """Validate the report against its checked-in, strict public-safe schema."""
    schema = json.loads(BRINGUP_SCHEMA_PATH.read_text(encoding="utf-8"))
    _validate_schema_value(report, schema, schema, "report")


def _new_bringup_report() -> dict:
    return {
        "schema_version": 1,
        "reached_stage": "none",
        "stages": {
            stage: {"status": "NOT_RUN", "duration_ms": 0}
            for stage in BRINGUP_STAGES
        },
        "preflight_checks": [],
        "failure_class": "NONE",
        "issue_numbers": [],
        "unsupported_imports": [],
        "runtime_imports": [],
        "runtime_output_kind": "NOT_RUN",
        "process_exit_code": None,
        "counts": {
            "functions": None,
            "instructions": None,
            "modules": None,
            "encrypted_modules": None,
            "unsupported_opcodes": {},
        },
        "exit_classification": "NOT_RUN",
    }


def _update_issues(report: dict, values) -> None:
    allowed = {71, 118, 285, 295, 296, 297, 298, 300, 308}
    report["issue_numbers"] = sorted(
        set(report["issue_numbers"]) | {value for value in values if value in allowed}
    )


def _set_bringup_stage(report: dict, stage: str, status: str, duration_ms: int) -> None:
    report["reached_stage"] = stage
    report["stages"][stage] = {
        "status": status,
        "duration_ms": max(0, int(duration_ms)),
    }


def _fail_bringup(report: dict, stage: str, failure_class: str, issues=(), duration_ms=0) -> None:
    _set_bringup_stage(report, stage, "FAIL", duration_ms)
    report["failure_class"] = failure_class
    _update_issues(report, issues)


def _sanitized_checks(preflight: dict) -> list[dict]:
    known_codes = {
        "DISC_SFO", "EXECUTABLE", "EXPERIMENTAL", "RUNTIME_PACKAGE",
        "SYSTEM_FONTS", "AUDIO_OUTPUT", "MODIFIED_DUMP_CFW_LOADER",
    }
    known_status = {"OK", "MISSING", "UNSUPPORTED", "IN_PROGRESS"}
    checks = []
    for check in preflight.get("checks", []):
        if check.get("code") not in known_codes or check.get("status") not in known_status:
            continue
        checks.append({
            "code": check["code"],
            "status": check["status"],
            "issue_numbers": sorted(set(check.get("issues", []))),
        })
    return checks


def _write_bringup_report(report: dict, path: Path) -> None:
    validate_bringup_report(report)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _bringup_human_summary(report: dict) -> str:
    if report["failure_class"] == "NONE":
        return f"Bring-up reached {report['reached_stage']}; launch {report['exit_classification'].lower()}."
    if report["failure_class"] == "MODIFIED_DUMP_CFW_LOADER":
        return (
            "Bring-up stopped at inspect: this disc image was modified by a custom-firmware "
            "patch. The original executable is EBOOT.OLD (encrypted); supply its decrypted "
            "form at titles/<DISC_ID>/decrypted/EBOOT.elf in user data, or use a clean dump. "
            "This boundary is in the works (#308)."
        )
    issues = " ".join(f"#{number}" for number in report["issue_numbers"])
    suffix = f"; in the works ({issues})" if issues else ""
    detail = ""
    if report["failure_class"] == "UNSUPPORTED_IMPORT" and report.get("runtime_imports"):
        imported = report["runtime_imports"][0]
        library = imported["library"] or "unknown PSP library"
        detail = f" ({library}, NID {imported['nid']}"
        if imported["nid_name"]:
            detail += f", {imported['nid_name']}"
        detail += ")"
    elif report["failure_class"] == "ENTRY_NOT_COMPILED":
        detail = " (the selected executable entry has no generated function)"
    elif report["failure_class"] == "RUNTIME_INPUT_UNAVAILABLE":
        detail = f" (runtime could not read a packaged input; exit code {report['process_exit_code']})"
    elif report["failure_class"] == "RUNTIME_ELF_REJECTED":
        detail = f" (runtime rejected the selected ELF load layout; exit code {report['process_exit_code']})"
    elif report["failure_class"] == "RUNTIME_TRACE_UNAVAILABLE":
        detail = f" (runtime could not read a usable reference trace; exit code {report['process_exit_code']})"
    elif report["failure_class"] == "RUNTIME_ARGUMENT_FAILURE":
        detail = f" (runtime rejected its launch arguments; exit code {report['process_exit_code']})"
    elif report["failure_class"] == "NATIVE_RUNTIME_CRASH":
        detail = f" (native runtime reported a crash; exit code {report['process_exit_code']})"
    elif report["failure_class"] == "UNRESOLVED_DISPATCH_TARGET":
        detail = " (the runtime rejected an unresolved dispatch target)"
    elif report["failure_class"] == "EXITED_ZERO_BEFORE_HLE":
        detail = " (the runtime exited zero before its first PSP kernel import)"
    elif report["failure_class"] == "GUEST_ACTIVITY_UNVERIFIED":
        detail = " (runtime telemetry did not verify a PSP kernel import)"
    elif report["failure_class"] == "LAUNCH_FAILED":
        kind = report.get("runtime_output_kind")
        if kind == "EMPTY":
            detail = f" (runtime emitted no diagnostic; exit code {report['process_exit_code']})"
        elif kind == "OTHER":
            detail = f" (runtime output did not match a known boundary; exit code {report['process_exit_code']})"
    return f"Bring-up stopped at {report['reached_stage']}: {report['failure_class']}{detail}{suffix}."


def _write_bringup_library(user_root: Path, iso_path: Path, metadata, title_id: str,
                           selected: str, is_experimental: bool) -> None:
    library_path = user_root / "library.json"
    games = []
    if library_path.exists():
        payload = json.loads(library_path.read_text(encoding="utf-8"),
                             object_pairs_hook=title_manifest.no_duplicate_keys)
        if not isinstance(payload, dict) or payload.get("schema_version") != 1 or \
                not isinstance(payload.get("games"), list):
            raise PackageBuildError("The bring-up user library has an unsupported schema.")
        games = [game for game in payload["games"]
                 if not isinstance(game, dict) or
                 str(game.get("disc_id", "")).upper() != metadata.disc_id.upper()]
    games.append({
        "disc_id": metadata.disc_id.upper(),
        "title_id": title_id,
        "iso_path": str(iso_path),
        "selected_executable": selected,
        "is_experimental": is_experimental,
    })
    _write_private_file(library_path, json.dumps({"schema_version": 1, "games": games},
                                                sort_keys=True, separators=(",", ":")).encode("utf-8"))


def _write_experimental_module_bindings(profile_path: Path, profile: dict,
                                         module_bindings: list[dict]) -> dict:
    manifest = title_manifest.validate_manifest({
        **profile["manifest"], "modules": module_bindings,
    })
    profile["manifest"] = manifest
    _write_private_file(
        profile_path,
        (json.dumps(profile, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n")
        .encode("utf-8"),
    )
    return manifest


def _public_import_rows(rows: list[dict]) -> list[dict]:
    identifier = re.compile(r"^[A-Za-z][A-Za-z0-9_.$-]{0,63}$")
    symbol = re.compile(r"^[A-Za-z_][A-Za-z0-9_.$-]{0,95}$")
    result = []
    for row in rows:
        library = row.get("library")
        name = row.get("name")
        if not isinstance(library, str) or not identifier.fullmatch(library):
            continue
        result.append({
            "library": library,
            "nid_name": name if isinstance(name, str) and symbol.fullmatch(name) else None,
        })
    return sorted(result, key=lambda row: (row["library"], row["nid_name"] or ""))


def _runtime_import_rows(output: str, imports: list[dict]) -> list[dict]:
    """Keep only the runtime's structured unknown-NID line and public import IDs."""
    line_pattern = re.compile(
        r"^HLE: unimplemented nid (0x[0-9a-fA-F]{8}) "
        r"\(([A-Za-z_][A-Za-z0-9_.$-]{0,95}|unknown)\) "
        r"\(thread uid 0x[0-9a-fA-F]+\)$"
    )
    identifier = re.compile(r"^[A-Za-z][A-Za-z0-9_.$-]{0,63}$")
    symbol = re.compile(r"^[A-Za-z_][A-Za-z0-9_.$-]{0,95}$")
    by_nid: dict[str, set[str]] = {}
    for row in imports:
        nid = row.get("nid")
        library = row.get("library")
        if (isinstance(nid, str) and re.fullmatch(r"0x[0-9a-fA-F]{8}", nid)
                and isinstance(library, str) and identifier.fullmatch(library)):
            by_nid.setdefault(nid.lower(), set()).add(library)

    result = set()
    for line in output.splitlines():
        match = line_pattern.fullmatch(line.strip())
        if not match:
            continue
        nid, name = match.groups()
        libraries = by_nid.get(nid.lower(), set())
        result.add((next(iter(libraries)) if len(libraries) == 1 else None,
                    nid.lower(), name if name != "unknown" and symbol.fullmatch(name) else None))
    return [
        {"library": library, "nid": nid, "nid_name": name}
        for library, nid, name in sorted(result, key=lambda row: (row[1], row[0] or ""))
    ]


def _runtime_output_kind(output: str, runtime_imports: list[dict]) -> str:
    folded = output.casefold()
    if runtime_imports:
        return "UNIMPLEMENTED_IMPORT"
    if any(re.match(r"^(?:DISPATCH_MISS_NEW\[\d+\]:|--- UNIQUE DISPATCH MISSES SUMMARY \(\d+ entries\) ---)$",
                    line.strip())
           for line in output.splitlines()):
        return "DISPATCH_MISS"
    if "no available video device" in folded or "video driver" in folded:
        return "VIDEO_UNAVAILABLE"
    if "unsupported instruction" in folded or "aot-gap" in folded:
        return "UNSUPPORTED_INSTRUCTION"
    if any(re.match(r"^sr_unimplemented: function 0x[0-9a-f]{8}: ", line.strip())
           for line in output.splitlines()):
        return "UNSUPPORTED_INSTRUCTION"
    if "no recompiled function at entry" in folded:
        return "ENTRY_NOT_COMPILED"
    lines = [line.strip() for line in output.splitlines()]
    if any(line == "invalid read_file params"
           or re.match(r"^(cannot (open|seek|size|rewind) |short read |file too large |allocation failure for )", line)
           for line in lines):
        return "DRIVER_INPUT_READ_FAILURE"
    if any(line in {
        "not an ELF or truncated header",
        "ELF program header entry too small",
        "ELF program header table size overflow",
        "ELF program header table out of range",
        "ELF PT_LOAD filesz exceeds memsz",
        "ELF PT_LOAD data out of range",
    } or line.startswith(("ELF PT_LOAD guest range invalid:", "image guest span invalid:"))
           for line in lines):
        return "DRIVER_ELF_REJECTION"
    if any(line.startswith("no '# init' in ") for line in lines):
        return "DRIVER_TRACE_INPUT_FAILURE"
    if any(line.startswith(("invalid or duplicate --expect-u32 option:", "usage: driver "))
           for line in lines):
        return "DRIVER_ARGUMENT_FAILURE"
    if "=== PSP RECOMPILER CRASH REPORT ===" in output:
        return "NATIVE_CRASH_REPORT"
    return "EMPTY" if not output.strip() else "OTHER"


def _flight_has_hle_import(path: Path | None) -> bool | None:
    """Return whether the private flight bundle proves an HLE import occurred.

    ``None`` means the bundle is missing, malformed, or its ring buffer dropped
    events before any retained HLE import. The bundle itself is never copied
    into the sanitized bring-up report.
    """
    if path is None:
        return None
    try:
        bundle = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    recorder = bundle.get("recorder") if isinstance(bundle, dict) else None
    events = bundle.get("events") if isinstance(bundle, dict) else None
    dropped = recorder.get("dropped") if isinstance(recorder, dict) else None
    if not isinstance(events, list) or isinstance(dropped, bool) or not isinstance(dropped, int) or dropped < 0:
        return None
    if any(
        isinstance(event, dict) and event.get("class") == "hle" and event.get("kind") == 1
        for event in events
    ):
        return True
    return False if dropped == 0 else None


def _count_instructions(sources: list[dict]) -> int:
    return sum((end - start) // 4 for source in sources
               for start, end in source.get("ranges", []))


def _count_unsupported_opcodes(codegen_report: Path, sources: list[dict]) -> dict[str, int]:
    import analyze
    import title_codegen_plan

    if not codegen_report.is_file():
        return {}
    _regions, instructions = title_codegen_plan._read_codegen_fallbacks(codegen_report, sources)
    counts = Counter()
    for row in instructions:
        if row.get("word") is None:
            continue
        word = int(row["word"], 16)
        mnemonic = analyze._cfg_opcode_identity(word)["mnemonic"].upper()
        counts[mnemonic] += 1
    return dict(sorted(counts.items()))


def cmd_bringup(args: argparse.Namespace) -> int:
    """Run the consumer route while writing only schema-checked safe evidence."""
    report = _new_bringup_report()
    report_path = Path(args.report).expanduser().resolve(strict=False)
    try:
        work_dir = _user_data_root(Path(args.work_dir))
        work_dir.mkdir(parents=True, exist_ok=True)
        user_root = _user_data_root(work_dir / "user-data")
        if not user_root.is_relative_to(work_dir):
            raise PackageBuildError("Bring-up user data escaped the selected work directory.")
        user_root.mkdir(parents=True, exist_ok=True)
        iso_path = Path(args.iso).expanduser().resolve(strict=True)
    except (PackageBuildError, OSError, ValueError):
        _fail_bringup(report, "inspect", "INVALID_ISO")
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1

    started = time.perf_counter()
    module_dir: Path | None = None
    try:
        metadata = inspect_iso(iso_path)
        preflight = inspect_compatibility_preflight(
            iso_path, metadata=metadata, runtime_root=user_root
        )
    except Exception:
        elapsed = int((time.perf_counter() - started) * 1000)
        _fail_bringup(report, "inspect", "INVALID_ISO", duration_ms=elapsed)
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    report["preflight_checks"] = _sanitized_checks(preflight)
    checks = {check["code"]: check for check in report["preflight_checks"]}
    disc_ok = checks.get("DISC_SFO", {}).get("status") == "OK"
    executable_ok = checks.get("EXECUTABLE", {}).get("status") == "OK"
    selected = preflight.get("selected_executable")
    if not disc_ok:
        _fail_bringup(report, "inspect", "INVALID_ISO",
                      checks.get("DISC_SFO", {}).get("issue_numbers", []),
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    if preflight.get("modified_dump_cfw_loader") and selected != "EBOOT.elf":
        _fail_bringup(report, "inspect", "MODIFIED_DUMP_CFW_LOADER", [308],
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    if not executable_ok or selected not in {"EBOOT.BIN", "BOOT.BIN", "EBOOT.elf"}:
        exec_issues = checks.get("EXECUTABLE", {}).get("issue_numbers", [])
        failure = "EXECUTABLE_UNSUPPORTED" if exec_issues else "INVALID_ISO"
        _fail_bringup(report, "inspect", failure, exec_issues,
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    _set_bringup_stage(report, "inspect", "PASS", int((time.perf_counter() - started) * 1000))

    started = time.perf_counter()
    try:
        profile_path: Path | None = None
        profile: dict | None = None
        if metadata.matched_profile is None:
            profile_path = write_experimental_profile(
                iso_path, user_root, metadata=metadata
            )
            profile = json.loads(profile_path.read_text(encoding="utf-8"),
                                 object_pairs_hook=title_manifest.no_duplicate_keys)
            manifest = title_manifest.validate_manifest(profile["manifest"])
            title_id = manifest["id"]
            is_experimental = True
        else:
            _manifest_source, manifest = _find_public_manifest(metadata.matched_profile.id)
            title_id = manifest["id"]
            is_experimental = False
        selected_elf = work_dir / "selected.elf"
        if selected == "EBOOT.elf":
            decrypted_eboot = preflight.get("decrypted_executable")
            if not isinstance(decrypted_eboot, str):
                raise PackageBuildError("User-supplied decrypted EBOOT.elf is unavailable (#295).")
            _copy_decrypted_elf(Path(decrypted_eboot), selected_elf)
        else:
            _extract_iso_executable(iso_path, str(selected).upper(), selected_elf)
        if is_experimental:
            try:
                module_candidates = _discover_iso_module_candidates(
                    iso_path, str(selected).upper()
                )
            except (OSError, IsoInspectionError, PackageBuildError):
                _fail_bringup(
                    report, "prepare_import", "GUEST_MODULE_DISCOVERY_FAILED",
                    [296], int((time.perf_counter() - started) * 1000),
                )
                _write_bringup_report(report, report_path)
                print(_bringup_human_summary(report))
                return 1
            report["counts"]["modules"] = len(module_candidates)
            report["counts"]["encrypted_modules"] = sum(
                candidate["kind"] == "encrypted-prx" for candidate in module_candidates
            )
            _update_issues(report, [285, 308])
            if report["counts"]["encrypted_modules"]:
                _fail_bringup(
                    report, "prepare_import", "GUEST_MODULE_DECRYPTION_REQUIRED",
                    [295], int((time.perf_counter() - started) * 1000),
                )
                _write_bringup_report(report, report_path)
                print(_bringup_human_summary(report))
                return 1
            if any(candidate["kind"] == "unsupported" for candidate in module_candidates):
                _fail_bringup(
                    report, "prepare_import", "GUEST_MODULE_FORMAT_UNSUPPORTED",
                    [295, 308], int((time.perf_counter() - started) * 1000),
                )
                _write_bringup_report(report, report_path)
                print(_bringup_human_summary(report))
                return 1
            if module_candidates:
                plain_modules = [
                    candidate for candidate in module_candidates
                    if candidate["kind"] == "plain-elf"
                ]
                try:
                    module_dir = _stage_iso_modules(
                        iso_path, user_root, metadata.disc_id, plain_modules
                    )
                except (OSError, IsoInspectionError, PackageBuildError):
                    _fail_bringup(
                        report, "prepare_import", "GUEST_MODULE_STAGE_FAILED",
                        [296], int((time.perf_counter() - started) * 1000),
                    )
                    _write_bringup_report(report, report_path)
                    print(_bringup_human_summary(report))
                    return 1
                try:
                    module_inputs = [
                        (
                            candidate["name"],
                            module_dir / candidate["name"],
                            "disc0:/" + "/".join((*candidate["directory"], candidate["name"])),
                        )
                        for candidate in plain_modules
                    ]
                    module_bindings = plan_provisional_module_bindings(
                        selected_elf, module_inputs
                    )
                except (IsoInspectionError, OSError):
                    _fail_bringup(
                        report, "prepare_import", "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE",
                        [308], int((time.perf_counter() - started) * 1000),
                    )
                    _write_bringup_report(report, report_path)
                    print(_bringup_human_summary(report))
                    return 1
                if profile_path is None or profile is None:
                    raise PackageBuildError("Experimental guest-module profile is unavailable.")
                try:
                    manifest = _write_experimental_module_bindings(
                        profile_path, profile, module_bindings
                    )
                except (OSError, KeyError, ValueError):
                    _fail_bringup(
                        report, "prepare_import", "GUEST_MODULE_STAGE_FAILED",
                        [296], int((time.perf_counter() - started) * 1000),
                    )
                    _write_bringup_report(report, report_path)
                    print(_bringup_human_summary(report))
                    return 1
        else:
            report["counts"]["modules"] = len(manifest.get("modules", []))
            report["counts"]["encrypted_modules"] = 0
            module_dir = _copy_optional_modules(
                iso_path, manifest,
                user_root / "cache" / "bringup" / metadata.disc_id.upper(), None,
            )
        library_executable = "EBOOT.BIN" if selected == "EBOOT.elf" else str(selected).upper()
        _write_bringup_library(
            user_root, iso_path, metadata, title_id, library_executable, is_experimental
        )
    except Exception as exc:
        failure = "EXPERIMENTAL_IMPORT_FAILED"
        issues = [308]
        if isinstance(exc, IsoInspectionError) and "load binding" in str(exc):
            failure = "RELOCATABLE_ELF_LOAD_BINDING_REQUIRED"
        if checks.get("EXECUTABLE", {}).get("issue_numbers"):
            if failure == "EXPERIMENTAL_IMPORT_FAILED":
                issues = checks["EXECUTABLE"]["issue_numbers"]
        _fail_bringup(report, "prepare_import", failure, issues,
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    _set_bringup_stage(report, "prepare_import", "PASS", int((time.perf_counter() - started) * 1000))
    if not metadata.matched_profile:
        _update_issues(report, [285, 308])

    started = time.perf_counter()
    try:
        import title_codegen_plan
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            sources, analysis_summary, unsupported_imports, _diagnostics = \
                title_codegen_plan._make_input_images(
                    manifest, selected_elf, module_dir, None, set()
                )
        report["counts"]["functions"] = int(analysis_summary["analyzed_functions"])
        report["counts"]["instructions"] = _count_instructions(sources)
        report["unsupported_imports"] = _public_import_rows(unsupported_imports)
    except Exception:
        _fail_bringup(report, "analyze", "ANALYSIS_FAILED", [296],
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    _set_bringup_stage(report, "analyze", "PASS", int((time.perf_counter() - started) * 1000))
    _update_issues(report, [71] if report["unsupported_imports"] else [])

    started = time.perf_counter()
    codegen_dir = work_dir / "codegen-stage"
    try:
        import title_codegen_plan
        codegen_dir.mkdir(parents=True, exist_ok=True)
        if not codegen_dir.resolve().is_relative_to(work_dir):
            raise PackageBuildError("Code generation output escaped the work directory.")
        plan = title_codegen_plan.build_plan(
            manifest,
            game_name=manifest["game_name"],
            game_elf=selected_elf,
            build_dir=codegen_dir,
            codegen_profile=manifest.get("codegen_profile"),
            module_dir=module_dir,
            python_command=sys.executable,
        )
        env = _runtime_build_environment()
        env.update(plan["environment"])
        command = list(plan["commands"]["codegen"])
        if command and command[0] == "python":
            command[0] = sys.executable
        completed = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True, text=True,
            check=False,
        )
        report["counts"]["unsupported_opcodes"] = _count_unsupported_opcodes(
            codegen_dir / f"{manifest['game_name']}_recomp_stubs.txt", sources
        )
        if completed.returncode != 0:
            _fail_bringup(report, "codegen", "CODEGEN_FAILED", [296],
                          int((time.perf_counter() - started) * 1000))
            _write_bringup_report(report, report_path)
            print(_bringup_human_summary(report))
            return 1
    except Exception:
        _fail_bringup(report, "codegen", "CODEGEN_FAILED", [296],
                      int((time.perf_counter() - started) * 1000))
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    _set_bringup_stage(report, "codegen", "PASS", int((time.perf_counter() - started) * 1000))

    observer_events = {}
    def observe_package_stage(stage, status, duration_ms):
        observer_events[stage] = (status, duration_ms)
        _set_bringup_stage(report, stage, status, duration_ms)

    build_args = argparse.Namespace(
        disc_id=metadata.disc_id,
        user_data_root=user_root,
        module_dir=module_dir,
        psp_header=None,
    )
    stdout_capture = io.StringIO()
    stderr_capture = io.StringIO()
    with contextlib.redirect_stdout(stdout_capture), contextlib.redirect_stderr(stderr_capture):
        build_status = cmd_build_package(build_args, stage_observer=observe_package_stage)
    if build_status != 0:
        combined = (stdout_capture.getvalue() + stderr_capture.getvalue()).casefold()
        if "production pgf/pgd runtime backends" in combined:
            failure, stage, issues = "PRODUCTION_RUNTIME_BACKEND_UNAVAILABLE", "compile", [297]
        elif "package_build_failed" in combined or observer_events.get("compile", (None,))[0] == "FAIL":
            failure, stage, issues = "COMPILE_FAILED", "compile", [296]
        else:
            failure, stage, issues = "BUILD_PACKAGE_FAILED", "build_package", [296, 297]
        if report["stages"][stage]["status"] == "NOT_RUN":
            _fail_bringup(report, stage, failure, issues, 0)
        else:
            report["failure_class"] = failure
            _update_issues(report, issues)
        _write_bringup_report(report, report_path)
        print(_bringup_human_summary(report))
        return 1
    if report["stages"]["compile"]["status"] == "NOT_RUN":
        _set_bringup_stage(report, "compile", "PASS", 0)
    if report["stages"]["build_package"]["status"] == "NOT_RUN":
        _set_bringup_stage(report, "build_package", "PASS", 0)

    started = time.perf_counter()
    package_dir = user_root / "packages" / metadata.disc_id.upper()
    try:
        package = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
        executable = package_dir / package["executable"]["path"]
        if not executable.is_file():
            raise PackageBuildError("The generated package executable is missing.")
        env = os.environ.copy()
        env.update({
            "SR_DISPATCH_FATAL": "1",
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "PSP_ISO": str(iso_path),
            "SR_DATAROOT": str(package.get("required_local_assets", [{}])[0].get("path", "data")),
        })
        flight_output: Path | None = None
        try:
            flight_fd, flight_name = tempfile.mkstemp(
                prefix="bringup-flight-", suffix=".json", dir=work_dir
            )
            os.close(flight_fd)
            flight_output = Path(flight_name)
            env["SR_FLIGHT"] = "hle,sched,prx,unsupported,fault,fatal;4096"
            env["SR_FLIGHT_OUTPUT"] = str(flight_output)
        except OSError:
            env.pop("SR_FLIGHT", None)
            env.pop("SR_FLIGHT_OUTPUT", None)
        image = package_dir / f"{Path(package['executable']['path']).stem}_image.bin"
        base = int(manifest["executable"]["base"])
        base_argument = str(base) if base == 0 else f"0x{base:08x}"
        launch_command = [
            str(executable), "--image", str(image), base_argument,
            package["runtime"]["run_entry"], "none", "none", "--sched",
        ]
        timeout = max(1, min(int(args.launch_timeout), 120))
        process = subprocess.Popen(
            launch_command, cwd=package_dir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, creationflags=(subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0),
        )
        try:
            launch_output, _ = process.communicate(timeout=timeout)
            report["runtime_imports"] = _runtime_import_rows(launch_output, unsupported_imports)
            report["runtime_output_kind"] = _runtime_output_kind(
                launch_output, report["runtime_imports"]
            )
            report["process_exit_code"] = process.returncode
            report["exit_classification"] = "EXITED_ZERO" if process.returncode == 0 else "EXITED_NONZERO"
            if process.returncode == 0:
                hle_observed = _flight_has_hle_import(flight_output)
                if hle_observed is False:
                    _fail_bringup(
                        report, "launch", "EXITED_ZERO_BEFORE_HLE", [285, 308],
                        int((time.perf_counter() - started) * 1000),
                    )
                elif hle_observed is None:
                    _fail_bringup(
                        report, "launch", "GUEST_ACTIVITY_UNVERIFIED", [285, 308],
                        int((time.perf_counter() - started) * 1000),
                    )
                else:
                    _set_bringup_stage(report, "launch", "PASS",
                                       int((time.perf_counter() - started) * 1000))
            else:
                folded = launch_output.casefold()
                if "no available video device" in folded or "video driver" in folded:
                    failure, issues = "HEADLESS_UNAVAILABLE", [297]
                    report["exit_classification"] = "HEADLESS_UNAVAILABLE"
                elif (report["runtime_imports"] or "unknown nid" in folded
                      or "unimplemented import" in folded):
                    failure, issues = "UNSUPPORTED_IMPORT", [71]
                elif (report["runtime_output_kind"] == "UNSUPPORTED_INSTRUCTION"
                      or "unsupported instruction" in folded or "aot-gap" in folded):
                    failure, issues = "UNSUPPORTED_INSTRUCTION", [118]
                elif report["runtime_output_kind"] == "ENTRY_NOT_COMPILED":
                    failure, issues = "ENTRY_NOT_COMPILED", [296]
                elif report["runtime_output_kind"] == "DRIVER_INPUT_READ_FAILURE":
                    failure, issues = "RUNTIME_INPUT_UNAVAILABLE", [297]
                elif report["runtime_output_kind"] == "DRIVER_ELF_REJECTION":
                    failure, issues = "RUNTIME_ELF_REJECTED", [296]
                elif report["runtime_output_kind"] == "DRIVER_TRACE_INPUT_FAILURE":
                    failure, issues = "RUNTIME_TRACE_UNAVAILABLE", [297]
                elif report["runtime_output_kind"] == "DRIVER_ARGUMENT_FAILURE":
                    failure, issues = "RUNTIME_ARGUMENT_FAILURE", [297]
                elif report["runtime_output_kind"] == "NATIVE_CRASH_REPORT":
                    failure, issues = "NATIVE_RUNTIME_CRASH", [297]
                elif report["runtime_output_kind"] == "DISPATCH_MISS":
                    failure, issues = "UNRESOLVED_DISPATCH_TARGET", [118]
                else:
                    failure, issues = "LAUNCH_FAILED", [297]
                _fail_bringup(report, "launch", failure, issues,
                              int((time.perf_counter() - started) * 1000))
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            report["exit_classification"] = "TIMED_OUT"
            _fail_bringup(report, "launch", "LAUNCH_TIMEOUT", [297],
                          int((time.perf_counter() - started) * 1000))
    except OSError:
        report["exit_classification"] = "EXITED_NONZERO"
        _fail_bringup(report, "launch", "LAUNCH_FAILED", [297],
                      int((time.perf_counter() - started) * 1000))

    _write_bringup_report(report, report_path)
    print(_bringup_human_summary(report))
    return 0 if report["failure_class"] == "NONE" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Nakagawa Recomp Headless CLI")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    p_fonts = subparsers.add_parser("fonts", help="Manage PSP firmware system fonts")
    fonts_subparsers = p_fonts.add_subparsers(dest="fonts_subcommand", required=True)
    p_fonts_import = fonts_subparsers.add_parser("import", help="Import dumped PSP firmware PGF fonts")
    p_fonts_import.add_argument("folder", type=Path, help="Folder containing dumped PSP firmware PGF fonts")
    p_fonts_import.add_argument("--user-data-root", type=Path, help="Override player per-user data directory")
    p_fonts_import.add_argument("--root", type=Path, dest="user_data_root", help=argparse.SUPPRESS)
    p_fonts_import.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    p_fonts_import.set_defaults(func=cmd_fonts_import)

    p_inspect = subparsers.add_parser("inspect", help="Inspect a PSP ISO image")
    p_inspect.add_argument("iso", help="Path to PSP ISO image")
    p_inspect.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    p_inspect.add_argument("--root", default=str(default_user_data_root()),
                           help="Player per-user data directory for package, font, and decrypted-input checks")
    p_inspect.set_defaults(func=cmd_inspect)

    p_prep = subparsers.add_parser("prepare", help="Prepare an ISO for native execution")
    p_prep.add_argument("iso", help="Path to PSP ISO image")
    p_prep.add_argument("--dest", help="Optional destination games directory")
    p_prep.set_defaults(func=cmd_prepare)

    p_launch = subparsers.add_parser("launch", help="Plan launch arguments for a prepared game")
    p_launch.add_argument("game_dir", help="Path to prepared game directory (containing manifest.json)")
    p_launch.add_argument("--profile", default="Standard", choices=["Standard", "Performance", "Benchmark", "Diagnostics"])
    p_launch.add_argument("--fps-cap", type=int, default=30)
    p_launch.add_argument("--software", action="store_true", help="Use software GE rasterizer")
    p_launch.set_defaults(func=cmd_launch)

    p_build = subparsers.add_parser(
        "build-package", help="Build the generated runtime package for a library disc ID"
    )
    p_build.add_argument("disc_id", help="Nine-character disc ID from the player library")
    p_build.add_argument("--user-data-root", type=Path,
                         help="Override the player per-user data directory")
    p_build.add_argument("--module-dir", type=Path,
                         help="Optional directory containing required guest PRXs")
    p_build.add_argument("--psp-header", type=Path,
                         help="PSP header required by some title manifests")
    p_build.set_defaults(func=cmd_build_package)

    p_bringup = subparsers.add_parser(
        "bringup", help="Run and report a sanitized PSP ISO consumer bring-up"
    )
    p_bringup.add_argument("iso", help="Path to the PSP ISO image")
    p_bringup.add_argument("--work-dir", required=True,
                           help="Private work directory outside the repository")
    p_bringup.add_argument("--report", required=True,
                           help="Destination for the public-safe JSON report")
    p_bringup.add_argument("--launch-timeout", type=int, default=20,
                           help="Hard launch limit in seconds (1..120; default 20)")
    p_bringup.set_defaults(func=cmd_bringup)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
