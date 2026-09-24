# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Safe, bounded ISO9660 and PARAM.SFO inspector for PSP disc images."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import struct
from typing import Dict, Optional

import title_manifest
from .title_registry import TitleRegistry, get_default_registry
from .types import IsoMetadata


SECTOR_SIZE = 2048
PVD_SECTOR = 16
ISO_MAGIC = b"\x01CD001\x01"
SFO_MAGIC = b"\x00PSF\x01\x01\x00\x00"
MAX_DIRECTORY_BYTES = 512 * 1024
MAX_SFO_BYTES = 64 * 1024
MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024
EXPERIMENTAL_PROFILE_SCHEMA_VERSION = 1
PSP_DEFAULT_MAIN_LOAD_ADDRESS = 0x08804000
SFO_FMT_UTF8_SPECIAL = 0x0004
SFO_FMT_UTF8 = 0x0204
SFO_FMT_UINT32 = 0x0404


class IsoInspectionError(ValueError):
    """Raised when an ISO image is unreadable or malformed."""


_IDENTITY_KEYS = frozenset({"DISC_ID", "TITLE", "DISC_VERSION"})


def parse_param_sfo(data: bytes) -> Dict[str, str]:
    """Parse a bounded PSP PARAM.SFO structure."""
    if len(data) < 20 or data[:8] != SFO_MAGIC:
        return {}

    try:
        key_table_start, data_table_start, entry_count = struct.unpack_from(
            "<III", data, 8
        )
    except struct.error as exc:
        raise IsoInspectionError("truncated PARAM.SFO header") from exc

    table_end = 20 + entry_count * 16
    if (
        entry_count > 256
        or key_table_start < table_end
        or key_table_start > data_table_start
        or data_table_start > len(data)
    ):
        raise IsoInspectionError("PARAM.SFO table bounds are invalid")

    result: Dict[str, str] = {}
    for index in range(entry_count):
        entry_offset = 20 + index * 16
        try:
            key_offset, param_fmt, data_len, max_len, data_offset = struct.unpack_from(
                "<HHIII", data, entry_offset
            )
        except struct.error as exc:
            raise IsoInspectionError("truncated PARAM.SFO entry table") from exc

        if key_offset > data_table_start - key_table_start:
            raise IsoInspectionError("PARAM.SFO key offset is outside the key table")
        key_start = key_table_start + key_offset
        key_end = data.find(b"\0", key_start, data_table_start)
        if key_end < 0:
            raise IsoInspectionError("PARAM.SFO key is not NUL-terminated")
        try:
            key_name = data[key_start:key_end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise IsoInspectionError("PARAM.SFO key is not ASCII") from exc

        if data_offset > len(data) - data_table_start:
            raise IsoInspectionError("PARAM.SFO data offset is outside the data table")
        data_start = data_table_start + data_offset
        if data_len > len(data) - data_start:
            raise IsoInspectionError("PARAM.SFO data length exceeds the buffer")
        if max_len < data_len:
            raise IsoInspectionError("PARAM.SFO data length exceeds max length")
        raw_value = data[data_start : data_start + data_len]

        if key_name in _IDENTITY_KEYS and param_fmt not in (
            SFO_FMT_UTF8,
            SFO_FMT_UTF8_SPECIAL,
        ):
            raise IsoInspectionError(
                f"SFO key '{key_name}' declares parameter format 0x{param_fmt:04x}, "
                "which is not a UTF-8 string"
            )

        if param_fmt == SFO_FMT_UTF8:
            value_bytes = raw_value.rstrip(b"\0")
        elif param_fmt == SFO_FMT_UTF8_SPECIAL:
            value_bytes = raw_value
        else:
            value_bytes = b""

        if param_fmt in (SFO_FMT_UTF8, SFO_FMT_UTF8_SPECIAL):
            try:
                value = value_bytes.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise IsoInspectionError(f"SFO key '{key_name}' is not valid UTF-8") from exc
        elif param_fmt == SFO_FMT_UINT32:
            if data_len < 4:
                raise IsoInspectionError(f"SFO key '{key_name}' has a short integer value")
            value = str(struct.unpack_from("<I", raw_value, 0)[0])
        else:
            value = ""

        if key_name in result:
            if result[key_name] != value:
                raise IsoInspectionError(
                    f"Conflicting duplicate SFO key '{key_name}' rejected as ambiguous: "
                    f"'{result[key_name]}' vs '{value}'"
                )
            continue
        result[key_name] = value

    return result


def _canonical_disc_id(value: str) -> str:
    normalized = "".join(
        char for char in value.strip().upper() if char not in "-_ "
    )
    if title_manifest.DISC_ID_RE.fullmatch(normalized) is None:
        raise IsoInspectionError(
            "PSP_GAME/PARAM.SFO does not contain a valid 9-character DISC_ID"
        )
    return normalized


def inspect_iso(
    iso_path: Path | str,
    registry: Optional[TitleRegistry] = None,
) -> IsoMetadata:
    """Inspect a PSP ISO image and match it against supported title profiles."""
    path = Path(iso_path)
    if not path.is_file():
        raise IsoInspectionError(f"ISO file does not exist: {path}")

    size_bytes = path.stat().st_size
    if size_bytes < 1024 * 1024:  # Under 1 MiB is not a valid PSP UMD image
        raise IsoInspectionError(f"File is too small to be a valid PSP ISO: {size_bytes} bytes")

    reg = registry or get_default_registry()

    with open(path, "rb") as f:
        f.seek(PVD_SECTOR * SECTOR_SIZE)
        pvd_data = f.read(SECTOR_SIZE)
        if len(pvd_data) < SECTOR_SIZE or pvd_data[0:7] != ISO_MAGIC:
            raise IsoInspectionError("Not a valid ISO9660 image (missing PVD descriptor)")

        volume_id = pvd_data[40:72].decode("latin-1", errors="replace").rstrip("\x00 ")
        sfo_entry = _lookup_iso_file(f, size_bytes, ("PSP_GAME", "PARAM.SFO"))

        if sfo_entry is None:
            disc_id = "UNKNOWN"
            title = "Unknown PSP Title"
            version = "1.00"
            identity_structured = False
        else:
            sfo_lba, sfo_size = sfo_entry
            if sfo_size < 20 or sfo_size > MAX_SFO_BYTES:
                raise IsoInspectionError("PSP_GAME/PARAM.SFO has an unsupported size")
            raw_sfo = _read_iso_extent(f, size_bytes, sfo_lba, sfo_size, 0, sfo_size)
            sfo_dict = parse_param_sfo(raw_sfo)
            disc_id = _canonical_disc_id(sfo_dict.get("DISC_ID", ""))
            title = sfo_dict.get("TITLE", "")
            version = sfo_dict.get("DISC_VERSION", "1.00")
            identity_structured = True
            if not title:
                title = f"PSP Title ({disc_id})"

        region = "UNKNOWN"
        upper_disc = disc_id.upper()
        if upper_disc.startswith("UCUS") or upper_disc.startswith("ULUS"):
            region = "NA"
        elif upper_disc.startswith("UCES") or upper_disc.startswith("ULES"):
            region = "EU"
        elif upper_disc.startswith("UCJS") or upper_disc.startswith("ULJS"):
            region = "JP"
        elif upper_disc.startswith("UCAS") or upper_disc.startswith("ULAS"):
            region = "ASIA"

        matched = reg.lookup_by_disc_id(disc_id) if identity_structured else None

        return IsoMetadata(
            disc_id=disc_id,
            title=title,
            version=version,
            region=region,
            volume_id=volume_id,
            size_bytes=size_bytes,
            matched_profile=matched,
        )


def write_experimental_profile(
    iso_path: Path | str,
    user_data_dir: Path | str,
    *,
    metadata: Optional[IsoMetadata] = None,
) -> Path:
    """Write a validated generic profile and local executable identity for an unknown PSP disc.

    The canonical manifest is validated by ``tools/title_manifest.py`` and
    contains no title bindings. Executable hashes and the selected ISO path are
    stored only below the caller's per-user data directory.
    """
    path = Path(iso_path)
    info = metadata or inspect_iso(path)
    if info.matched_profile is not None:
        raise IsoInspectionError("catalogued titles do not need an experimental profile")

    user_root = Path(user_data_dir).expanduser()
    try:
        user_root = user_root.resolve(strict=False)
        repository_root = Path(__file__).resolve().parents[2]
        if user_root == repository_root or repository_root in user_root.parents:
            raise IsoInspectionError("experimental profiles must be stored outside the repository")
    except OSError as exc:
        raise IsoInspectionError("user data directory could not be resolved") from exc

    report = inspect_compatibility_preflight(
        path, metadata=info, runtime_root=user_root
    )
    disc_check = next(check for check in report["checks"] if check["code"] == "DISC_SFO")
    if disc_check["status"] != "OK":
        raise IsoInspectionError("experimental import requires PARAM.SFO reached through the disc directory tree")
    if not title_manifest.DISC_ID_RE.fullmatch(info.disc_id):
        raise IsoInspectionError("PARAM.SFO does not contain a valid PSP disc ID")

    disc_id = info.disc_id.upper()
    profile_id = f"experimental-{disc_id.lower()}"
    region = info.region if info.region in {"JP", "NA", "EU", "KR", "ASIA", "OTHER"} else "OTHER"
    display_name = info.title.strip() if info.title and info.title.strip() else f"PSP Title ({disc_id})"
    if any(ord(char) < 0x20 for char in display_name):
        display_name = f"PSP Title ({disc_id})"
    manifest = title_manifest.validate_manifest({
        "schema_version": 1,
        "id": profile_id,
        "game_name": profile_id,
        "display_name": display_name,
        "kind": "retail",
        "disc": {
            "id": disc_id,
            "region": region,
            "revision_policy": "exact-disc-id",
        },
        "executable": {
            "base": 0,
            "entry": 0,
            "bss_metadata_source": "none",
            "extra_executable_spans": [],
        },
        "modules": [],
        "filesystem": {
            "data_root": "data",
            "memory_stick_root": "savedata",
            "device_prefixes": ["disc0:", "ms0:"],
        },
        "hle_profile": "generic",
        "codegen_profile": "none",
        "feature_requirements": [],
        "verification_profile": "experimental-unverified",
    })

    selected = report["selected_executable"]
    selected_path = ""
    executable_sha256: str | None = None
    elf_sha256: str | None = None
    executable_entry = 0
    executable_load_address: int | None = None
    if selected is not None:
        selected_path = f"PSP_GAME/SYSDIR/{selected}"
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            file_size = path.stat().st_size
            extent = _lookup_iso_file(
                stream, file_size, ("PSP_GAME", "SYSDIR", selected)
            )
            if extent is None:
                raise IsoInspectionError("selected executable disappeared from the ISO directory tree")
            lba, size = extent
            if size <= 0 or size > MAX_EXECUTABLE_BYTES:
                raise IsoInspectionError("selected executable is outside the supported hashing bound")
            header = _read_iso_extent(stream, file_size, lba, size, 0, min(size, 52))
            if len(header) < 52 or header[:7] != b"\x7fELF\x01\x01\x01":
                raise IsoInspectionError("selected executable has no supported little-endian ELF32 header")
            e_type, machine, version = struct.unpack_from("<HHI", header, 16)
            if machine != 8 or version != 1:
                raise IsoInspectionError("selected executable is not a supported MIPS ELF32 image")
            if e_type not in (2, 3, 0xFFA0):
                raise IsoInspectionError(
                    "experimental import needs a user-supplied load binding for unsupported relocatable ELF input (#308)"
                )
            executable_entry = struct.unpack_from("<I", header, 24)[0]
            if e_type in (3, 0xFFA0):
                executable_load_address = PSP_DEFAULT_MAIN_LOAD_ADDRESS
            offset = 0
            while offset < size:
                count = min(64 * 1024, size - offset)
                chunk = _read_iso_extent(stream, file_size, lba, size, offset, count)
                if len(chunk) != count:
                    raise IsoInspectionError("selected executable could not be read completely")
                digest.update(chunk)
                offset += count
        executable_sha256 = digest.hexdigest()
        elf_sha256 = executable_sha256
    manifest["executable"]["entry"] = executable_entry
    if executable_load_address is not None:
        manifest["executable"].update({
            "base": executable_load_address,
            "load_address": executable_load_address,
            "load_address_evidence": "documented-psp-default",
        })
    manifest = title_manifest.validate_manifest(manifest)

    profile = {
        "schema_version": EXPERIMENTAL_PROFILE_SCHEMA_VERSION,
        "manifest": manifest,
        "input_identity": {
            "disc_id": disc_id,
            "selected_executable": selected_path,
            "executable_sha256": executable_sha256,
            "elf_sha256": elf_sha256,
        },
    }
    profile_dir = user_root / "experimental" / disc_id
    profile_dir.mkdir(parents=True, exist_ok=True)
    resolved_dir = profile_dir.resolve(strict=True)
    if user_root != resolved_dir and user_root not in resolved_dir.parents:
        raise IsoInspectionError("experimental profile path escaped the user data directory")
    profile_path = resolved_dir / "profile.json"
    temporary_path = resolved_dir / "profile.json.tmp"
    temporary_path.write_text(
        json.dumps(profile, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary_path, profile_path)
    return profile_path


def _extent_from_record(record: bytes, file_size: int) -> tuple[int, int, bool]:
    if len(record) < 34 or record[0] < 34 or record[0] > len(record):
        raise IsoInspectionError("malformed ISO directory record")
    name_size = record[32]
    if 33 + name_size > record[0]:
        raise IsoInspectionError("truncated ISO directory identifier")
    lba_le = int.from_bytes(record[2:6], "little")
    lba_be = int.from_bytes(record[6:10], "big")
    size_le = int.from_bytes(record[10:14], "little")
    size_be = int.from_bytes(record[14:18], "big")
    if lba_le != lba_be or size_le != size_be:
        raise IsoInspectionError("ISO directory extent has conflicting endian copies")
    offset = lba_le * SECTOR_SIZE
    if offset > file_size or size_le > file_size - offset:
        raise IsoInspectionError("ISO directory extent exceeds the image bounds")
    return lba_le, size_le, bool(record[25] & 0x02)


def _read_iso_extent(
    stream, file_size: int, lba: int, size: int, offset: int, count: int
) -> bytes:
    base = lba * SECTOR_SIZE
    if base > file_size or size > file_size - base or offset > size or count > size - offset:
        raise IsoInspectionError("ISO file span exceeds the image bounds")
    stream.seek(base + offset)
    data = stream.read(count)
    if len(data) != count:
        raise IsoInspectionError("ISO file span is truncated")
    return data


def _lookup_iso_file(stream, file_size: int, path: tuple[str, ...]) -> tuple[int, int] | None:
    stream.seek(PVD_SECTOR * SECTOR_SIZE)
    pvd = stream.read(SECTOR_SIZE)
    if len(pvd) != SECTOR_SIZE or pvd[:7] != ISO_MAGIC:
        raise IsoInspectionError("missing primary volume descriptor")
    lba, size, is_dir = _extent_from_record(pvd[156:190], file_size)
    if not is_dir or size == 0 or size > MAX_DIRECTORY_BYTES:
        raise IsoInspectionError("root directory is not a bounded directory extent")

    for index, component in enumerate(path):
        if not is_dir or size == 0 or size > MAX_DIRECTORY_BYTES:
            raise IsoInspectionError("ISO path crosses a non-directory or oversized extent")
        directory = _read_iso_extent(stream, file_size, lba, size, 0, size)
        wanted = component.upper().encode("ascii")
        found: tuple[int, int, bool] | None = None
        offset = 0
        while offset < len(directory):
            record_size = directory[offset]
            if record_size == 0:
                offset = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
                continue
            if record_size < 34 or record_size > len(directory) - offset or \
                    (offset % SECTOR_SIZE) + record_size > SECTOR_SIZE:
                raise IsoInspectionError("malformed ISO directory record bounds")
            record = directory[offset : offset + record_size]
            name_size = record[32]
            if name_size == 0 or 33 + name_size > record_size:
                raise IsoInspectionError("malformed ISO directory identifier")
            name = record[33 : 33 + name_size]
            if name not in (b"\0", b"\1") and name.split(b";", 1)[0].upper() == wanted:
                found = _extent_from_record(record, file_size)
                break
            offset += record_size
        if found is None:
            return None
        lba, size, is_dir = found
        if index < len(path) - 1 and not is_dir:
            return None
    return (lba, size) if not is_dir else None


def _elf32_mips_usable(stream, file_size: int, lba: int, size: int) -> bool:
    if size < 52:
        return False
    header = _read_iso_extent(stream, file_size, lba, size, 0, 52)
    if header[:4] != b"\x7fELF" or header[4:7] != b"\x01\x01\x01":
        return False
    e_type, machine, version = struct.unpack_from("<HHI", header, 16)
    entry, phoff, shoff = struct.unpack_from("<III", header, 24)
    ehsize, phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHHH", header, 40)
    # Mirrors nk_iso.c: the analyzer accepts ET_REL/EXEC/DYN and PSP PRX (0xFFA0).
    if e_type not in (1, 2, 3, 0xFFA0) or machine != 8 or version != 1 or ehsize != 52:
        return False
    if phentsize != 32 or not 1 <= phnum <= 128 or phoff < ehsize:
        return False
    ph_end = phoff + phentsize * phnum
    if ph_end < phoff or ph_end > size:
        return False
    if shnum:
        sh_end = shoff + shentsize * shnum
        if shentsize != 40 or shoff < ehsize or sh_end < shoff or sh_end > size:
            return False
    elif shoff:
        return False

    table = _read_iso_extent(stream, file_size, lba, size, phoff, phentsize * phnum)
    have_load = False
    entry_executable = False
    for index in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
            "<8I", table, index * phentsize
        )
        if p_offset + p_filesz > size:
            return False
        if p_type != 1:
            continue
        memory_end = p_vaddr + p_memsz
        if p_memsz < p_filesz or memory_end > 0x100000000:
            return False
        if p_align > 1 and (p_align & (p_align - 1) or p_offset % p_align != p_vaddr % p_align):
            return False
        have_load = True
        if p_flags & 1 and p_vaddr <= entry < memory_end:
            entry_executable = True
    return have_load and entry_executable


def _classify_iso_executable(stream, file_size: int, path: str) -> dict[str, object]:
    entry = _lookup_iso_file(stream, file_size, ("PSP_GAME", "SYSDIR", path))
    if entry is None:
        return {"classification": "UNKNOWN", "present": False, "size_bytes": 0}
    lba, size = entry
    result: dict[str, object] = {
        "classification": "UNKNOWN", "present": True, "size_bytes": size,
    }
    if size == 0:
        result["classification"] = "EMPTY_OR_ZERO_FILLED"
        return result
    if size > MAX_EXECUTABLE_BYTES:
        return result
    header = _read_iso_extent(stream, file_size, lba, size, 0, min(size, 0x80))
    if header.startswith(b"\x7fELF"):
        if _elf32_mips_usable(stream, file_size, lba, size):
            result["classification"] = "PLAIN_MIPS_ELF32"
        return result
    if header.startswith(b"~SCE"):
        result["classification"] = "SCE_WRAPPER"
        return result
    if header.startswith(b"\0PBP"):
        result["classification"] = "PBP"
        return result
    if header.startswith(b"~PSP"):
        if len(header) < 0x64 or not 1 <= header[0x27] <= 4:
            return result
        sizes = struct.unpack_from("<4I", header, 0x54)[: header[0x27]]
        if any(value == 0 or value > MAX_EXECUTABLE_BYTES for value in sizes) or sum(sizes) > MAX_EXECUTABLE_BYTES:
            return result
        result["classification"] = "PSP_ENCRYPTED_CONTAINER"
        return result
    position = 0
    while position < size:
        count = min(8192, size - position)
        chunk = _read_iso_extent(stream, file_size, lba, size, position, count)
        if any(chunk):
            return result
        position += count
    result["classification"] = "EMPTY_OR_ZERO_FILLED"
    return result


def inspect_compatibility_preflight(
    iso_path: Path | str,
    *,
    metadata: IsoMetadata,
    runtime_root: Path | str,
) -> dict[str, object]:
    """Return the player/CLI compatibility checks for a selected ISO."""
    path = Path(iso_path)
    size_bytes = path.stat().st_size
    directory_error: str | None = None
    sfo_entry: tuple[int, int] | None = None
    try:
        with path.open("rb") as stream:
            sfo_entry = _lookup_iso_file(stream, size_bytes, ("PSP_GAME", "PARAM.SFO"))
            sfo_parsed = False
            if sfo_entry is not None and 20 <= sfo_entry[1] <= MAX_SFO_BYTES:
                raw_sfo = _read_iso_extent(stream, size_bytes, *sfo_entry, 0, sfo_entry[1])
                try:
                    values = parse_param_sfo(raw_sfo)
                    _canonical_disc_id(values.get("DISC_ID", ""))
                    sfo_parsed = True
                except IsoInspectionError:
                    sfo_parsed = False
            executables = {
                "EBOOT.BIN": _classify_iso_executable(stream, size_bytes, "EBOOT.BIN"),
                "BOOT.BIN": _classify_iso_executable(stream, size_bytes, "BOOT.BIN"),
            }
    except (OSError, IsoInspectionError, struct.error) as exc:
        sfo_parsed = False
        directory_error = str(exc)
        executables = {
            name: {"classification": "UNKNOWN", "present": False, "size_bytes": 0}
            for name in ("EBOOT.BIN", "BOOT.BIN")
        }

    eboot_kind = executables["EBOOT.BIN"]["classification"]
    boot_kind = executables["BOOT.BIN"]["classification"]
    selected = "EBOOT.BIN" if eboot_kind == "PLAIN_MIPS_ELF32" else None
    fallback = False
    if eboot_kind == "PSP_ENCRYPTED_CONTAINER" and boot_kind == "PLAIN_MIPS_ELF32":
        selected = "BOOT.BIN"
        fallback = True
    if directory_error:
        disc_check = {
            "code": "DISC_SFO", "status": "UNSUPPORTED",
            "message": "Disc directories could not be read safely; PARAM.SFO and executables were not trusted.",
            "issues": [],
        }
    elif sfo_parsed:
        disc_check = {
            "code": "DISC_SFO", "status": "OK",
            "message": "Disc image is readable and PARAM.SFO was parsed.", "issues": [],
        }
    elif sfo_entry is not None:
        disc_check = {
            "code": "DISC_SFO", "status": "UNSUPPORTED",
            "message": "PARAM.SFO is present but malformed; title identity was not trusted.",
            "issues": [],
        }
    else:
        disc_check = {
            "code": "DISC_SFO", "status": "MISSING",
            "message": "Disc image is readable, but PSP_GAME/PARAM.SFO is missing or could not be parsed.",
            "issues": [],
        }

    if selected == "BOOT.BIN":
        executable_check = {
            "code": "EXECUTABLE", "status": "OK",
            "message": "BOOT.BIN selected for analysis because EBOOT.BIN is encrypted.",
            "issues": [],
        }
    elif selected == "EBOOT.BIN":
        executable_check = {
            "code": "EXECUTABLE", "status": "OK",
            "message": "EBOOT.BIN is a plain MIPS ELF32 and selected for analysis.",
            "issues": [],
        }
    elif eboot_kind == "PSP_ENCRYPTED_CONTAINER":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "Encrypted executable. Decryption support is in the works (#295).",
            "issues": [295],
        }
    elif eboot_kind == "EMPTY_OR_ZERO_FILLED":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "EBOOT.BIN is empty or zero-filled and cannot be analyzed.", "issues": [],
        }
    elif eboot_kind == "SCE_WRAPPER":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "~SCE wrapper is not analyzable; container support is in the works (#295).",
            "issues": [295],
        }
    elif eboot_kind == "PBP":
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "PBP is not plain ELF; executable unpacking is in the works (#295).",
            "issues": [295],
        }
    else:
        executable_check = {
            "code": "EXECUTABLE", "status": "UNSUPPORTED",
            "message": "Unknown/malformed executable boundary; broader title support is in the works (#308).",
            "issues": [308],
        }

    from .launcher import RuntimeLauncher

    root = Path(runtime_root)
    package_present = RuntimeLauncher(repo_root=root).runtime_package_available(
        metadata.matched_profile
    )
    is_experimental = (
        metadata.matched_profile is None and disc_check["status"] == "OK" and
        title_manifest.DISC_ID_RE.fullmatch(metadata.disc_id.upper()) is not None
    )
    experimental_check = {
        "code": "EXPERIMENTAL",
        "status": "IN_PROGRESS",
        "message": "Experimental: this game has not been verified. Compatibility is unknown. Second-title verification is in the works (#285); generic title intake is in the works (#308).",
        "issues": [285, 308],
    } if is_experimental else None

    if is_experimental:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "MISSING",
            "message": "Experimental title runtime package is missing; generation is in the works (#296/#297).",
            "issues": [296, 297],
        }
    elif metadata.matched_profile is None:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "UNSUPPORTED",
            "message": "Title profile missing; generic title support is in the works (#308).",
            "issues": [308],
        }
    elif package_present:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "OK",
            "message": "Generated runtime executable and image are present.", "issues": [],
        }
    else:
        runtime_check = {
            "code": "RUNTIME_PACKAGE", "status": "MISSING",
            "message": "Runtime package missing; generation is in the works (#296/#297).",
            "issues": [296, 297],
        }

    if (root / "font" / "jpn0.pgf").is_file():
        fonts_check = {
            "code": "SYSTEM_FONTS", "status": "OK",
            "message": "User-supplied PSP system font jpn0.pgf is available.", "issues": [],
        }
    else:
        fonts_check = {
            "code": "SYSTEM_FONTS", "status": "MISSING",
            "message": "PSP font jpn0.pgf missing; provisioning is in the works (#300).",
            "issues": [300],
        }
    audio_check = {
        "code": "AUDIO_OUTPUT", "status": "OK",
        "message": "Sound plays through your default audio device. "
                   "With no device, the game runs silently.",
        "issues": [],
    }
    checks = [disc_check, executable_check, runtime_check, fonts_check, audio_check]
    if experimental_check is not None:
        checks.insert(0, experimental_check)
    return {
        "is_experimental": is_experimental,
        "selected_executable": selected,
        "boot_fallback": fallback,
        "executables": executables,
        "checks": checks,
    }
