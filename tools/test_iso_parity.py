# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Differential parity tests comparing native C core (nk_iso, nk_library, nk_launch) against Python."""

from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import hle_manifest
from import_fixtures import BASE_VADDR, SYSLIB_EXPORT, build_module_elf
from nk_core import iso_inspect
from nk_core.iso_inspect import (
    _classify_decrypted_elf_file,
    decrypt_needed_modules,
    decrypted_module_dir,
    IsoInspectionError,
    inspect_compatibility_preflight,
    inspect_iso,
    list_disc_module_candidates,
    plan_provisional_module_bindings,
    read_guest_module_interface,
    runtime_registered_nids,
    runtime_serves_module,
    RuntimeRegistryUnavailableError,
    write_experimental_profile,
)
import nk_cli


def build_param_sfo(disc_id: str, title: str, version: str = "1.00") -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with given keys."""
    entries = [
        ("DISC_ID", 0x0204, disc_id.encode("utf-8") + b"\0"),
        ("DISC_VERSION", 0x0204, version.encode("utf-8") + b"\0"),
        ("TITLE", 0x0204, title.encode("utf-8") + b"\0"),
    ]
    entries.sort(key=lambda e: e[0])

    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def build_custom_param_sfo(entries: list[tuple[str, int, bytes]]) -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with arbitrary raw entries (e.g. duplicates)."""
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def create_test_iso(
    path: Path,
    disc_id: str = "TEST00001",
    title: str = "Test Game",
    version: str = "1.00",
    volume_id: str = "TEST_VOL",
    sfo_bytes: bytes | None = None,
    update_sfo_bytes: bytes | None = None,
) -> None:
    sector_size = 2048
    sfo_lba = 32
    update_sfo_lba = 30
    root_lba, game_lba, sysdir_lba, update_lba = 33, 34, 35, 36
    num_sectors = max(1024, sfo_lba + 2, update_lba + 2)
    data = bytearray(num_sectors * sector_size)

    pvd_off = 16 * sector_size
    data[pvd_off] = 0x01
    data[pvd_off + 1 : pvd_off + 6] = b"CD001"
    data[pvd_off + 6] = 0x01
    data[pvd_off + 40 : pvd_off + 40 + len(volume_id)] = volume_id.encode("latin-1")

    if sfo_bytes is None:
        sfo_bytes = build_param_sfo(disc_id, title, version)
    sfo_off = sfo_lba * sector_size
    data[sfo_off : sfo_off + len(sfo_bytes)] = sfo_bytes
    if update_sfo_bytes is not None:
        update_off = update_sfo_lba * sector_size
        data[update_off : update_off + len(update_sfo_bytes)] = update_sfo_bytes

    root_entries = (
        _dir_record(bytes([0]), root_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PSP_GAME", game_lba, sector_size, True)
    )
    data[root_lba * sector_size : root_lba * sector_size + len(root_entries)] = root_entries

    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", sfo_lba, len(sfo_bytes), False)
    )
    if update_sfo_bytes is not None:
        game_entries += _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
    data[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = game_entries

    if update_sfo_bytes is not None:
        sysdir_entries = (
            _dir_record(bytes([0]), sysdir_lba, sector_size, True)
            + _dir_record(bytes([1]), game_lba, sector_size, True)
            + _dir_record(b"UPDATE", update_lba, sector_size, True)
        )
        update_entries = (
            _dir_record(bytes([0]), update_lba, sector_size, True)
            + _dir_record(bytes([1]), sysdir_lba, sector_size, True)
            + _dir_record(b"PARAM.SFO;1", update_sfo_lba, len(update_sfo_bytes), False)
        )
        data[sysdir_lba * sector_size : sysdir_lba * sector_size + len(sysdir_entries)] = sysdir_entries
        data[update_lba * sector_size : update_lba * sector_size + len(update_entries)] = update_entries

    pvd_root = _dir_record(bytes([0]), root_lba, sector_size, True)[:34]
    data[pvd_off + 156 : pvd_off + 156 + 34] = pvd_root
    path.write_bytes(data)


def create_test_iso_with_executables(
    path: Path, eboot: bytes, boot: bytes | None = None,
    disc_id: str = "TEST00001", title: str = "Test Game",
) -> None:
    """Add synthetic SYSDIR executables to the source-owned ISO fixture."""
    create_test_iso(path, disc_id=disc_id, title=title)
    sector_size = 2048
    data = bytearray(path.read_bytes())
    game_lba, sysdir_lba, eboot_lba, boot_lba = 34, 35, 36, 37
    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), 33, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, len(build_param_sfo(disc_id, title)), False)
        + _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
    )
    sysdir_entries = (
        _dir_record(bytes([0]), sysdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
        + _dir_record(b"EBOOT.BIN;1", eboot_lba, len(eboot), False)
    )
    if boot is not None:
        sysdir_entries += _dir_record(b"BOOT.BIN;1", boot_lba, len(boot), False)
    data[game_lba * sector_size : (game_lba + 1) * sector_size] = bytes(sector_size)
    data[sysdir_lba * sector_size : (sysdir_lba + 1) * sector_size] = bytes(sector_size)
    data[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = game_entries
    data[sysdir_lba * sector_size : sysdir_lba * sector_size + len(sysdir_entries)] = sysdir_entries
    data[eboot_lba * sector_size : eboot_lba * sector_size + len(eboot)] = eboot
    if boot is not None:
        data[boot_lba * sector_size : boot_lba * sector_size + len(boot)] = boot
    path.write_bytes(data)


def create_test_iso_with_modules(
    path: Path,
    eboot: bytes,
    *,
    sysdir_modules: dict[str, bytes],
    usrdir_modules: dict[str, bytes],
    old_eboot: bytes | None = None,
    disc_id: str = "TEST00001",
    title: str = "Test Game",
) -> None:
    """Build a source-owned ISO with module candidates in both PSP game dirs."""
    create_test_iso(path, disc_id=disc_id, title=title)
    sector_size = 2048
    data = bytearray(path.read_bytes())
    root_lba, game_lba, sysdir_lba, usrdir_lba = 33, 34, 35, 36
    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, len(build_param_sfo(disc_id, title)), False)
        + _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
        + _dir_record(b"USRDIR", usrdir_lba, sector_size, True)
    )
    sysdir_entries = (
        _dir_record(bytes([0]), sysdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
    )
    usrdir_entries = (
        _dir_record(bytes([0]), usrdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
    )
    next_lba = 37
    file_records = []
    files = [
        {"entries": sysdir_entries, "members": sysdir_modules},
        {"entries": usrdir_entries, "members": usrdir_modules},
    ]
    for item in files:
        directory_entries = item["entries"]
        members = item["members"]
        for name, contents in sorted(members.items()):
            encoded_name = name.encode("ascii") + b";1"
            directory_entries += _dir_record(encoded_name, next_lba, len(contents), False)
            file_records.append((next_lba, contents))
            next_lba += max(1, (len(contents) + sector_size - 1) // sector_size)
        item["entries"] = directory_entries
    sysdir_entries, usrdir_entries = files[0]["entries"], files[1]["entries"]
    sysdir_entries += _dir_record(b"EBOOT.BIN;1", next_lba, len(eboot), False)
    file_records.append((next_lba, eboot))
    next_lba += max(1, (len(eboot) + sector_size - 1) // sector_size)
    if old_eboot is not None:
        sysdir_entries += _dir_record(b"EBOOT.OLD;1", next_lba, len(old_eboot), False)
        file_records.append((next_lba, old_eboot))
    for entries, lba in ((game_entries, game_lba), (sysdir_entries, sysdir_lba),
                         (usrdir_entries, usrdir_lba)):
        start = lba * sector_size
        data[start : start + len(entries)] = entries
    for lba, contents in file_records:
        start = lba * sector_size
        data[start : start + len(contents)] = contents
    path.write_bytes(data)


def create_test_iso_with_module_tree(
    path: Path, modules: dict[str, bytes], *,
    disc_id: str = "TEST00001", title: str = "Synthetic Module Tree",
) -> dict[tuple[str, ...], int]:
    """Build a source-owned ISO carrying files at arbitrary SYSDIR/USRDIR paths."""
    sector_size = 2048
    sfo = build_param_sfo(disc_id, title)
    directories: set[tuple[str, ...]] = {
        (), ("PSP_GAME",), ("PSP_GAME", "SYSDIR"), ("PSP_GAME", "USRDIR"),
    }
    files: dict[tuple[str, ...], bytes] = {
        ("PSP_GAME", "PARAM.SFO"): sfo,
    }
    for member, contents in modules.items():
        parts = tuple(member.split("/"))
        if len(parts) == 2 and parts[0] == "PSP_GAME" and \
                parts[1].casefold() in {"sysdir", "usrdir"}:
            directories.discard(parts)
            files[parts] = contents
            continue
        if len(parts) < 3 or parts[:2] not in {
            ("PSP_GAME", "SYSDIR"), ("PSP_GAME", "USRDIR"),
        }:
            raise ValueError(f"module fixture path is outside SYSDIR/USRDIR: {member}")
        for end in range(1, len(parts)):
            directories.add(parts[:end])
        if parts in files or parts in directories:
            raise ValueError(f"duplicate module fixture path: {member}")
        files[parts] = contents

    def pack_records(records: list[bytes]) -> bytes:
        output = bytearray()
        for record in records:
            sector_offset = len(output) % sector_size
            if sector_offset + len(record) > sector_size:
                output.extend(bytes(sector_size - sector_offset))
            output.extend(record)
        output.extend(bytes((-len(output)) % sector_size))
        if not output:
            output.extend(bytes(sector_size))
        return bytes(output)

    directory_children: dict[tuple[str, ...], list[tuple[str, tuple[str, ...], bool]]] = {
        directory: [] for directory in directories
    }
    for directory in directories:
        if directory:
            directory_children[directory[:-1]].append((directory[-1], directory, True))
    for file_path in files:
        directory_children[file_path[:-1]].append((file_path[-1], file_path, False))

    directory_sizes: dict[tuple[str, ...], int] = {}
    for directory, children in directory_children.items():
        placeholder = [
            _dir_record(bytes([0]), 1, sector_size, True),
            _dir_record(bytes([1]), 1, sector_size, True),
        ]
        for name, _child_path, is_directory in sorted(
            children, key=lambda child: child[0].casefold()
        ):
            encoded = name.encode("ascii") if is_directory else name.encode("ascii") + b";1"
            placeholder.append(_dir_record(encoded, 1, sector_size, is_directory))
        directory_sizes[directory] = len(pack_records(placeholder))

    directory_lbas = {(): 33, ("PSP_GAME",): 34}
    next_lba = 35
    for directory in sorted(
        (item for item in directories if item not in directory_lbas),
        key=lambda item: (len(item), item),
    ):
        directory_lbas[directory] = next_lba
        next_lba += directory_sizes[directory] // sector_size

    file_lbas = {("PSP_GAME", "PARAM.SFO"): 32}
    file_sizes = {("PSP_GAME", "PARAM.SFO"): len(sfo)}
    for file_path, contents in sorted(files.items()):
        if file_path == ("PSP_GAME", "PARAM.SFO"):
            continue
        file_lbas[file_path] = next_lba
        file_sizes[file_path] = len(contents)
        next_lba += max(1, (len(contents) + sector_size - 1) // sector_size)

    directory_payloads: dict[tuple[str, ...], bytes] = {}
    for directory, children in directory_children.items():
        parent = directory[:-1] if directory else ()
        records = [
            _dir_record(bytes([0]), directory_lbas[directory], directory_sizes[directory], True),
            _dir_record(bytes([1]), directory_lbas[parent], directory_sizes[parent], True),
        ]
        for name, child_path, is_directory in sorted(
            children, key=lambda child: child[0].casefold()
        ):
            encoded = name.encode("ascii") if is_directory else name.encode("ascii") + b";1"
            records.append(_dir_record(
                encoded,
                directory_lbas[child_path] if is_directory else file_lbas[child_path],
                directory_sizes[child_path] if is_directory else file_sizes[child_path],
                is_directory,
            ))
        directory_payloads[directory] = pack_records(records)

    data = bytearray(max(1024, next_lba) * sector_size)
    pvd_offset = 16 * sector_size
    data[pvd_offset] = 0x01
    data[pvd_offset + 1:pvd_offset + 6] = b"CD001"
    data[pvd_offset + 6] = 0x01
    volume_id = b"MODULE_TREE"
    data[pvd_offset + 40:pvd_offset + 40 + len(volume_id)] = volume_id
    data[pvd_offset + 156:pvd_offset + 190] = _dir_record(
        bytes([0]), 33, directory_sizes[()], True
    )[:34]
    for directory, payload in directory_payloads.items():
        start = directory_lbas[directory] * sector_size
        data[start:start + len(payload)] = payload
    for file_path, contents in files.items():
        start = file_lbas[file_path] * sector_size
        data[start:start + len(contents)] = contents
    path.write_bytes(data)
    return directory_lbas



def archive_title_manifest(disc_id: str, title_id: str) -> dict:
    """A user title manifest for a synthetic disc whose data ships in XB archives.

    The whole USRDIR is the title's loose-content root and ``xbdata`` its data
    root: the archive layout the native staging transaction extracts.
    """
    return {
        "schema_version": 1,
        "id": title_id,
        "display_name": "Synthetic archive title",
        "kind": "retail",
        "disc": {"id": disc_id, "region": "NA", "revision_policy": "exact-disc-id"},
        "executable": {
            "base": 0,
            "entry": 0,
            "bss_metadata_source": "none",
            "extra_executable_spans": [],
        },
        "modules": [],
        "filesystem": {
            "data_root": "xbdata",
            "memory_stick_root": "memstick",
            "device_prefixes": ["host0:", "ms0:"],
            "loose_content_roots": [
                {"root": ".", "mount": "", "precedence": 0, "skip_primary_root": True},
            ],
        },
        "hle_profile": "generic",
        "codegen_profile": "none",
        "feature_requirements": [],
        "verification_profile": "experimental-unverified",
    }


def create_archive_title_iso(
    path: Path, *, disc_id: str, title: str, executable: bytes
) -> None:
    """Build a source-owned disc whose game data is an XB archive below USRDIR/xbdata."""
    from test_xb_probe import _make_archive
    from xb_probe import XBCompression

    archive = _make_archive([
        ("data/raw.bin", b"synthetic archive member", XBCompression.NONE),
        ("data/sound/theme.sgd", b"synthetic sound member", XBCompression.LZS),
    ])
    create_test_iso_with_module_tree(path, {
        "PSP_GAME/SYSDIR/EBOOT.BIN": executable,
        "PSP_GAME/USRDIR/xbdata/menu/assets.xb": archive,
        "PSP_GAME/USRDIR/readme.txt": b"synthetic loose file\n",
    }, disc_id=disc_id, title=title)

def build_plain_mips_elf(
    e_type: int = 2, *, vaddr: int = 0x08800000, memsz: int = 4,
    entry: int | None = None, p_type: int = 1, p_flags: int = 5, filesz: int = 4,
) -> bytes:
    """A one-segment ELF32/MIPS image; ``entry`` defaults to ``vaddr``."""
    elf = bytearray(88)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, e_type, 8, 1)
    struct.pack_into("<III", elf, 24, vaddr if entry is None else entry, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 1, 0, 0)
    struct.pack_into("<8I", elf, 52, p_type, 84, vaddr, vaddr, filesz, memsz, p_flags, 4)
    elf[84:88] = b"\x34\x12\x00\x00"
    return bytes(elf)


def build_psp_container() -> bytes:
    container = bytearray(0x80)
    container[:4] = b"~PSP"
    container[0x27] = 1
    struct.pack_into("<I", container, 0x54, 0x1000)
    return bytes(container)


def build_overlapping_mips_elf(
    *, vaddr1: int = 0, memsz1: int = 0x2000, vaddr2: int = 0x1000, memsz2: int = 0x2000
) -> bytes:
    elf = bytearray(124)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, 0xFFA0, 8, 1)
    struct.pack_into("<III", elf, 24, vaddr1, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 2, 0, 0)
    struct.pack_into("<8I", elf, 52, 1, 116, vaddr1, vaddr1, 4, memsz1, 5, 4)
    struct.pack_into("<8I", elf, 84, 1, 120, vaddr2, vaddr2, 4, memsz2, 5, 4)
    elf[116:124] = b"\x00\x00\x00\x00\x00\x00\x00\x00"
    return bytes(elf)


def _both_endian32(value: int) -> bytes:
    """ECMA-119 7.3.3: a 32-bit value recorded little-endian then big-endian."""
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _dir_record(name: bytes, extent_lba: int, size_bytes: int, is_dir: bool) -> bytes:
    """Build one ECMA-119 9.1 directory record, padded to an even length."""
    rec = bytearray(33 + len(name))
    rec[1] = 0                                          # extended attribute length
    rec[2:10] = _both_endian32(extent_lba)              # 9.1.3 extent
    rec[10:18] = _both_endian32(size_bytes)             # 9.1.4 data length
    rec[25] = 0x02 if is_dir else 0x00                  # 9.1.6 file flags
    rec[28:32] = (1).to_bytes(2, "little") + (1).to_bytes(2, "big")
    rec[32] = len(name)                                 # 9.1.10 identifier length
    rec[33:33 + len(name)] = name
    if len(rec) % 2:                                    # 9.1.12 padding
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def create_custom_sfo_iso(path: Path, sfo_bytes: bytes, volume_id: str = "CUSTOM_VOL") -> None:
    create_test_iso(path, volume_id=volume_id, sfo_bytes=sfo_bytes)


def build_pbp_package(
    path: Path,
    *,
    disc_id: str = "TEST00001",
    title: str = "PBP Test Package",
    sfo_bytes: bytes | None = None,
    offsets: list[int] | None = None,
    header: bytes | None = None,
    padding: bytes = b"",
) -> None:
    """Write a synthetic PlayStation Store package (PBP).

    The default layout is a 40-byte header, PARAM.SFO in the first section and
    every other section empty. ``offsets`` or a whole ``header`` override that
    layout so a caller can build a hostile package.
    """
    if sfo_bytes is None:
        sfo_bytes = build_param_sfo(disc_id, title)
    if header is None:
        built = bytearray(40)
        built[:4] = b"\x00PBP"
        struct.pack_into("<I", built, 4, 0x00010000)
        if offsets is None:
            sfo_end = 40 + len(sfo_bytes)
            offsets = [40] + [sfo_end] * 7
        for index, value in enumerate(offsets):
            struct.pack_into("<I", built, 8 + index * 4, value)
        header = bytes(built)
    path.write_bytes(header + sfo_bytes + padding)


class IsoParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.gcc = shutil.which("gcc")
        if not cls.gcc:
            raise unittest.SkipTest("gcc not available")

        # Compile the native test runner once.  The tests assert parser and
        # launch behaviour, not compiler freshness; each test still receives
        # its own input/output directory below.
        cls._build_dir = Path(tempfile.mkdtemp(prefix="nk_parity_build_"))
        cls.harness_c = cls._build_dir / "parity_harness.c"
        executable_name = "parity_harness.exe" if sys.platform == "win32" else "parity_harness"
        cls.exe_path = cls._build_dir / executable_name
        cls.path_limit_exe = cls._build_dir / (
            "parity_path_limit.exe" if sys.platform == "win32" else "parity_path_limit"
        )

        has_launch = (ROOT / "src" / "core" / "nk_launch.c").is_file()
        core_srcs = [
            ROOT / "src" / "core" / "nk_iso.c",
            ROOT / "src" / "core" / "nk_library.c",
            ROOT / "src" / "core" / "nk_title_manifest.c",
            ROOT / "src" / "core" / "nk_json.c",
            ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
        ]
        if has_launch:
            core_srcs.append(ROOT / "src" / "core" / "nk_launch.c")
        if sys.platform == "win32":
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_posix.c")

        harness_code = f"""#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_title_manifest.h"
#if {1 if has_launch else 0}
#include "nk_launch.h"
#endif

static bool print_module_path(const char *path, const NkIsoDirEntry *entry,
                              void *userdata) {{
    (void)userdata;
    size_t length = strlen(entry->name);
    if (length >= 4 &&
        tolower((unsigned char)entry->name[length - 4]) == '.' &&
        tolower((unsigned char)entry->name[length - 3]) == 'p' &&
        tolower((unsigned char)entry->name[length - 2]) == 'r' &&
        tolower((unsigned char)entry->name[length - 1]) == 'x') {{
        printf("MODULE_PATH:%s\\n", path);
    }} else if (length >= 4 &&
        tolower((unsigned char)entry->name[length - 4]) == '.' &&
        tolower((unsigned char)entry->name[length - 3]) == 'e' &&
        tolower((unsigned char)entry->name[length - 2]) == 'l' &&
        tolower((unsigned char)entry->name[length - 1]) == 'f') {{
        printf("MODULE_PATH:%s\\n", path);
    }}
    return true;
}}

int main(int argc, char **argv) {{
    if (argc < 2) return 1;
    const char *mode = argv[1];

    if (strcmp(mode, "module_walk") == 0) {{
        if (argc < 3) return 1;
        NkIsoReader *reader = nk_iso_reader_open(argv[2]);
        if (!reader) return 2;
        NkIsoModuleWalkStatus status = nk_iso_reader_walk_module_tree(
            reader, print_module_path, NULL);
        printf("MODULE_WALK_STATUS:%d\\n", (int)status);
        nk_iso_reader_close(reader);
        return 0;
    }}

    if (strcmp(mode, "inspect") == 0) {{
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoMetadata meta;
        NkResult res = nk_iso_inspect(iso_path, &meta);
        if (res != NK_OK) {{
            printf("RESULT:ERROR:%d:%s\\n", (int)res, meta.error_message);
            printf("BOUNDARY:%s\\n", meta.boundary_code);
            printf("MESSAGE:%s\\n", meta.error_message);
            return 0;
        }}
        printf("RESULT:OK\\n");
        printf("DISC_ID:%s\\n", meta.disc_id);
        printf("TITLE:%s\\n", meta.title_name);
        printf("VERSION:%s\\n", meta.disc_version);
        printf("VOLUME_ID:%s\\n", meta.volume_id);
        printf("SUPPORTED:%d\\n", meta.is_supported ? 1 : 0);
        printf("PARAM_SFO_PARSED:%d\\n", meta.param_sfo_parsed ? 1 : 0);
        printf("EBOOT_KIND:%d\\n", (int)meta.executables.eboot.kind);
        printf("BOOT_KIND:%d\\n", (int)meta.executables.boot.kind);
        printf("SELECTED_EXECUTABLE:%d\\n", (int)meta.executables.selected);
        printf("BOOT_FALLBACK:%d\\n", meta.executables.boot_fallback ? 1 : 0);
        printf("MATCHED_ID:%s\\n", meta.matched_title_id[0] ? meta.matched_title_id : "NONE");
        printf("STATUS:%d\\n", (int)meta.status);
        return 0;
    }}

    if (strcmp(mode, "library_test") == 0) {{
        if (argc < 3) return 1;
        const char *lib_json = argv[2];
        NkLibrary lib;
        nk_library_init(&lib);

        NkGameEntry g1;
        memset(&g1, 0, sizeof(g1));
        snprintf(g1.disc_id, sizeof(g1.disc_id), "UCUS98701");
        snprintf(g1.title_name, sizeof(g1.title_name), "Hot Shots Tennis");
        snprintf(g1.title_id, sizeof(g1.title_id), "hst-ucus98701-v1");
        snprintf(g1.iso_path, sizeof(g1.iso_path), "C:/games/hst.iso");
        g1.status = NK_STATUS_VERIFIED;
        g1.is_experimental = true;
        g1.executable_eboot_kind = 2;
        g1.executable_boot_kind = 1;
        g1.executable_selection = 2;
        g1.executable_boot_fallback = true;
        snprintf(g1.selected_executable, sizeof(g1.selected_executable), "BOOT.BIN");
        g1.is_prepared = true;
        g1.iso_size_bytes = 1000000;

        assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
        assert(nk_library_count(&lib) == 1);
        assert(nk_library_save(&lib, lib_json) == NK_OK);

        NkLibrary loaded;
        assert(nk_library_load(&loaded, lib_json) == NK_OK);
        assert(nk_library_count(&loaded) == 1);

        const NkGameEntry *entry = nk_library_find_by_disc_id(&loaded, "UCUS98701");
        assert(entry != NULL);
        assert(strcmp(entry->title_name, "Hot Shots Tennis") == 0);
        assert(strcmp(entry->title_id, "hst-ucus98701-v1") == 0);
        assert(entry->status == NK_STATUS_VERIFIED);
        assert(entry->is_prepared == true);
        assert(entry->is_experimental == true);
        assert(entry->executable_eboot_kind == 2);
        assert(entry->executable_boot_kind == 1);
        assert(entry->executable_selection == 2);
        assert(entry->executable_boot_fallback == true);
        assert(strcmp(entry->selected_executable, "BOOT.BIN") == 0);

        /* Test remove */
        assert(nk_library_remove(&loaded, "UCUS98701") == NK_OK);
        assert(nk_library_count(&loaded) == 0);
        assert(nk_library_save(&loaded, lib_json) == NK_OK);

        printf("LIBRARY_TEST_PASSED\\n");
        return 0;
    }}

    if (strcmp(mode, "experimental_profile") == 0) {{
        if (argc < 4) return 1;
        const char *iso_path = argv[2];
        const char *user_data_root = argv[3];
        NkIsoMetadata meta;
        NkResult inspect_result = nk_iso_inspect(iso_path, &meta);
        if (inspect_result != NK_OK) {{
            printf("PROFILE_RESULT:ERROR:DISC:%s\\n", meta.error_message);
            return 0;
        }}
        char profile_id[64] = {{0}};
        char error[256] = {{0}};
        bool ok = nk_title_manifest_write_experimental_profile(
            iso_path, meta.param_sfo_parsed, meta.disc_id, meta.title_name,
            meta.executables.selected_path, user_data_root, profile_id,
            sizeof(profile_id), error, sizeof(error));
        if (!ok) {{
            printf("PROFILE_RESULT:ERROR:%s\\n", error);
            return 0;
        }}
        printf("PROFILE_RESULT:OK\\n");
        printf("PROFILE_ID:%s\\n", profile_id);
        printf("PACKAGE_AVAILABLE:%d\\n",
               nk_launch_runtime_package_available(user_data_root, profile_id) ? 1 : 0);
        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", meta.disc_id);
        snprintf(game.title_id, sizeof(game.title_id), "%s", profile_id);
        snprintf(game.iso_path, sizeof(game.iso_path), "%s", iso_path);
        NkLaunchSession session;
        NkResult launch_result = nk_launch_prepare_session(
            &session, &game, user_data_root);
        printf("LAUNCH_PREPARE:%s\\n",
               launch_result == NK_OK ? "OK" : "REFUSED");
        printf("LAUNCH_REASON:%s\\n", session.last_error);
        return 0;
    }}

#if {1 if has_launch else 0}
    if (strcmp(mode, "launch_test") == 0) {{
        if (argc < 4) return 1;
        const char *repo_root = argv[2];
        const char *iso_path = argv[3];

        const char *want_disc = argc > 4 ? argv[4] : "UCUS98701";
        const char *want_title = argc > 5 ? argv[5] : "hst-ucus98701-v1";

        NkGameEntry g;
        memset(&g, 0, sizeof(g));
        snprintf(g.disc_id, sizeof(g.disc_id), "%s", want_disc);
        snprintf(g.title_id, sizeof(g.title_id), "%s", want_title);
        snprintf(g.iso_path, sizeof(g.iso_path), "%s", iso_path);

        NkLaunchSession session;
        NkResult res = nk_launch_prepare_session(&session, &g, repo_root);
        if (res != NK_OK) {{
            printf("LAUNCH_PREPARE_ERROR:%d:%s\\n", (int)res, session.last_error);
            return 0;
        }}
        printf("LAUNCH_PREPARE_OK\\n");
        printf("EXE:%s\\n", session.executable_path);
        printf("ISO:%s\\n", session.iso_path);
        printf("BASE:0x%08x\\n", session.base_address);
        printf("ENTRY:0x%08x\\n", session.entry_point);
        return 0;
    }}

    if (strcmp(mode, "boot_roundtrip") == 0) {{
        if (argc < 5) return 1;
        NkLibrary lib;
        NkResult load_result = nk_library_load(&lib, argv[2]);
        printf("LOAD_RESULT:%d\\n", (int)load_result);
        if (load_result != NK_OK) return 0;
        const NkGameEntry *entry = nk_library_find_by_disc_id(&lib, "TEST00001");
        if (!entry) return 2;
        printf("BOOT_LENGTH:%u\\n", (unsigned)strlen(entry->boot_executable));
        NkResult save_result = nk_library_save(&lib, argv[3]);
        printf("SAVE_RESULT:%d\\n", (int)save_result);
        if (save_result != NK_OK) return 0;
        NkLibrary saved_lib;
        NkResult saved_load_result = nk_library_load(&saved_lib, argv[3]);
        const NkGameEntry *saved_entry = saved_load_result == NK_OK
            ? nk_library_find_by_disc_id(&saved_lib, "TEST00001") : NULL;
        printf("SAVE_BOOT_MATCH:%d\\n",
               saved_entry && strcmp(entry->boot_executable,
                                     saved_entry->boot_executable) == 0);
        NkLaunchSession session;
        NkResult session_result = nk_launch_prepare_session(&session, entry, argv[4]);
        printf("SESSION_RESULT:%d\\n", (int)session_result);
        printf("SESSION_BOOT_MATCH:%d\\n",
               strcmp(entry->boot_executable, session.boot_executable) == 0);
        return 0;
    }}

    if (strcmp(mode, "boot_session_unterminated") == 0) {{
        NkGameEntry game;
        NkLaunchSession session;
        memset(&game, 0, sizeof(game));
        memset(&session, 0, sizeof(session));
        snprintf(game.disc_id, sizeof(game.disc_id), "TEST00001");
        snprintf(game.title_id, sizeof(game.title_id), "synthetic-allegrex-v1");
        memset(game.boot_executable, 'X', sizeof(game.boot_executable));
        NkResult result = nk_launch_prepare_session(&session, &game, ".");
        printf("SESSION_RESULT:%d\\n", (int)result);
        printf("ERROR:%s\\n", session.last_error);
        return 0;
    }}
#endif

    if (strcmp(mode, "boot_save_unterminated") == 0) {{
        if (argc < 3) return 1;
        NkLibrary lib;
        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        nk_library_init(&lib);
        snprintf(game.disc_id, sizeof(game.disc_id), "TEST00001");
        memset(game.boot_executable, 'X', sizeof(game.boot_executable));
        assert(nk_library_add_or_update(&lib, &game) == NK_OK);
        NkResult result = nk_library_save(&lib, argv[2]);
        printf("SAVE_RESULT:%d\\n", (int)result);
        return 0;
    }}

    if (strcmp(mode, "reader_test") == 0) {{
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoReader *reader = nk_iso_reader_open(iso_path);
        if (!reader) {{
            printf("READER:OPEN_FAILED\\n");
            return 0;
        }}
        printf("READER:OPEN_OK\\n");
        printf("VOLUME_ID:%s\\n", nk_iso_reader_volume_id(reader));
        printf("FILE_SIZE:%llu\\n", (unsigned long long)nk_iso_reader_file_size(reader));

        uint32_t lba = 0, sz = 0;
        bool is_dir = false;
        int rc = nk_iso_reader_lookup(reader, "PSP_GAME/PARAM.SFO", &lba, &sz, &is_dir);
        printf("LOOKUP_SFO:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "PSP_GAME", &lba, &sz, &is_dir);
        printf("LOOKUP_DIR:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "NONEXISTENT", &lba, &sz, &is_dir);
        printf("LOOKUP_MISS:%d\\n", rc);

        NkIsoDirEntry de;
        rc = nk_iso_reader_list(reader, "", 0, &de);
        printf("LIST_ROOT_0:%d:%s:%u:%u:%d\\n", rc, de.name, (unsigned)de.lba, (unsigned)de.size, de.is_directory ? 1 : 0);

        rc = nk_iso_reader_list(reader, "", 1, &de);
        printf("LIST_ROOT_1:%d\\n", rc);

        uint8_t buf[256];
        int bytes = nk_iso_reader_read(reader, 32, 0, buf, sizeof(buf));
        printf("READ_SFO:%d\\n", bytes);

        bytes = nk_iso_reader_read(reader, 2000, 0, buf, sizeof(buf));
        printf("READ_EOF:%d\\n", bytes);

        nk_iso_reader_close(reader);
        return 0;
    }}

    return 2;
}}
"""
        cls.harness_c.write_text(harness_code, encoding="utf-8")

        cmd = [
            cls.gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(ROOT / "src" / "core"),
            "-I", str(ROOT / "src" / "core" / "generated"),
            str(cls.harness_c),
        ] + [str(s) for s in core_srcs] + [
            *(["-lshell32", "-lole32", "-luuid"] if sys.platform == "win32" else []),
            "-o", str(cls.exe_path),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode:
            raise AssertionError(f"Compilation of parity harness failed: {res.stderr}")
        path_limit_cmd = [cls.gcc, "-DNK_ISO_MODULE_TREE_MAX_PATH_BYTES=64", *cmd[1:-1],
                          str(cls.path_limit_exe)]
        res = subprocess.run(path_limit_cmd, capture_output=True, text=True)
        if res.returncode:
            raise AssertionError(f"Compilation of path-limit harness failed: {res.stderr}")

    def setUp(self) -> None:
        # Inputs and any application data remain isolated per test even though
        # the immutable native runner is shared by the class.
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_parity_test_"))
        self.exe_path = type(self).exe_path

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    @classmethod
    def tearDownClass(cls) -> None:
        build_dir = getattr(cls, "_build_dir", None)
        if build_dir is not None:
            shutil.rmtree(build_dir, ignore_errors=True)
        super().tearDownClass()

    def _run_native_inspect(self, iso_path: Path) -> dict[str, str]:
        cmd = [str(self.exe_path), "inspect", str(iso_path)]
        res = subprocess.run(
            cmd, capture_output=True, text=True, encoding="utf-8", errors="strict"
        )
        self.assertEqual(res.returncode, 0, f"Native inspect failed: {res.stderr}")
        lines = res.stdout.strip().splitlines()
        out: dict[str, str] = {}
        for l in lines:
            if ":" in l:
                k, v = l.split(":", 1)
                out[k] = v
        return out

    def _run_native_module_walk(
        self, iso_path: Path, executable: Path | None = None
    ) -> tuple[int, list[str]]:
        result = subprocess.run(
            [str(executable or self.exe_path), "module_walk", str(iso_path)],
            capture_output=True, text=True, encoding="utf-8", errors="strict",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        status = None
        paths = []
        for line in result.stdout.splitlines():
            if line.startswith("MODULE_WALK_STATUS:"):
                status = int(line.split(":", 1)[1])
            elif line.startswith("MODULE_PATH:"):
                paths.append(line.split(":", 1)[1])
        self.assertIsNotNone(status, result.stdout)
        return status, paths

    def test_module_discovery_covers_usrdir_module_and_nested_paths(self) -> None:
        iso_file = self.temp_dir / "module-tree.iso"
        paths = {
            "PSP_GAME/USRDIR/module/sdk.prx": build_plain_mips_elf(0xFFA0),
            "PSP_GAME/USRDIR/DATA/MODULE/MODULE/nested.prx": build_psp_container(),
            "PSP_GAME/USRDIR/Mixed/SDK_VARIANT.PrX": build_psp_container(),
        }
        create_test_iso_with_module_tree(iso_file, paths)
        candidates = list_disc_module_candidates(iso_file)
        members = {candidate["members"][0] for candidate in candidates}
        self.assertEqual(members, {tuple(path.split("/")) for path in paths})
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        package_members = {
            tuple(candidate["directory"]) + (candidate["entry"].name,)
            for candidate in package_candidates
        }
        self.assertEqual(package_members, members)
        copied_modules = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-sdk.prx",
                "guest_path": "disc0:/PSP_GAME/USRDIR/module/sdk.prx",
                "role": "guest-prx", "required": True,
            }]},
            self.temp_dir / "manifest-package", None,
        )
        self.assertIsNotNone(copied_modules)
        self.assertEqual(
            (copied_modules / "manifest-sdk.prx").read_bytes(), paths[
                "PSP_GAME/USRDIR/module/sdk.prx"
            ],
        )
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(set(native_paths), set(paths))

    def test_usrdir_prx_discovery_checklist_and_extraction_share_module_rule(self) -> None:
        iso_file = self.temp_dir / "usrdir-plain-prx.iso"
        member = "PSP_GAME/USRDIR/module/libfont.prx"
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        create_test_iso_with_module_tree(iso_file, {member: prx})

        candidates = list_disc_module_candidates(iso_file)
        self.assertEqual([candidate["members"][0] for candidate in candidates], [
            tuple(member.split("/")),
        ])
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        self.assertEqual([candidate["kind"] for candidate in package_candidates], [
            "plain-elf",
        ])

        preflight = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=self.temp_dir,
        )
        module_check = next(
            check for check in preflight["checks"] if check["code"] == "GUEST_MODULES"
        )
        self.assertEqual(module_check["status"], "OK", module_check["message"])
        self.assertIn("Guest modules: 1 of 1 ready.", module_check["message"])

        copied_modules = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-libfont.prx",
                "guest_path": f"disc0:/{member}",
                "role": "guest-prx", "required": True,
            }]},
            self.temp_dir / "usrdir-prx-package", None,
        )
        self.assertIsNotNone(copied_modules)
        self.assertEqual((copied_modules / "manifest-libfont.prx").read_bytes(), prx)

        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(native_paths, [member])
    def test_module_discovery_fails_closed_on_malformed_directory_tail(self) -> None:
        iso_file = self.temp_dir / "malformed-module-directory.iso"
        member = "PSP_GAME/USRDIR/before.prx"
        directory_lbas = create_test_iso_with_module_tree(
            iso_file, {member: build_psp_container()}
        )
        image = bytearray(iso_file.read_bytes())
        record = _dir_record(b"before.prx;1", 1, 128, False)
        malformed_offset = 2 * len(_dir_record(b"\0", 1, 2048, True)) + len(record)
        image[directory_lbas[("PSP_GAME", "USRDIR")] * 2048 + malformed_offset] = 1
        iso_file.write_bytes(image)

        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)
        self.assertEqual(native_paths, [member])

    def test_module_discovery_rejects_dot_only_directory_name(self) -> None:
        iso_file = self.temp_dir / "dot-only-module-directory.iso"
        create_test_iso_with_module_tree(iso_file, {
            "PSP_GAME/USRDIR/./sdk.prx": build_psp_container(),
        })
        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)
        self.assertEqual(native_paths, [])

    def test_module_discovery_fails_closed_when_a_root_is_a_file(self) -> None:
        iso_file = self.temp_dir / "module-root-file.iso"
        create_test_iso_with_module_tree(iso_file, {
            "PSP_GAME/SYSDIR": b"not a directory",
            "PSP_GAME/USRDIR/module/sdk.prx": build_psp_container(),
        })
        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, _native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)

    def test_native_module_walk_reports_path_limit(self) -> None:
        iso_file = self.temp_dir / "module-path-limit.iso"
        directory_name = "d" * 50
        member = f"PSP_GAME/USRDIR/{directory_name}/nested.prx"
        create_test_iso_with_module_tree(iso_file, {member: build_psp_container()})

        # The production depth and ISO record bounds put legal paths below 1200
        # bytes. Compile this C harness with a smaller cap to execute the same
        # fail-closed branch without changing the production bound.
        native_status, native_paths = self._run_native_module_walk(
            iso_file, type(self).path_limit_exe
        )
        self.assertEqual(native_status, 3)
        self.assertEqual(native_paths, [])

    def test_copy_optional_modules_falls_back_after_stale_manifest_path(self) -> None:
        iso_file = self.temp_dir / "stale-module-path.iso"
        member = "PSP_GAME/USRDIR/module/sdk.prx"
        module_bytes = build_plain_mips_elf(0xFFA0)
        create_test_iso_with_module_tree(iso_file, {member: module_bytes})
        output = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-sdk.prx",
                "role": "guest-prx",
                "required": True,
                "guest_path": "disc0:/PSP_GAME/SYSDIR/module/sdk.prx",
            }]},
            self.temp_dir / "stale-module-package", None,
        )
        self.assertIsNotNone(output)
        self.assertEqual((output / "manifest-sdk.prx").read_bytes(), module_bytes)

    def test_module_discovery_rejects_duplicate_basenames_with_both_paths(self) -> None:
        iso_file = self.temp_dir / "duplicate-module-name.iso"
        first = "PSP_GAME/USRDIR/module/shared.prx"
        second = "PSP_GAME/USRDIR/BIN/SHARED.PRX"
        create_test_iso_with_module_tree(iso_file, {
            first: build_psp_container(), second: build_psp_container(),
        })
        with self.assertRaisesRegex(
            IsoInspectionError,
            "DUPLICATE_DISC_MODULE_BASENAME.*shared.prx.*SHARED.PRX",
        ):
            list_disc_module_candidates(iso_file)
        with self.assertRaisesRegex(
            nk_cli.PackageBuildError,
            "DUPLICATE_DISC_MODULE_BASENAME.*shared.prx.*SHARED.PRX",
        ):
            nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=self.temp_dir,
        )
        module_check = next(
            check for check in report["checks"] if check["code"] == "GUEST_MODULES"
        )
        self.assertEqual(module_check["status"], "UNSUPPORTED")
        self.assertIn(first, module_check["message"])
        self.assertIn(second, module_check["message"])
        self.assertEqual(module_check["issues"], [308])

    def test_nonexperimental_bringup_keeps_module_boundary_detail_and_issue(self) -> None:
        failure, issues, detail = nk_cli._bringup_import_failure(
            nk_cli.PackageBuildError(
                "DUPLICATE_DISC_MODULE_BASENAME: shared.prx occurs twice"
            ),
            {"EXECUTABLE": {"issue_numbers": [308]}},
        )
        self.assertEqual(failure, "GUEST_MODULE_DISCOVERY_FAILED")
        self.assertEqual(issues, [308])
        self.assertEqual(
            detail, "DUPLICATE_DISC_MODULE_BASENAME: shared.prx occurs twice"
        )

    def test_module_discovery_accepts_more_than_32_candidates(self) -> None:
        iso_file = self.temp_dir / "module-candidate-many.iso"
        modules = {
            f"PSP_GAME/USRDIR/MODULES/module-{index:02}.prx": build_psp_container()
            for index in range(40)
        }
        create_test_iso_with_module_tree(iso_file, modules)
        self.assertEqual(len(list_disc_module_candidates(iso_file)), 40)
        self.assertEqual(
            len(nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")), 40
        )

    def test_module_discovery_fails_closed_above_documented_candidate_cap(self) -> None:
        iso_file = self.temp_dir / "module-candidate-cap.iso"
        modules = {
            f"PSP_GAME/USRDIR/module-{index:03}.prx": build_psp_container()
            for index in range(257)
        }
        create_test_iso_with_module_tree(iso_file, modules)
        with self.assertRaisesRegex(
            IsoInspectionError,
            "DISC_MODULE_CANDIDATE_LIMIT.*256.*larger module sets are in the works",
        ):
            list_disc_module_candidates(iso_file)
        with self.assertRaisesRegex(
            nk_cli.PackageBuildError,
            "DISC_MODULE_CANDIDATE_LIMIT.*256.*larger module sets are in the works",
        ):
            nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")

    def test_module_discovery_excludes_kmodule_but_keeps_usrdir_module(self) -> None:
        iso_file = self.temp_dir / "kernel-module-exclusion.iso"
        user_module = "PSP_GAME/USRDIR/module/user.prx"
        kernel_module = "PSP_GAME/USRDIR/KMODULE/kernel.prx"
        create_test_iso_with_module_tree(iso_file, {
            user_module: build_psp_container(),
            kernel_module: build_psp_container(),
        })
        candidates = list_disc_module_candidates(iso_file)
        self.assertEqual(
            {candidate["members"][0] for candidate in candidates},
            {tuple(user_module.split("/"))},
        )
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        self.assertEqual(
            {candidate["directory"] + (candidate["entry"].name,)
             for candidate in package_candidates},
            {tuple(user_module.split("/"))},
        )
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(native_paths, [user_module])

    def test_synthetic_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on synthetic title."""
        iso_file = self.temp_dir / "synthetic.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title", volume_id="SYNTH_VOL")

        # Python inspection
        py_meta = inspect_iso(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertTrue(py_meta.is_supported)
        self.assertIsNotNone(py_meta.matched_profile)

        # Native C inspection
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), py_meta.disc_id)
        self.assertEqual(c_meta.get("TITLE"), py_meta.title)
        self.assertEqual(c_meta.get("VERSION"), py_meta.version)
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("PARAM_SFO_PARSED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_game_sfo_wins_over_update_sfo(self) -> None:
        iso_file = self.temp_dir / "update-decoy.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="TEST00001",
            title="Synthetic Test Title",
            volume_id="VOLUME_ID",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertEqual(py_meta.title, "Synthetic Test Title")
        self.assertEqual(c_meta.get("DISC_ID"), py_meta.disc_id)
        self.assertEqual(c_meta.get("TITLE"), py_meta.title)
        self.assertEqual(c_meta.get("SUPPORTED"), "1")

    def test_psn_layout_uses_reachable_game_sfo(self) -> None:
        iso_file = self.temp_dir / "psn-layout.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 2.71")
        create_test_iso(
            iso_file,
            disc_id="NPUH10028",
            title="Super Pocket Tennis",
            volume_id="TYPE_0",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "NPUH10028")
        self.assertEqual(py_meta.title, "Super Pocket Tennis")
        self.assertEqual(c_meta.get("DISC_ID"), "NPUH10028")
        self.assertEqual(c_meta.get("TITLE"), "Super Pocket Tennis")

    def test_sfo_disc_id_and_utf8_title_override_volume_fallback(self) -> None:
        iso_file = self.temp_dir / "utf8-identity.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="TEST00002",
            title="Café® ™",
            volume_id="TEST00001",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00002")
        self.assertEqual(py_meta.title, "Café® ™")
        self.assertEqual(py_meta.matched_profile.id, "synthetic-title2-v1")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00002")
        self.assertEqual(c_meta.get("TITLE"), "Café® ™")
        self.assertEqual(c_meta.get("MATCHED_ID"), "synthetic-title2-v1")

    def test_catalog_match_uses_game_sfo_identity(self) -> None:
        from nk_core.title_registry import TitleRegistry
        from nk_core.types import TitleProfile

        iso_file = self.temp_dir / "catalog-identity.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="NPUH10028",
            title="Super Pocket Tennis",
            volume_id="TYPE_0",
            update_sfo_bytes=update_sfo,
        )
        registry = TitleRegistry(include_defaults=False)
        registry.register(TitleProfile(
            id="psn-fixture-v1",
            name="PSN Fixture",
            disc_ids=["NPUH10028"],
            regions=["NA"],
        ))

        metadata = inspect_iso(iso_file, registry)
        self.assertTrue(metadata.is_supported)
        self.assertEqual(metadata.matched_profile.id, "psn-fixture-v1")

    def test_malformed_reachable_sfo_fails_closed(self) -> None:
        from nk_core.iso_inspect import IsoInspectionError

        iso_file = self.temp_dir / "malformed-sfo.iso"
        valid_sfo = build_param_sfo("TEST00001", "Malformed Fixture")
        create_test_iso(
            iso_file,
            volume_id="TEST00001",
            sfo_bytes=valid_sfo[:32],
        )

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_file)
        native = self._run_native_inspect(iso_file)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR"))

    def test_sfo_data_length_must_not_exceed_max_length(self) -> None:
        from nk_core.iso_inspect import IsoInspectionError

        iso_file = self.temp_dir / "bad-max-length.iso"
        malformed = bytearray(build_param_sfo("TEST00001", "Bad Max Length"))
        struct.pack_into("<I", malformed, 20 + 8, 4)
        create_test_iso(
            iso_file,
            volume_id="TEST00001",
            sfo_bytes=bytes(malformed),
        )

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_file)
        native = self._run_native_inspect(iso_file)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR"))

    def test_title_id_is_not_used_as_disc_id(self) -> None:
        iso_file = self.temp_dir / "title-id.iso"
        sfo = build_custom_param_sfo([
            ("TITLE_ID", 0x0204, b"UCUS99999\0"),
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("TITLE", 0x0204, b"Identity Fixture\0"),
        ])
        create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00001")
        self.assertEqual(c_meta.get("TITLE"), "Identity Fixture")

    def test_cli_inspect_shows_structured_compatibility_preflight(self) -> None:
        iso_file = self.temp_dir / "cli-preflight.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("compatibility_preflight", payload)
        checks = payload["compatibility_preflight"]["checks"]
        self.assertEqual(
            [check["code"] for check in checks],
            ["DISC_SFO", "EXECUTABLE", "RUNTIME_PACKAGE", "DATA_ROOT", "SYSTEM_FONTS", "AUDIO_OUTPUT"],
        )
        self.assertTrue(all(check["status"] in {
            "OK", "MISSING", "UNSUPPORTED", "IN_PROGRESS",
        } for check in checks))
        by_code = {check["code"]: check for check in checks}
        self.assertEqual(by_code["DATA_ROOT"]["status"], "MISSING")
        self.assertIn("data folder", by_code["DATA_ROOT"]["message"].lower())
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["SYSTEM_FONTS"]["status"], "MISSING")
        self.assertEqual(by_code["AUDIO_OUTPUT"]["status"], "OK")

        meta = inspect_iso(iso_file)
        assert meta.matched_profile is not None
        title_name = meta.matched_profile.game_name
        package_dir = runtime_root / "build" / title_name
        package_dir.mkdir(parents=True)
        (package_dir / f"{title_name}.exe").write_bytes(b"synthetic executable")
        (package_dir / f"{title_name}_image.bin").write_bytes(b"synthetic image")
        data_root = runtime_root / "fixtures" / "profile_zero"
        data_root.mkdir(parents=True)
        font_dir = runtime_root / "font"
        font_dir.mkdir()
        (font_dir / "jpn0.pgf").write_bytes(b"synthetic font marker")
        ready = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(ready.returncode, 0, ready.stderr)
        ready_checks = {check["code"]: check for check in
                        json.loads(ready.stdout)["compatibility_preflight"]["checks"]}
        self.assertEqual(ready_checks["DATA_ROOT"]["status"], "OK")
        self.assertEqual(ready_checks["RUNTIME_PACKAGE"]["status"], "OK")
        self.assertEqual(ready_checks["SYSTEM_FONTS"]["status"], "OK")

        data_root.rmdir()
        extracted_data_root = (
            iso_file.parent / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "fixtures" / "profile_zero"
        )
        extracted_data_root.mkdir(parents=True)
        extracted_ready = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(extracted_ready.returncode, 0, extracted_ready.stderr)
        extracted_checks = {check["code"]: check for check in
                            json.loads(extracted_ready.stdout)["compatibility_preflight"]["checks"]}
        self.assertEqual(extracted_checks["DATA_ROOT"]["status"], "OK")

    def test_cli_reports_refused_local_profile_with_shared_catalog_state(self) -> None:
        iso_file = self.temp_dir / "cli-refused-profile.iso"
        user_root = self.temp_dir / "profile-user-data"
        manifest_dir = user_root / "manifests"
        manifest_dir.mkdir(parents=True)
        create_test_iso(iso_file, disc_id="TEST00007", title="Synthetic Test Title")

        manifest = json.loads((ROOT / "assets" / "titles" / "showcase-scene.json")
                              .read_text(encoding="utf-8"))
        manifest["kind"] = "retail"
        manifest["disc"] = {
            "id": "TEST00007", "region": "OTHER",
            "revision_policy": "exact-disc-id",
        }
        manifest.pop("profile_zero", None)
        manifest["runtime_bindings"] = {
            "schema_version": 1,
            "vblank_frame_counter_addr": 0x08804000,
        }
        (manifest_dir / "retired-binding.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["catalogued"])
        self.assertTrue(payload["supported"])
        self.assertIn("vblank_frame_counter_addr", payload["profile_validation"])
        human = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(human.returncode, 0, human.stderr)
        self.assertIn("Catalogued: YES", human.stdout)
        self.assertIn("Supported:  YES", human.stdout)
        self.assertIn("vblank_frame_counter_addr", human.stdout)

    def test_cli_preflight_selects_plain_boot_fallback(self) -> None:
        iso_file = self.temp_dir / "cli-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf()
        )
        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["EBOOT.BIN"]["classification"],
                         "PSP_ENCRYPTED_CONTAINER")
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        self.assertTrue(report["boot_fallback"])
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["PARAM_SFO_PARSED"], "1")
        self.assertEqual(native["EBOOT_KIND"], "2")
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")
        self.assertEqual(native["BOOT_FALLBACK"], "1")
        executable_check = next(
            check for check in report["checks"] if check["code"] == "EXECUTABLE"
        )
        self.assertEqual(executable_check["status"], "OK")
        self.assertIn("BOOT.BIN selected for analysis", executable_check["message"])

    def test_cli_preflight_guides_encrypted_eboot_to_title_scoped_user_input(self) -> None:
        iso_file = self.temp_dir / "cli-encrypted-user-input.iso"
        user_root = self.temp_dir / "user-data"
        decrypted_dir = user_root / "titles" / "TEST00001" / "decrypted"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id="TEST00001",
            title="Synthetic Test Title",
        )

        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "UNSUPPORTED")
        self.assertIn(f"supply decrypted modules at {decrypted_dir}".lower(),
                      executable["message"].lower())
        self.assertNotRegex(executable["message"], r"#[0-9]+")
        cli = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(cli.returncode, 0, cli.stderr)
        cli_checks = json.loads(cli.stdout)["compatibility_preflight"]["checks"]
        cli_executable = next(check for check in cli_checks
                              if check["code"] == "EXECUTABLE")
        self.assertIn(str(decrypted_dir), cli_executable["message"])
        self.assertIn("matching local key file", cli_executable["message"])
        self.assertNotIn("automatic decryption is in the works", cli_executable["message"].lower())
        self.assertNotRegex(cli_executable["message"], r"#[0-9]+")

        decrypted_dir.mkdir(parents=True)
        eboot = decrypted_dir / "EBOOT.elf"
        eboot.write_bytes(b"not an ELF")
        invalid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        invalid_check = next(check for check in invalid["checks"]
                             if check["code"] == "EXECUTABLE")
        self.assertEqual(invalid_check["status"], "UNSUPPORTED")
        self.assertIn("invalid", invalid_check["message"].lower())
        self.assertNotIn("in the works", invalid_check["message"].lower())
        self.assertEqual(invalid_check["issues"], [])

        eboot.write_bytes(build_plain_mips_elf())
        valid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        valid_check = next(check for check in valid["checks"]
                           if check["code"] == "EXECUTABLE")
        self.assertEqual(valid_check["status"], "OK")
        self.assertEqual(valid["selected_executable"], "EBOOT.elf")
        self.assertEqual(valid["decrypted_executable"], str(eboot))
        for name, wrapped in (("sce", b"~SCE" + bytes(124)),
                              ("pbp", b"\0PBP" + bytes(124))):
            wrapped_iso = self.temp_dir / f"cli-wrapped-{name}.iso"
            create_test_iso_with_executables(
                wrapped_iso, wrapped, disc_id="TEST00001",
                title="Synthetic Test Title",
            )
            wrapped_meta = inspect_iso(wrapped_iso)
            wrapped_report = inspect_compatibility_preflight(
                wrapped_iso, metadata=wrapped_meta, runtime_root=user_root
            )
            wrapped_check = next(check for check in wrapped_report["checks"]
                                 if check["code"] == "EXECUTABLE")
            self.assertEqual(wrapped_check["status"], "OK")
            self.assertEqual(wrapped_report["selected_executable"], "EBOOT.elf")

    def test_build_package_discovers_user_decrypted_elf_and_modules(self) -> None:
        iso_file = self.temp_dir / "synthetic-package-input.iso"
        user_root = self.temp_dir / "package-user-data"
        disc_id = "TEST00002"
        title_id = "synthetic-title2-v1"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id=disc_id,
            title="Synthetic Title 2 Fixture",
        )
        entry = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        user_root.mkdir()
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry]}),
            encoding="utf-8",
        )
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True)
        eboot_bytes = build_plain_mips_elf()
        module_bytes = build_plain_mips_elf(0xFFA0)
        (decrypted_dir / "EBOOT.elf").write_bytes(eboot_bytes)
        (decrypted_dir / "synthetic2.prx").write_bytes(module_bytes)

        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["modules"][0]["required"] = True
        manifest["modules"][0]["role"] = "guest-prx"
        captured_command: list[str] = []

        def fake_package_build(command, **_kwargs):
            captured_command.extend(command)
            build_dir = Path(command[command.index("--output-dir") + 1])
            build_dir.mkdir(parents=True, exist_ok=True)
            module_path = Path(command[command.index("--module-dir") + 1])
            identity = json.loads(Path(
                command[command.index("--title-input-identity-file") + 1]
            ).read_text(encoding="utf-8"))
            cache_key = nk_cli._current_package_cache_key(
                manifest,
                Path(command[1]),
                hashlib.sha256(eboot_bytes).hexdigest(),
                module_path,
                None,
                identity,
            )
            executable = build_dir / "synthetic.exe"
            image = build_dir / "synthetic_image.bin"
            generated = build_dir / "synthetic_recomp.o"
            executable.write_bytes(b"fake executable")
            image.write_bytes(b"fake image")
            generated.write_bytes(b"fake object")
            cache = nk_cli.package_cache.cache_metadata(
                cache_key,
                nk_cli._package_codegen_options(manifest, os.environ),
            )
            package = {
                "format": "nakagawa-aot-package",
                "schema_version": 2,
                "cache": cache,
                "title": {"id": title_id},
                "inputs": {
                    "manifest": {"sha256": cache_key["aot"]["components"]["manifest_sha256"]},
                    "executable": {"sha256": hashlib.sha256(eboot_bytes).hexdigest()},
                    "modules": [{
                        "name": "synthetic2.prx",
                        "load_address": f"0x{int(manifest['modules'][0]['load_address']):08x}",
                        "sha256": hashlib.sha256(module_bytes).hexdigest(),
                    }],
                    "psp_header": None,
                },
                "title_input_identity": identity,
                "executable": {
                    "path": executable.name,
                    "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                },
                "generated_objects": [{
                    "path": generated.name,
                    "sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
                }],
            }
            report = {"cache": cache}
            (build_dir / "package.json").write_text(
                json.dumps(package), encoding="utf-8"
            )
            (build_dir / "build-report.json").write_text(
                json.dumps(report), encoding="utf-8"
            )
            nk_cli.package_cache.write_completion_manifest(
                build_dir, cache_key, title_input_identity=identity
            )
            return subprocess.CompletedProcess(command, 0, "", "")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()
        with patch.object(nk_cli, "_load_entry_manifest",
                          return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_package_build), \
             patch.object(nk_cli, "_stage_runtime_assets"):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)

        game_elf = Path(captured_command[captured_command.index("--game-elf") + 1])
        module_dir = Path(captured_command[captured_command.index("--module-dir") + 1])
        self.assertEqual(game_elf.read_bytes(), eboot_bytes)
        self.assertEqual((module_dir / "synthetic2.prx").read_bytes(), module_bytes)
        captured_command.clear()
        with patch.object(nk_cli, "_load_entry_manifest",
                          return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_package_build), \
             patch.object(nk_cli, "_stage_runtime_assets"):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)
        self.assertEqual(captured_command, [])
        (decrypted_dir / "synthetic2.prx").write_bytes(b"not an ELF")
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a decrypted ELF"):
            nk_cli._copy_optional_modules(
                iso_file, manifest, user_root / "bad-module-cache", None,
                decrypted_dir,
            )

    def test_package_build_mode_selection_and_limits(self) -> None:
        disc_id = "TEST00004"
        disc_id_priv = "TEST00005"
        user_root = self.temp_dir / "user-data-mode"
        user_root.mkdir(parents=True, exist_ok=True)
        iso_file = self.temp_dir / "mode-test.iso"
        eboot_bytes = build_plain_mips_elf()
        create_test_iso_with_executables(iso_file, build_psp_container(), disc_id=disc_id, title="Mode Test Title")
        iso_file_priv = self.temp_dir / "mode-test-priv.iso"
        create_test_iso_with_executables(iso_file_priv, build_psp_container(), disc_id=disc_id_priv, title="Mode Test Title Priv")

        title_id = "synthetic-title2-v1"
        entry_pub = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        entry_priv = {
            "disc_id": disc_id_priv,
            "title_id": title_id,
            "iso_path": str(iso_file_priv),
            "selected_executable": "",
            "is_experimental": False,
        }
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry_pub, entry_priv]}),
            encoding="utf-8",
        )
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True, exist_ok=True)
        (decrypted_dir / "EBOOT.elf").write_bytes(eboot_bytes)

        decrypted_dir_priv = user_root / "titles" / disc_id_priv / "decrypted"
        decrypted_dir_priv.mkdir(parents=True, exist_ok=True)
        (decrypted_dir_priv / "EBOOT.elf").write_bytes(eboot_bytes)

        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        captured_commands: list[list[str]] = []

        def fake_run(command, **_kwargs):
            captured_commands.append(list(command))
            build_dir = Path(command[command.index("--output-dir") + 1])
            build_dir.mkdir(parents=True, exist_ok=True)
            executable = build_dir / "synthetic.exe"
            image = build_dir / "synthetic_image.bin"
            generated = build_dir / "synthetic_recomp.o"
            executable.write_bytes(b"fake executable")
            image.write_bytes(b"fake image")
            generated.write_bytes(b"fake object")
            (build_dir / "package.json").write_text(
                json.dumps({
                    "cache": {"format": "nakagawa-aot-cache", "schema_version": 1, "key": {}, "codegen_options": {}, "runtime_abi_compatibility": {"current_epoch": 1, "generated_code_reusable": True}},
                    "title": {"id": title_id},
                    "inputs": {"manifest": {"sha256": "0" * 64}, "executable": {"sha256": hashlib.sha256(eboot_bytes).hexdigest()}, "modules": [], "psp_header": None},
                    "executable": {"path": executable.name, "sha256": hashlib.sha256(b"fake executable").hexdigest()},
                    "generated_objects": [{"path": generated.name, "sha256": hashlib.sha256(b"fake object").hexdigest()}],
                }),
                encoding="utf-8",
            )
            # Like title_codegen_plan.build_package, the report records the backend
            # mode the command line selected (the real report is covered end to end
            # in test_production_smoke).
            public = "--public-safe" in command
            (build_dir / "build-report.json").write_text(
                json.dumps({
                    "cache": {"format": "nakagawa-aot-cache", "schema_version": 1, "key": {}, "codegen_options": {}, "runtime_abi_compatibility": {"current_epoch": 1, "generated_code_reusable": True}},
                    "backends": "public" if public else "private",
                    "limits": [
                        "Fonts: import your own PSP fonts; the public PGF reader is available for supported inputs.",
                        "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
                    ] if public else [],
                }),
                encoding="utf-8",
            )
            return subprocess.CompletedProcess(command, 0, "", "")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()

        captured_commands.clear()
        with patch.object(nk_cli, "_load_entry_manifest", return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli, "_has_private_backends", return_value=False), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_run), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             patch.object(nk_cli.package_cache, "validate_package_cache", return_value=(True, "")):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)
            self.assertTrue(len(captured_commands) > 0)
            self.assertIn("--public-safe", captured_commands[0])
            report = json.loads((user_root / "packages" / disc_id / "build-report.json").read_text(encoding="utf-8"))
            self.assertEqual(report.get("backends"), "public")
            self.assertEqual(report.get("limits"), [
                "Fonts: import your own PSP fonts; the public PGF reader is available for supported inputs.",
                "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
            ])
            completion = json.loads((user_root / "packages" / disc_id / "completion-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(completion.get("backends"), "public")
            self.assertEqual(completion.get("limits"), [
                "Fonts: import your own PSP fonts; the public PGF reader is available for supported inputs.",
                "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
            ])

        # Test private mode when private backends are present
        disc_id_priv = "TEST00005"
        args_priv = type("BuildArgs", (), {
            "disc_id": disc_id_priv,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()
        captured_commands.clear()
        with patch.object(nk_cli, "_load_entry_manifest", return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli, "_has_private_backends", return_value=True), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_run), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             patch.object(nk_cli.package_cache, "validate_package_cache", return_value=(True, "")):
            self.assertEqual(nk_cli.cmd_build_package(args_priv), 0)
            self.assertTrue(len(captured_commands) > 0)
            self.assertNotIn("--public-safe", captured_commands[0])
            report_priv = json.loads((user_root / "packages" / disc_id_priv / "build-report.json").read_text(encoding="utf-8"))
            self.assertEqual(report_priv.get("backends"), "private")
            self.assertEqual(report_priv.get("limits"), [])
            completion_priv = json.loads((user_root / "packages" / disc_id_priv / "completion-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(completion_priv.get("backends"), "private")
            self.assertEqual(completion_priv.get("limits"), [])

    def test_prx_format_boot_fallback_is_selected_by_both_inspectors(self) -> None:
        """Retail BOOT.BIN is often PSP PRX-format (e_type 0xFFA0), which the
        analyzer and runtime loader accept; the preflight must not call it
        unusable."""
        iso_file = self.temp_dir / "prx-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root-prx"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf(0xFFA0)
        )
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")

    def test_second_title_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on second public synthetic disc ID."""
        iso_file = self.temp_dir / "test5_mock.iso"
        create_test_iso(iso_file, disc_id="TEST00005", title="Phase 5 Test", volume_id="TEST00005")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00005")
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), "pspdev-phase5-v1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_uncataloged_retail_iso_parity(self) -> None:
        """Verify retail disc IDs are uncataloged in public mode by both inspectors."""
        iso_file = self.temp_dir / "retail_mock.iso"
        create_test_iso(iso_file, disc_id="UCUS98701", title="Hot Shots Tennis", volume_id="UCUS98701")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "UCUS98701")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_uncatalogued_param_sfo_disc_is_experimental(self) -> None:
        """Unknown PSP discs get validated local identity profiles and stay launch-gated."""
        iso_file = self.temp_dir / "experimental_mock.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        executable_bytes = build_plain_mips_elf() + bytes((i % 251 for i in range(70000)))
        create_test_iso_with_executables(
            iso_file, executable_bytes, disc_id="ULUS99998",
            title="Experimental Fixture",
        )

        metadata = inspect_iso(iso_file)
        self.assertIsNone(metadata.matched_profile)
        preflight = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertTrue(preflight["is_experimental"])
        by_code = {check["code"]: check for check in preflight["checks"]}
        self.assertEqual(by_code["EXPERIMENTAL"]["issues"], [308])
        self.assertIn("Compatibility is unknown", by_code["EXPERIMENTAL"]["message"])
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["issues"], [308])

        python_root = self.temp_dir / "python-user-data"
        python_profile_path = write_experimental_profile(
            iso_file, python_root, metadata=metadata
        )
        python_profile = json.loads(python_profile_path.read_text(encoding="utf-8"))
        from title_manifest import validate_manifest
        manifest = validate_manifest(python_profile["manifest"])
        identity = python_profile["input_identity"]
        expected_hash = hashlib.sha256(executable_bytes).hexdigest()
        self.assertEqual(manifest["disc"]["id"], "ULUS99998")
        self.assertEqual(manifest["display_name"], "Experimental Fixture")
        self.assertEqual(manifest["executable"]["base"], 0)
        self.assertEqual(manifest["executable"]["entry"], 0x08800000)
        self.assertEqual(manifest["codegen_profile"], "none")
        self.assertEqual(manifest["modules"], [])
        self.assertEqual(identity["selected_executable"], "PSP_GAME/SYSDIR/EBOOT.BIN")
        self.assertEqual(identity["executable_sha256"], expected_hash)
        self.assertEqual(identity["elf_sha256"], expected_hash)
        self.assertTrue(python_profile_path.is_relative_to(python_root))

        native_root = self.temp_dir / "native-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:OK", native.stdout)
        self.assertIn("PACKAGE_AVAILABLE:0", native.stdout)
        self.assertIn("LAUNCH_PREPARE:REFUSED", native.stdout)
        native_profile_path = native_root / "experimental" / "ULUS99998" / "profile.json"
        native_profile = json.loads(native_profile_path.read_text(encoding="utf-8"))
        native_manifest = validate_manifest(native_profile["manifest"])
        self.assertEqual(native_manifest["id"], "experimental-ulus99998")
        self.assertEqual(native_profile["input_identity"]["executable_sha256"], expected_hash)
        self.assertEqual(native_profile["input_identity"]["elf_sha256"], expected_hash)

    def test_experimental_import_uses_documented_load_binding_for_relocatable_elf(self) -> None:
        """Relocatable main modules use the documented generic PSP load base."""
        from title_manifest import validate_manifest

        for e_type in (3, 0xFFA0):
            with self.subTest(e_type=e_type):
                iso_file = self.temp_dir / f"relocatable_{e_type:04x}.iso"
                create_test_iso_with_executables(
                    iso_file, build_plain_mips_elf(e_type=e_type),
                    disc_id="ULUS99997", title="Synthetic Relocatable Fixture",
                )
                user_root = self.temp_dir / f"relocatable-user-data-{e_type:04x}"
                metadata = inspect_iso(iso_file)
                profile_path = write_experimental_profile(
                    iso_file, user_root, metadata=metadata
                )
                profile = json.loads(profile_path.read_text(encoding="utf-8"))
                executable = validate_manifest(profile["manifest"])["executable"]
                self.assertEqual(executable["base"], 0x08804000)
                self.assertEqual(executable["load_address"], 0x08804000)
                self.assertEqual(
                    executable["load_address_evidence"], "documented-psp-default"
                )
                self.assertTrue(profile_path.is_relative_to(user_root))

    def test_provisional_guest_module_placement_is_deterministic_and_disjoint(self) -> None:
        main_elf = self.temp_dir / "placement-main.elf"
        alpha = self.temp_dir / "alpha.prx"
        beta = self.temp_dir / "beta.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, memsz=0x40000))
        alpha.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x21001))
        beta.write_bytes(build_plain_mips_elf(e_type=3, vaddr=0, memsz=0x17001))
        inputs = [
            ("beta.prx", beta, "disc0:/PSP_GAME/USRDIR/beta.prx"),
            ("alpha.prx", alpha, "disc0:/PSP_GAME/SYSDIR/alpha.prx"),
        ]

        first = plan_provisional_module_bindings(main_elf, inputs)
        second = plan_provisional_module_bindings(main_elf, list(reversed(inputs)))
        self.assertEqual(first, second)
        self.assertEqual([module["name"] for module in first], ["alpha.prx", "beta.prx"])
        self.assertTrue(all(module["load_address_evidence"] == "provisional" for module in first))
        self.assertTrue(all(module["role"] == "guest-prx" and module["required"] for module in first))
        spans = {"alpha.prx": 0x21001, "beta.prx": 0x17001}
        ranges = sorted(
            (module["load_address"], module["load_address"] + spans[module["name"]])
            for module in first
        )
        self.assertGreaterEqual(ranges[0][0], 0x08800000 + 0x40000 + 0x00100000)
        self.assertLessEqual(ranges[-1][1], 0x09EF0000)
        self.assertLess(ranges[0][1], ranges[1][0])

    def test_provisional_guest_module_placement_fails_when_no_safe_span_remains(self) -> None:
        main_elf = self.temp_dir / "placement-full-main.elf"
        huge_a = self.temp_dir / "huge-a.prx"
        huge_b = self.temp_dir / "huge-b.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        huge_a.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x01000000))
        huge_b.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x01000000))
        with self.assertRaisesRegex(ValueError, "do not fit above the main-image heap reserve") as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("a.prx", huge_a, "disc0:/PSP_GAME/USRDIR/a.prx"),
                 ("b.prx", huge_b, "disc0:/PSP_GAME/USRDIR/b.prx")],
            )
        self.assertEqual(
            getattr(ctx.exception, "boundary_code", None),
            "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE",
        )

    def test_provisional_guest_module_placement_fails_when_main_image_exhausts_safe_span(self) -> None:
        main_elf = self.temp_dir / "placement-huge-main.elf"
        module_a = self.temp_dir / "mod-a.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x016F0000))
        module_a.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x1000))
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("a.prx", module_a, "disc0:/PSP_GAME/USRDIR/a.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE")
        self.assertIn("leaves no safe guest-module address range", str(ctx.exception))

    def test_provisional_guest_module_placement_encrypted_module_raises_decryption_required(self) -> None:
        main_elf = self.temp_dir / "placement-main-enc.elf"
        enc_module = self.temp_dir / "enc.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        enc_module.write_bytes(build_psp_container())
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("enc.prx", enc_module, "disc0:/PSP_GAME/USRDIR/enc.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_DECRYPTION_REQUIRED")

    def test_provisional_guest_module_placement_overlapping_segments_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-ovl.elf"
        ovl_module = self.temp_dir / "ovl.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        ovl_module.write_bytes(build_overlapping_mips_elf())
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("ovl.prx", ovl_module, "disc0:/PSP_GAME/USRDIR/ovl.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("overlapping loadable segments", str(ctx.exception))

    def test_provisional_guest_module_placement_fixed_address_module_support_and_collision(self) -> None:
        main_elf = self.temp_dir / "placement-main-fixed.elf"
        fixed_ok = self.temp_dir / "fixed_ok.prx"
        fixed_bad = self.temp_dir / "fixed_bad.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        fixed_ok.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x09000000))
        placed = plan_provisional_module_bindings(
            main_elf,
            [("fixed_ok.prx", fixed_ok, "disc0:/PSP_GAME/USRDIR/fixed_ok.prx")],
        )
        self.assertEqual(len(placed), 1)
        self.assertEqual(placed[0]["load_address"], 0x09000000)
        self.assertEqual(placed[0]["load_address_evidence"], "fixed-address")

        fixed_bad.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x08810000))
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("fixed_bad.prx", fixed_bad, "disc0:/PSP_GAME/USRDIR/fixed_bad.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_LOAD_BINDING_REQUIRED")

    # Synthetic NIDs for the exact HLE-served rule. The registry is injected so
    # these cases pin the rule itself, independent of what src/rt/hle.c serves.
    SERVED_A, SERVED_B, SERVED_C = 0x5EED0001, 0x5EED0002, 0x5EED0003
    UNSERVED_A, UNSERVED_B, UNSERVED_C = 0x0BAD0001, 0x0BAD0002, 0x0BAD0003

    def _plan_with_registry(self, main_elf: Path, modules: dict[str, bytes], registered=None):
        inputs = []
        for name, blob in modules.items():
            path = self.temp_dir / name
            path.write_bytes(blob)
            inputs.append((name, path, f"disc0:/PSP_GAME/USRDIR/{name}"))
        registry = frozenset(
            {self.SERVED_A, self.SERVED_B, self.SERVED_C} if registered is None else registered
        )
        with patch.object(iso_inspect, "runtime_registered_nids", return_value=registry):
            return plan_provisional_module_bindings(main_elf, inputs)

    def _main_importing(self, imports: list[tuple[str, list[int]]]) -> Path:
        main_elf = self.temp_dir / "placement-main-imports.elf"
        main_elf.write_bytes(
            build_module_elf([SYSLIB_EXPORT], imports=imports, e_type=2, base_vaddr=BASE_VADDR)
        )
        return main_elf

    def test_module_the_runtime_serves_completely_is_not_placed(self) -> None:
        main_elf = self.temp_dir / "placement-main-served.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        placed = self._plan_with_registry(main_elf, {
            # user-mode library: every exported function registered
            "served_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A, self.SERVED_B], [])]
            ),
            # kernel-mode driver: its syscall-exported library is fully registered;
            # the kernel-only library is not callable from user mode
            "served_driver.prx": build_module_elf(
                [
                    SYSLIB_EXPORT,
                    ("SynthCodec", 0x4001, [self.SERVED_C], []),
                    ("SynthCodec_driver", 0x0001, [self.UNSERVED_A], []),
                ],
                module_attributes=0x1006,
            ),
            # one callable function the runtime lacks keeps the module translated
            "partial_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthVideo", 0x0001, [self.SERVED_A, self.UNSERVED_B], [])]
            ),
            # the runtime registers functions only, so an exported variable keeps it
            "variable_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthData", 0x0001, [self.SERVED_B], [self.UNSERVED_C])]
            ),
            # an overlay that only runs from module_start exports nothing callable
            "overlay.prx": build_module_elf([SYSLIB_EXPORT]),
        })
        self.assertEqual(
            [module["name"] for module in placed],
            ["overlay.prx", "partial_user.prx", "variable_user.prx"],
        )

    def test_firmware_kernel_module_the_main_executable_does_not_need_is_left_out(self) -> None:
        """A kernel driver used only by a firmware library never blocks the title."""
        main_elf = self._main_importing([("SynthVideo", [self.SERVED_A])])
        placed = self._plan_with_registry(main_elf, {
            "video_library.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthVideo", 0x0001, [self.SERVED_A, self.UNSERVED_B], [])],
                imports=[("SynthVideoDriver", [self.UNSERVED_A])],
            ),
            "video_driver.prx": build_module_elf(
                [
                    SYSLIB_EXPORT,
                    ("SynthVideoDriver", 0x4001, [self.UNSERVED_A, self.UNSERVED_C], []),
                    ("SynthVideoDriver_driver", 0x0001, [0x0BAD0010], []),
                ],
                module_attributes=0x1006,
            ),
            "lifecycle_only_driver.prx": build_module_elf([SYSLIB_EXPORT], module_attributes=0x1006),
        })
        self.assertEqual([module["name"] for module in placed], ["video_library.prx"])

    def test_kernel_module_the_main_executable_needs_raises_format_unsupported(self) -> None:
        main_elf = self._main_importing(
            [("SynthDriver", [self.UNSERVED_A]), ("SynthAudio", [self.SERVED_A])]
        )
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A, self.UNSERVED_B], [])],
            module_attributes=0x1006,
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"title_driver.prx": driver})
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("requires PSP kernel mode: title_driver.prx", str(ctx.exception))
        self.assertIn(f"0x{self.UNSERVED_A:08x}", str(ctx.exception))
        self.assertNotIn(f"0x{self.UNSERVED_B:08x}", str(ctx.exception))

    def test_kernel_module_import_is_not_a_need_when_something_else_provides_it(self) -> None:
        main_elf = self._main_importing([("SynthDriver", [self.UNSERVED_A])])
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        # the runtime registers the imported NID
        placed = self._plan_with_registry(
            main_elf, {"driver.prx": driver},
            registered={self.SERVED_A, self.UNSERVED_A},
        )
        self.assertEqual(placed, [])
        # a placed user-mode module exports the imported NID
        placed = self._plan_with_registry(main_elf, {
            "driver.prx": driver,
            "user_impl.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthDriver", 0x0001, [self.UNSERVED_A], [])]
            ),
        })
        self.assertEqual([module["name"] for module in placed], ["user_impl.prx"])
        # the import names a kernel-only library, which user code cannot link to
        kernel_only = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x0001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        self.assertEqual(self._plan_with_registry(main_elf, {"driver.prx": kernel_only}), [])

    def test_unreadable_main_import_table_names_the_analyzer_boundary(self) -> None:
        blob = bytearray(
            build_module_elf(
                [SYSLIB_EXPORT], imports=[("SynthDriver", [self.UNSERVED_A])],
                e_type=2, base_vaddr=BASE_VADDR,
            )
        )
        data_off = 0x1000  # import_fixtures.DATA_FILE_OFF: the segment's file offset
        stub_top = struct.unpack_from("<I", blob, data_off + 44)[0]
        struct.pack_into("<I", blob, data_off + (stub_top - BASE_VADDR) + 12, 0)  # nidData = 0
        main_elf = self.temp_dir / "placement-main-bad-imports.elf"
        main_elf.write_bytes(bytes(blob))
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"driver.prx": driver})
        self.assertEqual(ctx.exception.boundary_code, "ANALYZER_IMPORT_NID_TABLE_MISSING")

    def test_malformed_export_table_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-bad-exports.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        broken = build_module_elf(
            [SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A], [])], corrupt="entry_overrun"
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"broken.prx": broken})
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("invalid module or export table: broken.prx", str(ctx.exception))

    def test_runtime_registry_failure_stops_planning_with_a_named_boundary(self) -> None:
        runtime_registered_nids.cache_clear()
        self.addCleanup(runtime_registered_nids.cache_clear)
        main_elf = self.temp_dir / "placement-main-registry.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        library = self.temp_dir / "library.prx"
        library.write_bytes(
            build_module_elf([SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A], [])])
        )
        overlay = self.temp_dir / "overlay.prx"
        overlay.write_bytes(build_module_elf([SYSLIB_EXPORT]))
        failures = (
            hle_manifest.ManifestError("registration the extractor cannot account for"),
            OSError("hle.c is missing"),
        )
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                with patch.object(hle_manifest, "registered_nids", side_effect=failure):
                    with self.assertRaises(RuntimeRegistryUnavailableError) as ctx:
                        plan_provisional_module_bindings(
                            main_elf, [("library.prx", library, "disc0:/library.prx")]
                        )
                    self.assertEqual(
                        ctx.exception.boundary_code, "RUNTIME_HLE_REGISTRY_UNAVAILABLE"
                    )
                    self.assertIn(str(failure), str(ctx.exception))
                    # A module with nothing to compare never consults the registry.
                    placed = plan_provisional_module_bindings(
                        main_elf, [("overlay.prx", overlay, "disc0:/overlay.prx")]
                    )
                    self.assertEqual([module["name"] for module in placed], ["overlay.prx"])

    def test_live_runtime_registry_drives_the_served_decision(self) -> None:
        live = runtime_registered_nids()
        self.assertEqual(live, hle_manifest.registered_nids())
        some_registered = sorted(live)[:3]
        unregistered = next(nid for nid in range(0x0BAD0000, 0x0BAE0000) if nid not in live)
        served = read_guest_module_interface(
            "driver.prx",
            build_module_elf(
                [SYSLIB_EXPORT, ("SynthCodec", 0x4001, some_registered, [])],
                module_attributes=0x1006,
            ),
        )
        self.assertTrue(served.requires_kernel)
        self.assertTrue(runtime_serves_module(served, live))
        partial = read_guest_module_interface(
            "driver.prx",
            build_module_elf(
                [SYSLIB_EXPORT, ("SynthCodec", 0x4001, [*some_registered, unregistered], [])],
                module_attributes=0x1006,
            ),
        )
        self.assertFalse(runtime_serves_module(partial, live))

    def test_provisional_guest_module_placement_corrupt_elf_header_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-corrupt.elf"
        corrupt_module = self.temp_dir / "corrupt.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        corrupt_module.write_bytes(b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 30)
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_provisional_module_bindings(
                main_elf,
                [("corrupt.prx", corrupt_module, "disc0:/PSP_GAME/USRDIR/corrupt.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")

    def test_non_psp_iso_without_directory_reachable_sfo_is_refused(self) -> None:
        """An embedded but unreferenced PARAM.SFO does not authorize experimental import."""
        iso_file = self.temp_dir / "non-psp.iso"
        create_test_iso(iso_file, disc_id="ULUS99997", title="Orphaned SFO")
        data = bytearray(iso_file.read_bytes())
        root_off = 33 * 2048
        original_root = bytes(data[root_off:root_off + 2048])
        data[root_off:root_off + 2048] = bytes(2048)
        # Keep only the ISO root's self and parent records, removing PSP_GAME.
        data[root_off:root_off + 68] = original_root[:68]
        iso_file.write_bytes(data)

        metadata = inspect_iso(iso_file)
        self.assertEqual(metadata.disc_id, "UNKNOWN")
        self.assertEqual(metadata.title, "Unknown PSP Title")
        runtime_root = self.temp_dir / "empty-runtime-root-non-psp"
        runtime_root.mkdir()
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertFalse(report["is_experimental"])
        self.assertEqual(report["checks"][0]["code"], "DISC_SFO")
        with self.assertRaisesRegex(ValueError, "requires PARAM.SFO"):
            write_experimental_profile(iso_file, self.temp_dir / "refused-user-data",
                                       metadata=metadata)

        native_root = self.temp_dir / "native-refused-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:ERROR:Experimental profiles require", native.stdout)
        self.assertFalse((native_root / "experimental").exists())

    def test_pbp_package_is_named_identically_in_both_tiers(self) -> None:
        """A PBP package is refused by boundary code and one plain sentence."""
        package = self.temp_dir / "store-package.iso"
        build_pbp_package(package, disc_id="TEST00424", title="Store Package")

        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(package)
        message = str(caught.exception)
        self.assertEqual(caught.exception.boundary_code, "PBP_PACKAGE_UNSUPPORTED")
        self.assertEqual(
            message,
            "This is a PlayStation Store package (PBP), not a disc image. "
            "Nakagawa Recomp can't use these yet. Title: Store Package (ID: TEST00424)",
        )
        self.assertNotRegex(message, r"#\d+")
        self.assertNotIn("ISO9660", message)

        native = self._run_native_inspect(package)
        self.assertEqual(native.get("BOUNDARY"), caught.exception.boundary_code)
        self.assertEqual(native.get("MESSAGE"), message)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR:-3:"), native)

    def test_pbp_hostile_packages_fail_closed_in_both_tiers(self) -> None:
        """Every hostile package gets the same boundary code and sentence in both tiers."""
        oversized_sfo = build_param_sfo("TEST00424", "Oversized") + bytes(70000)
        past_eof = 40 + len(build_param_sfo("TEST00424", "Past EOF"))
        cases: list[tuple[str, dict]] = [
            ("header-truncated", {"header": b"\x00PBP\x00\x00\x01\x00", "sfo_bytes": b""}),
            ("offsets-descending", {"offsets": [40, 16, 40, 40, 40, 40, 40, 40]}),
            ("offset-past-eof", {"offsets": [40, past_eof, past_eof, past_eof,
                                             past_eof, past_eof, past_eof, past_eof + 64]}),
            ("empty-sfo", {"offsets": [40] * 8, "sfo_bytes": b""}),
            ("sfo-bad-magic", {"sfo_bytes": bytes(32)}),
            ("sfo-oversized", {"sfo_bytes": oversized_sfo}),
            ("disc-id-invalid", {"disc_id": "NODISCID"}),
            ("title-absent", {"sfo_bytes": build_custom_param_sfo(
                [("DISC_ID", 0x0204, b"TEST00424\0")])}),
            ("title-controls", {"title": "Line\tBreak\nTitle"}),
        ]
        for name, kwargs in cases:
            with self.subTest(name=name):
                package = self.temp_dir / f"{name}.iso"
                build_pbp_package(package, **kwargs)
                with self.assertRaises(IsoInspectionError) as caught:
                    inspect_iso(package)
                message = str(caught.exception)
                self.assertIsNotNone(caught.exception.boundary_code)
                self.assertRegex(caught.exception.boundary_code or "", r"^PBP_[A-Z_]+$")
                self.assertNotRegex(message, r"#\d+")
                native = self._run_native_inspect(package)
                self.assertEqual(
                    native.get("BOUNDARY"), caught.exception.boundary_code, native
                )
                self.assertEqual(native.get("MESSAGE"), message, native)

    def test_unsupported_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on unsupported disc ID."""
        iso_file = self.temp_dir / "unsupported.iso"
        create_test_iso(iso_file, disc_id="ULUS99999", title="Random Unsupported Game", volume_id="ULUS99999")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "ULUS99999")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_native_library_crud(self) -> None:
        """Verify native C library persistence (add, save, load, lookup, remove)."""
        lib_json = self.temp_dir / "library.json"
        cmd = [str(self.exe_path), "library_test", str(lib_json)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Library test failed: {res.stderr}")
        self.assertIn("LIBRARY_TEST_PASSED", res.stdout)

    def test_native_boot_executable_byte_limit_round_trip_and_overflow_refusal(self) -> None:
        root = self.temp_dir / "boot-limit-root"
        root.mkdir()

        def write_library(name: str, boot_executable: str) -> Path:
            path = self.temp_dir / name
            path.write_text(json.dumps({
                "schema_version": 1,
                "games": [{
                    "disc_id": "TEST00001",
                    "title_name": "Synthetic",
                    "title_id": "synthetic-allegrex-v1",
                    "iso_path": "synthetic.iso",
                    "selected_executable": "EBOOT.BIN",
                    "boot_executable": boot_executable,
                }],
            }, ensure_ascii=False), encoding="utf-8")
            return path

        overlong = write_library("boot-overlong.json", "X" * 256)
        rejected_save = self.temp_dir / "boot-rejected-save.json"
        rejected_save_run = subprocess.run(
            [str(self.exe_path), "boot_save_unterminated", str(rejected_save)],
            capture_output=True, text=True,
        )
        self.assertEqual(rejected_save_run.returncode, 0, rejected_save_run.stderr)
        self.assertIn("SAVE_RESULT:-12", rejected_save_run.stdout)
        self.assertFalse(rejected_save.exists())

        rejected = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(overlong),
             str(self.temp_dir / "must-not-be-saved.json"), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(rejected.returncode, 0, rejected.stderr)
        self.assertIn("LOAD_RESULT:-12", rejected.stdout)

        valid_name = '"' * 255
        self.assertEqual(len(valid_name.encode("utf-8")), 255)
        valid = write_library("boot-valid.json", valid_name)
        saved = self.temp_dir / "boot-valid-saved.json"
        roundtrip = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(valid), str(saved), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(roundtrip.returncode, 0, roundtrip.stderr)
        self.assertIn("LOAD_RESULT:0", roundtrip.stdout)
        self.assertIn("BOOT_LENGTH:255", roundtrip.stdout)
        self.assertIn("SAVE_BOOT_MATCH:1", roundtrip.stdout)
        self.assertIn("SESSION_BOOT_MATCH:1", roundtrip.stdout)
        self.assertEqual(json.loads(saved.read_text(encoding="utf-8"))["games"][0]["boot_executable"], valid_name)

        utf8_name = "é" * 127 + "A"
        self.assertEqual(len(utf8_name.encode("utf-8")), 255)
        utf8_input = write_library("boot-valid-utf8.json", utf8_name)
        utf8_saved = self.temp_dir / "boot-valid-utf8-saved.json"
        utf8_roundtrip = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(utf8_input),
             str(utf8_saved), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(utf8_roundtrip.returncode, 0, utf8_roundtrip.stderr)
        self.assertIn("BOOT_LENGTH:255", utf8_roundtrip.stdout)
        self.assertIn("SAVE_BOOT_MATCH:1", utf8_roundtrip.stdout)
        self.assertIn("SESSION_BOOT_MATCH:1", utf8_roundtrip.stdout)
        self.assertEqual(
            json.loads(utf8_saved.read_text(encoding="utf-8"))["games"][0]["boot_executable"],
            utf8_name,
        )

        unterminated_session = subprocess.run(
            [str(self.exe_path), "boot_session_unterminated"],
            capture_output=True, text=True,
        )
        self.assertEqual(unterminated_session.returncode, 0, unterminated_session.stderr)
        self.assertIn("SESSION_RESULT:-12", unterminated_session.stdout)
        self.assertIn("boot_executable", unterminated_session.stdout)

    def test_native_launch_plan(self) -> None:
        """Native launch resolution is identity-bound to the selected title.

        Failing-before contract: this fixture used to PROVE the defect --
        a stale build/<retail>/<retail> runtime satisfied an unrelated
        selected title (display-smoke-v1) and the session paired that binary
        with another title's session data. The stale artifact now must be
        irrelevant, and only the selected title's own build may resolve.
        """
        if not (ROOT / "src" / "core" / "nk_launch.c").is_file():
            self.skipTest("nk_launch.c not present in this slice")
        mock_root = self.temp_dir / "mock_repo"
        stale_dir = mock_root / "build" / "hst"
        stale_dir.mkdir(parents=True, exist_ok=True)
        exe_name = "hst.exe" if sys.platform == "win32" else "hst"
        stale_exe = stale_dir / exe_name
        stale_exe.write_bytes(b"MZfake")
        (stale_dir / "hst_image.bin").write_bytes(b"image")

        mock_iso = self.temp_dir / "game.iso"
        create_test_iso(mock_iso)

        # The native harness resolves application data from the environment, so
        # every launch below runs with it pointed at this test's temp directory.
        # Built once and passed to both invocations: the refusal case further
        # down must not escape the isolation either.
        env = {
            **os.environ,
            "LOCALAPPDATA": str(self.temp_dir),
            "APPDATA": str(self.temp_dir),
            "USERPROFILE": str(self.temp_dir),
            "HOME": str(self.temp_dir),
        }

        # A selected title the catalog describes must NOT resolve through the
        # stale retail-shaped build: only its own game_name layout counts, and
        # an absent own runtime is an honest missing-runtime error.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("Runtime binary not found", res.stdout)
        self.assertNotIn("hst.exe", res.stdout)

        # With the selected title's own runtime present, the session resolves
        # it -- still ignoring the stale sibling -- and takes its addresses
        # from the catalog rather than from a constant.
        own_dir = mock_root / "build" / "display-smoke"
        own_dir.mkdir(parents=True, exist_ok=True)
        own_exe = own_dir / ("display-smoke.exe" if sys.platform == "win32" else "display-smoke")
        own_exe.write_bytes(b"MZfake")
        (own_dir / "display-smoke_image.bin").write_bytes(b"image")

        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        missing_data = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(missing_data.returncode, 0, missing_data.stderr)
        self.assertIn("LAUNCH_PREPARE_ERROR", missing_data.stdout)
        self.assertIn("data folder", missing_data.stdout.lower())
        self.assertNotIn("LAUNCH_PREPARE_OK", missing_data.stdout)

        (mock_root / "fixtures" / "display_smoke").mkdir(parents=True)

        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn(Path(own_exe).name, res.stdout)
        self.assertNotIn("hst.exe", res.stdout)
        self.assertIn(Path(mock_iso).name, res.stdout)
        self.assertIn("BASE:0x08810000", res.stdout)
        self.assertIn("ENTRY:0x08810000", res.stdout)

        # A title it does not describe is refused. This used to "succeed" by
        # launching the guest at a hard-coded 0x0029a060 with a zero base: an
        # address belonging to no public title, so the runtime was started at a
        # constant rather than at anything the catalog knew. A public-safe tree
        # has no addresses for a retail identity and must say so.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "UCUS98701", "hst-ucus98701-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("No catalog entry", res.stdout)

    def test_duplicate_sfo_keys_parity(self) -> None:
        """Verify identical duplicate SFO keys accepted and conflicting rejected by both Python and C."""
        from nk_core.iso_inspect import IsoInspectionError

        # 1. Conflicting keys: Python and Native C must reject
        sfo_conflict = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"ULUS10001\0"),
            ("TITLE", 0x0204, b"Test Conflict\0"),
        ])
        iso_conflict = self.temp_dir / "conflict.iso"
        create_custom_sfo_iso(iso_conflict, sfo_conflict)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_conflict)

        c_conflict = self._run_native_inspect(iso_conflict)
        self.assertTrue(c_conflict.get("RESULT", "").startswith("ERROR"))
        valid_iso = self.temp_dir / "preflight-valid.iso"
        create_test_iso(valid_iso)
        preflight_meta = inspect_iso(valid_iso)
        preflight = inspect_compatibility_preflight(
            iso_conflict, metadata=preflight_meta, runtime_root=self.temp_dir
        )
        disc_check = next(check for check in preflight["checks"]
                          if check["code"] == "DISC_SFO")
        self.assertEqual(disc_check["status"], "UNSUPPORTED")

        # 2. Byte-identical keys: Python and Native C must accept
        sfo_identical = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Test Identical\0"),
        ])
        iso_identical = self.temp_dir / "identical.iso"
        create_custom_sfo_iso(iso_identical, sfo_identical)

        py_identical = inspect_iso(iso_identical)
        self.assertEqual(py_identical.disc_id, "UCUS98701")

        c_identical = self._run_native_inspect(iso_identical)
        self.assertEqual(c_identical.get("RESULT"), "OK")
        self.assertEqual(c_identical.get("DISC_ID"), "UCUS98701")

    def test_identity_parameter_format_parity(self) -> None:
        """An identity field in a non-string format is refused by both parsers.

        The native reader used to ignore the parameter format at entry offset
        +2 and decode the bytes as text regardless, while the Python inspector
        yielded the decoded integer. A disc whose DISC_ID declared an integer
        format could therefore be matched to a catalog title by one parser and
        not the other -- the differential contract these two are meant to keep.
        """
        from nk_core.iso_inspect import IsoInspectionError

        # DISC_ID declared as uint32 (0x0404) while carrying string bytes.
        sfo_int_id = build_custom_param_sfo([
            ("DISC_ID", 0x0404, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Wrong Format\0"),
        ])
        iso_int_id = self.temp_dir / "int_disc_id.iso"
        create_custom_sfo_iso(iso_int_id, sfo_int_id)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_int_id)

        c_int_id = self._run_native_inspect(iso_int_id)
        self.assertTrue(c_int_id.get("RESULT", "").startswith("ERROR"))

        # An unrecognised format is refused the same way, not silently ignored.
        sfo_unknown = build_custom_param_sfo([
            ("DISC_ID", 0x0101, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Unknown Format\0"),
        ])
        iso_unknown = self.temp_dir / "unknown_fmt.iso"
        create_custom_sfo_iso(iso_unknown, sfo_unknown)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_unknown)

        c_unknown = self._run_native_inspect(iso_unknown)
        self.assertTrue(c_unknown.get("RESULT", "").startswith("ERROR"))

        # A non-identity key in a non-string format is NOT a reason to reject:
        # only identity decides whether a disc matches a catalog title.
        sfo_other = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Fine\0"),
            ("PARENTAL_LEVEL", 0x0404, bytes([1, 0, 0, 0])),
        ])
        iso_other = self.temp_dir / "other_int_key.iso"
        create_custom_sfo_iso(iso_other, sfo_other)

        py_other = inspect_iso(iso_other)
        self.assertEqual(py_other.disc_id, "UCUS98701")

        c_other = self._run_native_inspect(iso_other)
        self.assertEqual(c_other.get("RESULT"), "OK")
        self.assertEqual(c_other.get("DISC_ID"), "UCUS98701")

    def test_reader_parity_inspect_and_lookup(self) -> None:
        iso_path = self.temp_dir / "reader_test.iso"
        create_test_iso(iso_path, disc_id="UCUS98701", title="Reader Test", volume_id="READER_VOL")

        # Python inspect
        py_meta = inspect_iso(iso_path)
        self.assertEqual(py_meta.volume_id.rstrip("\x00 "), "READER_VOL")

        # Native reader test
        cmd = [str(self.exe_path), "reader_test", str(iso_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        lines = dict(line.split(":", 1) for line in res.stdout.strip().splitlines() if ":" in line)
        self.assertEqual(lines.get("READER"), "OPEN_OK")
        self.assertEqual(lines.get("VOLUME_ID"), "READER_VOL")
        self.assertEqual(lines.get("FILE_SIZE"), str(1024 * 2048))

        sfo_parts = lines.get("LOOKUP_SFO", "").split(":")
        self.assertEqual(sfo_parts[0], "0")
        self.assertEqual(sfo_parts[1], "32")
        self.assertEqual(sfo_parts[3], "0")

        dir_parts = lines.get("LOOKUP_DIR", "").split(":")
        self.assertEqual(dir_parts[0], "0")
        self.assertEqual(dir_parts[1], "34")
        self.assertEqual(dir_parts[3], "1")

        self.assertEqual(lines.get("LOOKUP_MISS"), "-1")

        list_parts = lines.get("LIST_ROOT_0", "").split(":")
        self.assertEqual(list_parts[0], "1")
        self.assertEqual(list_parts[1], "PSP_GAME")
        self.assertEqual(list_parts[2], "34")
        self.assertEqual(list_parts[4], "1")

        self.assertEqual(lines.get("LIST_ROOT_1"), "0")
        self.assertGreater(int(lines.get("READ_SFO", "0")), 0)
        self.assertEqual(lines.get("READ_EOF"), "0")

    def test_reader_invalid_iso_fails_open(self) -> None:
        bad_iso = self.temp_dir / "bad.iso"
        bad_iso.write_bytes(b"garbage data not an iso" * 100)
        cmd = [str(self.exe_path), "reader_test", str(bad_iso)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertIn("READER:OPEN_FAILED", res.stdout)


class PackageBuildFailureMessageTests(unittest.TestCase):
    """The generic compile/link failure branch names the first real diagnostic line."""

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_build_fail_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _build_with_failing_compile(self, disc_id: str, stderr: str, returncode: int,
                                    with_log: bool, stdout: str = "") -> tuple[int, str, Path]:
        user_root = self.temp_dir / f"user-{disc_id}"
        user_root.mkdir(parents=True, exist_ok=True)
        iso_file = self.temp_dir / f"{disc_id}.iso"
        create_test_iso_with_executables(iso_file, build_psp_container(), disc_id=disc_id,
                                         title="Build Fail Title")
        title_id = "synthetic-title2-v1"
        entry = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry]}), encoding="utf-8")
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True, exist_ok=True)
        (decrypted_dir / "EBOOT.elf").write_bytes(build_plain_mips_elf())
        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        progress = self.temp_dir / f"{disc_id}-progress.jsonl"
        log_file = self.temp_dir / f"{disc_id}-build.log"
        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
            "progress_json": progress,
            "log_file": log_file if with_log else None,
        })()

        def fake_compile(command, **_kwargs):
            return subprocess.CompletedProcess(list(command), returncode, stdout=stdout, stderr=stderr)

        with patch.object(nk_cli, "_load_entry_manifest", return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli, "_has_private_backends", return_value=False), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_compile), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             patch.object(nk_cli.package_cache, "validate_package_cache", return_value=(True, "")):
            rc = nk_cli.cmd_build_package(args)

        events = [json.loads(line) for line in progress.read_text(encoding="utf-8").splitlines()]
        compile_failures = [e for e in events if e["stage"] == "compile" and e["status"] == "FAIL"]
        self.assertEqual(len(compile_failures), 1)
        return rc, compile_failures[0]["message"], log_file

    def test_linker_failure_names_first_diagnostic_and_exit_code(self) -> None:
        stderr = (
            "make: *** [Makefile:12: atrac3p.o] Error 1\n"
            "ld.exe: cannot find D:/games/packages/.build-package-xyz/atrac3p: No such file or directory\n"
            "collect2.exe: error: ld returned 1 exit status\n"
            "make: *** [Makefile:88: all] Error 1\n"
        )
        rc, message, log_file = self._build_with_failing_compile(
            "TEST00101", stderr, returncode=2, with_log=True)
        self.assertEqual(rc, 2)
        self.assertEqual(
            message,
            "PACKAGE_BUILD_FAILED: ld.exe: cannot find D:/games/packages/.build-package-xyz/atrac3p: "
            f"No such file or directory (exit code 2; full log: {log_file})",
        )

    def test_diagnostic_on_stdout_without_log_file(self) -> None:
        stdout = (
            "gcc -c src/rt/hle.c -o build/hle.o\n"
            "src/rt/hle.c:12:5: error: 'missing' undeclared (first use in this function)\n"
        )
        rc, message, _log_file = self._build_with_failing_compile(
            "TEST00103", "make: *** [Makefile:88: all] Error 2\n", returncode=2,
            with_log=False, stdout=stdout)
        self.assertEqual(rc, 2)
        self.assertEqual(
            message,
            "PACKAGE_BUILD_FAILED: src/rt/hle.c:12:5: error: 'missing' undeclared "
            "(first use in this function) (exit code 2)",
        )

    def test_diagnostic_markers_skip_make_noise_and_warnings(self) -> None:
        first = nk_cli._first_build_diagnostic
        self.assertEqual(
            first("a.c:1:1: warning: unused variable 'x'\n"
                  "ld: game.o: in function `main': undefined reference to `missing_fn'\n"),
            "ld: game.o: in function `main': undefined reference to `missing_fn'",
        )
        self.assertEqual(first("make: *** [Makefile:1: x] error: 1\nb.c:2:3: error: bad\n"),
                         "b.c:2:3: error: bad")
        self.assertIsNone(first("make: *** [Makefile:1: all] Error 2\nwarning: nothing\n"))
        self.assertEqual(len(first("x.c:1:1: error: " + "y" * 1000)), 300)

    def test_fallback_names_log_file_when_one_was_requested(self) -> None:
        rc, message, log_file = self._build_with_failing_compile(
            "TEST00104", "make: *** [Makefile:88: all] Error 4\n", returncode=4, with_log=True)
        self.assertEqual(rc, 4)
        self.assertEqual(
            message, f"PACKAGE_BUILD_FAILED: build exited with code 4 (full log: {log_file})")

    def test_failure_without_diagnostic_uses_fallback(self) -> None:
        stderr = "make: *** [Makefile:88: all] Error 3\n"
        rc, message, _log_file = self._build_with_failing_compile(
            "TEST00102", stderr, returncode=3, with_log=False)
        self.assertEqual(rc, 3)
        self.assertEqual(message, "PACKAGE_BUILD_FAILED: build exited with code 3")

class GuestModuleElfRuleTests(unittest.TestCase):
    """Issue #729: a PSP PRX guest module is usable by its real contract (an
    executable PT_LOAD with code bytes), not by an executable e_entry.  The
    module checklist and package extraction apply the same rule, while the
    executable rule keeps its e_entry requirement."""

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_prx_rule_"))
        self.addCleanup(shutil.rmtree, self.temp_dir, True)

    def _write(self, name: str, data: bytes) -> Path:
        path = self.temp_dir / name
        path.write_bytes(data)
        return path

    def test_prx_with_unset_entry_and_code_segment_is_usable_module(self) -> None:
        prx = self._write(
            "libfont.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(prx, module=True), "PLAIN_MIPS_ELF32")

    def test_executables_with_entry_in_code_segment_stay_usable(self) -> None:
        for e_type in (2, 3):
            with self.subTest(e_type=e_type):
                path = self._write(f"plain_{e_type}.elf", build_plain_mips_elf(e_type))
                self.assertEqual(_classify_decrypted_elf_file(path), "PLAIN_MIPS_ELF32")
                self.assertEqual(
                    _classify_decrypted_elf_file(path, module=True), "PLAIN_MIPS_ELF32"
                )

    def test_prx_without_load_segment_is_rejected(self) -> None:
        path = self._write(
            "noload.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, p_type=4)
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_prx_whose_only_load_segment_is_not_executable_is_rejected(self) -> None:
        path = self._write(
            "data.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, p_flags=4)
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_prx_code_segment_without_file_bytes_is_rejected(self) -> None:
        path = self._write(
            "bss.prx",
            build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, filesz=0),
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_truncated_prx_is_rejected(self) -> None:
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        for length in (40, 60):
            with self.subTest(length=length):
                path = self._write(f"truncated_{length}.prx", prx[:length])
                self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_unset_entry_stays_rejected_for_executables(self) -> None:
        executable = self._write(
            "unset.elf", build_plain_mips_elf(2, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(executable), "UNKNOWN")
        self.assertEqual(_classify_decrypted_elf_file(executable, module=True), "UNKNOWN")
        # A PRX-format main image still needs its e_entry in a code segment:
        # the launcher starts at e_entry, so the module rule does not apply.
        main_prx = self._write(
            "main_prx.elf", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(main_prx), "UNKNOWN")

    def test_checklist_and_extraction_agree_on_prx_modules(self) -> None:
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        truncated = prx[:60]
        iso = self.temp_dir / "guest-modules.iso"
        create_test_iso_with_modules(
            iso, build_plain_mips_elf(),
            sysdir_modules={"libfont.prx": prx, "truncated.prx": truncated},
            usrdir_modules={},
        )
        user_root = self.temp_dir / "user-data"

        def spec(name: str) -> dict:
            return {"names": [name], "members": [("PSP_GAME", "SYSDIR", name)]}

        # Checklist, disc copies: the valid PRX is ready, the truncated one is not.
        checklist = decrypt_needed_modules(
            iso, user_data_root=user_root, disc_id="TEST00001",
            modules=[spec("libfont.prx"), spec("truncated.prx")],
        )
        status = {result["name"]: result["status"] for result in checklist["results"]}
        self.assertEqual(status["libfont.prx"], "skipped")
        self.assertNotEqual(status["truncated.prx"], "skipped")

        # Extraction, disc copies: the same two modules, the same verdicts.
        for name, expect_ok in (("libfont.prx", True), ("truncated.prx", False)):
            with self.subTest(source="disc", module=name):
                manifest = {"modules": [{"name": name, "role": "guest-prx", "required": True}]}
                cache = self.temp_dir / f"disc-cache-{name}"
                if expect_ok:
                    output = nk_cli._copy_optional_modules(iso, manifest, cache, None)
                    self.assertEqual((output / name).read_bytes(), prx)
                else:
                    with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a usable plain MIPS ELF32"):
                        nk_cli._copy_optional_modules(iso, manifest, cache, None)
                    self.assertFalse((cache / "modules" / name).exists())

        # Checklist and extraction, user-supplied decrypted copies.
        decrypted = decrypted_module_dir(user_root, "TEST00001")
        decrypted.mkdir(parents=True)
        for name, data, expect_ok in (("libfont.prx", prx, True),
                                      ("truncated.prx", truncated, False)):
            with self.subTest(source="user", module=name):
                (decrypted / name).write_bytes(data)
                checklist = decrypt_needed_modules(
                    iso, user_data_root=user_root, disc_id="TEST00001",
                    modules=[spec(name)],
                )
                result = checklist["results"][0]
                manifest = {"modules": [{"name": name, "role": "guest-prx", "required": True}]}
                cache = self.temp_dir / f"user-cache-{name}"
                if expect_ok:
                    self.assertEqual((result["status"], result["reason"]), ("skipped", "user-supplied"))
                    output = nk_cli._copy_optional_modules(
                        iso, manifest, cache, decrypted
                    )
                    self.assertEqual((output / name).read_bytes(), data)
                else:
                    self.assertEqual(result["status"], "failed")
                    self.assertEqual(result["reason"], "user-supplied-invalid")
                    with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a usable plain MIPS ELF32"):
                        nk_cli._copy_optional_modules(iso, manifest, cache, decrypted)
                    self.assertFalse((cache / "modules" / name).exists())


# nk_cli.py writes titles as UTF-8 whatever the host pipe encoding is (#732). On Windows a
# redirected Python stdout uses the ANSI code page (cp1252), so trademark and registered
# signs became single bytes and a UTF-8 reader saw U+FFFD. The child is forced to a cp1252
# pipe so the failure reproduces on any host.
CONSOLE_ENCODING_TITLE = "Café® ™"


class NkCliConsoleEncodingTests(unittest.TestCase):
    def run_inspect(self, iso: Path, pipe_encoding: str) -> bytes:
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = pipe_encoding
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect", str(iso)],
            cwd=str(ROOT), env=env, capture_output=True, timeout=120,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
        return completed.stdout

    def test_piped_title_with_trademark_and_registered_signs_is_utf8(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_encoding_") as temp:
            iso = Path(temp) / "sign-title.iso"
            create_test_iso(iso, disc_id="TEST00002", title=CONSOLE_ENCODING_TITLE, volume_id="TEST00001")
            # A legacy ANSI pipe is what a Windows parent process gets by default.
            raw = self.run_inspect(iso, "cp1252")

        text = raw.decode("utf-8")  # strict: a stray cp1252 byte must fail here
        self.assertNotIn("�", text)
        title_line = next(line for line in text.splitlines() if line.startswith("Title:"))
        self.assertEqual(title_line, "Title:      " + CONSOLE_ENCODING_TITLE)

    def test_output_written_to_a_file_is_utf8_too(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_encoding_file_") as temp:
            temp_root = Path(temp)
            iso = temp_root / "sign-title.iso"
            create_test_iso(iso, disc_id="TEST00002", title=CONSOLE_ENCODING_TITLE, volume_id="TEST00001")
            out_path = temp_root / "inspect.txt"
            env = os.environ.copy()
            env["PYTHONIOENCODING"] = "cp1252"
            with out_path.open("wb") as out:
                completed = subprocess.run(
                    [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect", str(iso)],
                    cwd=str(ROOT), env=env, stdout=out, stderr=subprocess.PIPE, timeout=120,
                )
            self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
            text = out_path.read_bytes().decode("utf-8")
        self.assertIn("Title:      " + CONSOLE_ENCODING_TITLE, text)


if __name__ == "__main__":
    unittest.main()
