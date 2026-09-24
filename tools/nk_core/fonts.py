# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""PSP firmware PGF system font provisioning, structural validation, and local cache contract."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
from typing import Any, Dict, Optional, Set, Tuple

# Expected PSP firmware PGF font names derived from runtime s_font_specs (hle.c)
# and PSP flash0:/font/ firmware layouts (jpn0.pgf, kr0.pgf, ltn0-15.pgf).
EXPECTED_FONT_NAMES: Set[str] = frozenset(
    {"jpn0.pgf", "kr0.pgf", *(f"ltn{i}.pgf" for i in range(16))}
)

MANIFEST_SCHEMA_VERSION = 1


class FontValidationError(ValueError):
    """Raised when a PGF font file fails structural validation."""


class FontImportError(RuntimeError):
    """Raised when font import cannot find or stage font files."""


def default_user_data_root() -> Path:
    """Return canonical per-user application data root matching C nk_platform_get_path."""
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or os.environ.get("USERPROFILE")
        if not base:
            base = str(Path.home() / "AppData" / "Local")
        return Path(base) / "Nakagawa" / "data"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "NakagawaRecomp" / "data"
    xdg = os.environ.get("XDG_DATA_HOME")
    return Path(xdg) / "nakagawa-recomp" if xdg else Path.home() / ".local" / "share" / "nakagawa-recomp"


def get_font_cache_dir(user_data_root: Optional[Path] = None) -> Path:
    """Return versioned per-user font cache directory: <user data>/fonts/v1."""
    root = (user_data_root or default_user_data_root()).expanduser().resolve(strict=False)
    return root / "fonts" / "v1"


def validate_pgf_data(data: bytes, filename: str = "PGF") -> Dict[str, Any]:
    """Structurally validate PGF font bytes without relying on firmware hash tables.

    Checks:
    - Minimum size for PGF header fields
    - Little-endian header offset and header size
    - Declared header size does not exceed file size
    - PGF0 magic signature at header_offset + 4
    - Non-negative revision and version fields
    - first_glyph <= last_glyph bounds (when header size >= 186)
    """
    file_size = len(data)
    if file_size < 8:
        raise FontValidationError(
            f"Font file '{filename}' is too small ({file_size} bytes; minimum header is 8 bytes)."
        )

    header_offset, header_size = struct.unpack_from("<HH", data, 0)
    if header_size < 8:
        raise FontValidationError(
            f"Font file '{filename}' declares an invalid header size ({header_size} bytes; minimum is 8)."
        )

    if header_offset + header_size > file_size:
        raise FontValidationError(
            f"Font file '{filename}' is truncated: declared header end "
            f"({header_offset + header_size} bytes) exceeds file size ({file_size} bytes)."
        )

    magic = data[header_offset + 4 : header_offset + 8]
    if magic != b"PGF0":
        raise FontValidationError(
            f"Font file '{filename}' has invalid magic {magic!r}; expected b'PGF0'."
        )

    if header_size >= 16:
        revision, version = struct.unpack_from("<ii", data, header_offset + 8)
        if revision < 0 or version < 0:
            raise FontValidationError(
                f"Font file '{filename}' has invalid revision ({revision}) or version ({version})."
            )

    if header_size >= 186:
        first_glyph, last_glyph = struct.unpack_from("<HH", data, header_offset + 182)
        if first_glyph > last_glyph:
            raise FontValidationError(
                f"Font file '{filename}' has corrupt glyph indices: first glyph ({first_glyph}) "
                f"exceeds last glyph ({last_glyph})."
            )

    sha256_hex = hashlib.sha256(data).hexdigest()
    return {
        "size": file_size,
        "sha256": sha256_hex,
    }


def validate_pgf_file(file_path: Path) -> Dict[str, Any]:
    """Read and structurally validate a PGF font file from disk."""
    if not file_path.is_file():
        raise FontValidationError(f"Font file '{file_path.name}' does not exist or is not a file.")
    try:
        data = file_path.read_bytes()
    except OSError as exc:
        raise FontValidationError(f"Font file '{file_path.name}' could not be read: {exc}") from exc
    return validate_pgf_data(data, filename=file_path.name)


def inspect_font_cache(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
    cache_dir: Optional[Path] = None,
) -> Tuple[str, str]:
    """Read font cache manifest and report discovery status: ('OK', 'MISSING', or 'INVALID').

    Returns (status, message).
    """
    cdir = cache_dir or get_font_cache_dir(user_data_root)
    manifest_path = cdir / "manifest.json"

    if not manifest_path.is_file():
        # Check fallback path where flagship or developer repo root supplies font/jpn0.pgf
        if fallback_root:
            cand_fallback = fallback_root / "font" / "jpn0.pgf"
            if cand_fallback.is_file():
                return "OK", "User-supplied PSP system font jpn0.pgf is available."
        return "MISSING", "PSP font jpn0.pgf missing; run fonts import <folder> (#300)."

    try:
        text = manifest_path.read_text(encoding="utf-8")
        manifest = json.loads(text)
    except (OSError, json.JSONDecodeError) as exc:
        return "INVALID", f"PSP font cache manifest is unreadable or malformed ({exc}); run fonts import <folder> (#300)."

    if not isinstance(manifest, dict):
        return "INVALID", "PSP font cache manifest must be a JSON object; run fonts import <folder> (#300)."

    schema_version = manifest.get("schema_version")
    if schema_version != MANIFEST_SCHEMA_VERSION:
        return "INVALID", f"PSP font cache schema version mismatch ({schema_version}); run fonts import <folder> (#300)."

    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        return "INVALID", "PSP font cache manifest missing 'files' dictionary; run fonts import <folder> (#300)."

    if "jpn0.pgf" not in files:
        return "INVALID", "PSP font cache manifest does not declare required font jpn0.pgf; run fonts import <folder> (#300)."

    # Verify each declared file on disk
    for name, entry in files.items():
        if not isinstance(name, str) or not isinstance(entry, dict):
            return "INVALID", "PSP font cache manifest contains invalid entry; run fonts import <folder> (#300)."
        font_path = cdir / name
        if not font_path.is_file():
            return "INVALID", f"PSP font file '{name}' declared in manifest is missing from cache; run fonts import <folder> (#300)."
        expected_size = entry.get("size")
        expected_sha = entry.get("sha256")
        if not isinstance(expected_size, int) or not isinstance(expected_sha, str):
            return "INVALID", f"PSP font manifest entry '{name}' has invalid size/sha256; run fonts import <folder> (#300)."

        try:
            validated = validate_pgf_file(font_path)
        except FontValidationError as exc:
            return "INVALID", f"PSP font file '{name}' in cache failed validation: {exc}; run fonts import <folder> (#300)."

        if validated["size"] != expected_size:
            return "INVALID", f"PSP font file '{name}' size mismatch in cache (got {validated['size']}, expected {expected_size}); run fonts import <folder> (#300)."
        if validated["sha256"].lower() != expected_sha.lower():
            return "INVALID", f"PSP font file '{name}' SHA-256 mismatch in cache; run fonts import <folder> (#300)."

    return "OK", "User-supplied PSP system font jpn0.pgf is available."


def scan_source_folder(folder: Path) -> Dict[str, Path]:
    """Scan folder and common dump subdirectories for expected PSP firmware font files."""
    if not folder.is_dir():
        raise FontImportError(f"Font source path is not a directory: '{folder}'")

    search_dirs = [
        folder,
        folder / "font",
        folder / "FONT",
        folder / "flash0" / "font",
        folder / "flash0" / "FONT",
    ]

    found: Dict[str, Path] = {}
    for sdir in search_dirs:
        if not sdir.is_dir():
            continue
        try:
            for entry in sdir.iterdir():
                if entry.is_file():
                    lower_name = entry.name.lower()
                    if lower_name in EXPECTED_FONT_NAMES and lower_name not in found:
                        found[lower_name] = entry
        except OSError as exc:
            raise FontImportError(f"Error reading directory '{sdir}': {exc}") from exc

    return found


def import_fonts(
    source_folder: Path,
    user_data_root: Optional[Path] = None,
) -> Dict[str, Any]:
    """Import and validate PSP firmware fonts from source folder into local cache directory.

    Re-import is idempotent.
    A partially failed import leaves the previous cache intact.
    """
    matched = scan_source_folder(source_folder)
    if not matched:
        expected_sample = ", ".join(sorted(EXPECTED_FONT_NAMES)[:5]) + "..."
        raise FontImportError(
            f"No PSP firmware PGF font files found in '{source_folder}'. "
            f"Expected files like: {expected_sample}"
        )

    # 1. Structural validation of ALL source files first.
    # If any file fails, abort immediately before touching cache!
    validated_entries: Dict[str, Tuple[Path, Dict[str, Any]]] = {}
    for canonical_name, src_path in sorted(matched.items()):
        val = validate_pgf_file(src_path)
        validated_entries[canonical_name] = (src_path, val)

    # 2. Prepare target cache directory
    cache_dir = get_font_cache_dir(user_data_root)
    cache_dir.mkdir(parents=True, exist_ok=True)

    # If an existing manifest is valid, merge existing files so incremental imports
    # preserve previously imported fonts.
    manifest_path = cache_dir / "manifest.json"
    files_map: Dict[str, Dict[str, Any]] = {}
    if manifest_path.is_file():
        try:
            old_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            if isinstance(old_manifest, dict) and old_manifest.get("schema_version") == MANIFEST_SCHEMA_VERSION:
                old_files = old_manifest.get("files")
                if isinstance(old_files, dict):
                    files_map.update(old_files)
        except Exception:
            pass

    # 3. Copy validated files using temporary staging files in cache directory
    staged_copies: list[Tuple[Path, Path]] = []
    try:
        for canonical_name, (src_path, _val) in validated_entries.items():
            dest_final = cache_dir / canonical_name
            dest_tmp = cache_dir / f"{canonical_name}.tmp"
            data = src_path.read_bytes()
            dest_tmp.write_bytes(data)
            staged_copies.append((dest_tmp, dest_final))

        # Commit file renames
        for dest_tmp, dest_final in staged_copies:
            os.replace(dest_tmp, dest_final)
            files_map[dest_final.name] = {
                "size": dest_final.stat().st_size,
                "sha256": validated_entries[dest_final.name][1]["sha256"],
            }

        # 4. Atomically write manifest.json
        manifest_data = {
            "schema_version": MANIFEST_SCHEMA_VERSION,
            "import_time": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "files": files_map,
        }
        manifest_tmp = cache_dir / "manifest.json.tmp"
        manifest_tmp.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")
        os.replace(manifest_tmp, manifest_path)
    except Exception as exc:
        # Cleanup temporary files on failure
        for dest_tmp, _ in staged_copies:
            try:
                if dest_tmp.is_file():
                    dest_tmp.unlink()
            except OSError:
                pass
        manifest_tmp = cache_dir / "manifest.json.tmp"
        if manifest_tmp.is_file():
            try:
                manifest_tmp.unlink()
            except OSError:
                pass
        raise FontImportError(f"Failed to stage imported fonts into cache: {exc}") from exc

    return {
        "status": "OK",
        "cache_dir": str(cache_dir),
        "imported_count": len(validated_entries),
        "files": files_map,
    }


def resolve_font_directory(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
) -> Optional[Path]:
    """Resolve font directory for runtime execution: cache first, then fallback."""
    cdir = get_font_cache_dir(user_data_root)
    status, _ = inspect_font_cache(cache_dir=cdir)
    if status == "OK":
        return cdir
    if fallback_root:
        fallback = fallback_root / "font"
        if fallback.is_dir() and (fallback / "jpn0.pgf").is_file():
            return fallback
    return None
