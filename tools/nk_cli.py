#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Headless CLI interface for Nakagawa Recomp title inspection, preparation, and launch."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
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
    _lookup_iso_file,
    _read_iso_extent,
    _classify_decrypted_elf_file,
    decrypted_module_dir,
    inspect_compatibility_preflight,
)
import title_manifest


ROOT = Path(__file__).resolve().parent.parent
PACKAGE_CACHE_MARKER = ".nk-aot-package-cache-v1"


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
        for member in (("PSP_GAME", "SYSDIR", name), ("PSP_GAME", "SYSDIR", "PRX", name)):
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


def cmd_build_package(args: argparse.Namespace) -> int:
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
        completed = subprocess.run(command, cwd=ROOT, env=_runtime_build_environment(),
                                   capture_output=True, text=True, check=False)
        if completed.stdout:
            sys.stdout.write(completed.stdout)
        if completed.returncode != 0:
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
        return 0
    except (PackageBuildError, OSError, ValueError, KeyError, TypeError) as exc:
        sys.stderr.write(f"Package build refused: {exc}\n")
        return 1


def print_progress(event: ProgressEvent) -> None:
    pct_str = f"{event.percentage:.1f}%" if event.percentage is not None else "..."
    sys.stdout.write(f"[{event.stage.value}] {pct_str} {event.operation} - {event.message}\n")
    sys.stdout.flush()


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Nakagawa Recomp Headless CLI")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

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

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
