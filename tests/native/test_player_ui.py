# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Headless SDL regressions for the native player's real event/render loop."""

from __future__ import annotations

import atexit
import os
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from typing import Sequence
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from nk_core import package_cache  # noqa: E402  (the package-layer epochs the fixture must spell)
from test_player_package_route import (  # noqa: E402
    build_plain_mips_elf,
    build_psp_container,
    create_test_iso_with_executables,
)
PLAYER_EXE = Path(
    os.environ.get("NAKAGAWA_PLAYER_UI_TEST_EXE", "build/nakagawa_player_ui_test.exe")
)
if not PLAYER_EXE.is_absolute():
    PLAYER_EXE = ROOT / PLAYER_EXE

# The UI tests must not discover showcase packages from ``build/demos``.  Keep
# the test executable in a short-lived sibling directory with its runtime DLLs,
# so its executable directory has no demos folder even after ``make showcase``.
_PLAYER_EXE_BUILD_PATH = PLAYER_EXE
_PLAYER_EXE_SANDBOX = None
if _PLAYER_EXE_BUILD_PATH.is_file():
    _PLAYER_EXE_SANDBOX = tempfile.TemporaryDirectory(
        prefix=".player-ui-regressions-", dir=_PLAYER_EXE_BUILD_PATH.parent,
        ignore_cleanup_errors=True,
    )
    # Remove the sandbox copy when the test process exits; a player still holding
    # the executable open on Windows must not turn cleanup into a test failure.
    atexit.register(_PLAYER_EXE_SANDBOX.cleanup)
    PLAYER_EXE = Path(_PLAYER_EXE_SANDBOX.name) / _PLAYER_EXE_BUILD_PATH.name
    shutil.copy2(_PLAYER_EXE_BUILD_PATH, PLAYER_EXE)
    for runtime_dll in _PLAYER_EXE_BUILD_PATH.parent.glob("*.dll"):
        shutil.copy2(runtime_dll, PLAYER_EXE.parent / runtime_dll.name)
    if (PLAYER_EXE.parent / "demos").exists():
        raise RuntimeError("player UI regression executable sandbox contains demos/")

# Starts argv[1] (with the rest of argv) with no console and null standard handles, as
# Explorer starts the GUI player, and exits with its exit code.
CONSOLE_LESS_LAUNCHER = """
import ctypes, subprocess, sys
from ctypes import wintypes
class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR)] + [
                (name, wintypes.DWORD) for name in ("dwX", "dwY", "dwXSize", "dwYSize",
                "dwXCountChars", "dwYCountChars", "dwFillAttribute", "dwFlags")] + [
                ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]
class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p,
    ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
    ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)]
info = STARTUPINFOW()
info.cb = ctypes.sizeof(info)
info.dwFlags = 0x00000100  # STARTF_USESTDHANDLES with every handle left null
process = PROCESS_INFORMATION()
command = ctypes.create_unicode_buffer(subprocess.list2cmdline(sys.argv[1:]))
if not kernel32.CreateProcessW(None, command, None, None, False, 0x00000008,  # DETACHED_PROCESS
                               None, None, ctypes.byref(info), ctypes.byref(process)):
    sys.exit(100)
kernel32.WaitForSingleObject(process.hProcess, 60000)
code = wintypes.DWORD()
kernel32.GetExitCodeProcess(process.hProcess, ctypes.byref(code))
kernel32.CloseHandle(process.hThread)
kernel32.CloseHandle(process.hProcess)
sys.exit(code.value)
"""

# NAKAGAWA_PLAYER_UI_SHARD=<k>/<n> runs only the tests whose position in test-id order
# is k modulo n. The Makefile's player-ui-regressions runs all n shards side by side,
# so together they run every test exactly once; unset, every test runs here.
PLAYER_UI_SHARD_ENV = "NAKAGAWA_PLAYER_UI_SHARD"


def parse_shard(spec: str) -> tuple[int, int]:
    match = re.fullmatch(r"(0|[1-9][0-9]{0,3})/([1-9][0-9]{0,3})", spec)
    if match is None or int(match.group(1)) >= int(match.group(2)):
        raise ValueError(f"{PLAYER_UI_SHARD_ENV} must be k/n with 0 <= k < n, not {spec!r}")
    return int(match.group(1)), int(match.group(2))


def _flatten(suite: unittest.TestSuite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from _flatten(test)
        else:
            yield test


def select_shard(tests: unittest.TestSuite, spec: str) -> unittest.TestSuite:
    """The tests of shard k/n in test-id order. A shard that admits nothing is refused."""
    shard, count = parse_shard(spec)
    ordered = sorted(_flatten(tests), key=lambda test: test.id())
    selected = [test for index, test in enumerate(ordered) if index % count == shard]
    if not selected:
        raise ValueError(f"{PLAYER_UI_SHARD_ENV}={spec} admits none of {len(ordered)} tests")
    return unittest.TestSuite(selected)


def load_tests(loader: unittest.TestLoader, standard_tests: unittest.TestSuite,
               pattern: str | None) -> unittest.TestSuite:
    spec = os.environ.get(PLAYER_UI_SHARD_ENV, "").strip()
    return select_shard(standard_tests, spec) if spec else standard_tests


# The player accepts a package only for the CpuState ABI it was compiled with, so a
# synthetic ready package must name that same version (src/rt/recomp.h).
CPU_STATE_ABI_VERSION = int(
    re.search(
        r"#define SR_CPUSTATE_ABI_VERSION (\d+)u",
        (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8"),
    ).group(1)
)


def read_bmp(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError("SDL did not save a Windows BMP frame")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise AssertionError(f"unsupported BMP DIB header size {dib_size}")
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if (
        width <= 0
        or signed_height == 0
        or bits_per_pixel not in (24, 32)
        or compression not in (0, 3)
        or (compression == 3 and bits_per_pixel != 32)
    ):
        raise AssertionError("SDL BMP has an unsupported pixel layout")
    if compression == 3:
        red_mask, green_mask, blue_mask, _alpha_mask = struct.unpack_from("<IIII", data, 54)
        if (red_mask, green_mask, blue_mask) != (0x00FF0000, 0x0000FF00, 0x000000FF):
            raise AssertionError("SDL BMP uses unexpected channel masks")
    return width, abs(signed_height), bits_per_pixel, data[pixel_offset:]


def pixel_rgb(
    bmp: tuple[int, int, int, bytes], x: int, y: int, top_down: bool = False
) -> tuple[int, int, int]:
    width, height, bpp, pixels = bmp
    bytes_per_pixel = bpp // 8
    stride = (width * bytes_per_pixel + 3) & ~3
    row = y if top_down else height - y - 1
    offset = row * stride + x * bytes_per_pixel
    blue, green, red = pixels[offset : offset + 3]
    return red, green, blue


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def png_pixel(red: int, green: int, blue: int) -> bytes:
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(b"\x00" + bytes((red, green, blue, 255))))
        + png_chunk(b"IEND", b"")
    )


def undecodable_png() -> bytes:
    """PNG-shaped bytes the header reader accepts but the decoder rejects.

    The signature, IHDR and IEND are well formed, so the image is an ordinary
    ISO entry that reads back intact; the IDAT carries bytes that are not a
    zlib stream, so SDL_LoadPNG_IO rejects it. This exercises decoder failure
    separately from missing ISO data and from a build with no PNG decoder.
    """
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
    return (
        b"\x89PNG\r\n\x1a\n"
        + ihdr
        + png_chunk(b"IDAT", b"not a zlib stream")
        + png_chunk(b"IEND", b"")
    )


def corrupt_png_header() -> bytes:
    """Bytes that the ISO reader rejects before a decoder is involved."""
    return b"not a PNG image"


def synthetic_art_iso(
    path: Path, *, include_picture: bool = True, icon_bytes: bytes | None = None,
    source_executable: bytes | None = None,
) -> None:
    sector_size = 2048
    icon_image = png_pixel(50, 190, 120) if icon_bytes is None else icon_bytes
    assert len(icon_image) <= sector_size, "the synthetic icon must fit one sector"
    image = bytearray((23 if source_executable is not None else 22) * sector_size)
    if source_executable is not None:
        assert len(source_executable) <= sector_size, "the synthetic executable must fit one sector"

    def both_endian(value: int) -> bytes:
        return struct.pack("<I", value) + struct.pack(">I", value)

    def record(name: bytes, lba: int, size: int, directory: bool) -> bytes:
        record_size = 33 + len(name)
        if record_size & 1:
            record_size += 1
        result = bytearray(record_size)
        result[0] = record_size
        result[2:10] = both_endian(lba)
        result[10:18] = both_endian(size)
        result[25] = 2 if directory else 0
        result[28] = 1
        result[31] = 1
        result[32] = len(name)
        result[33 : 33 + len(name)] = name
        return bytes(result)

    pvd = memoryview(image)[16 * sector_size : 17 * sector_size]
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[158:166] = both_endian(17)
    pvd[166:174] = both_endian(sector_size)
    pvd[156:190] = record(b"\x00", 17, sector_size, True)

    root_records = (
        record(b"\x00", 17, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"PSP_GAME", 18, sector_size, True)
    )
    game_records = (
        record(b"\x00", 18, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"ICON0.PNG;1", 19, len(icon_image), False)
    )
    picture_size = 0
    if include_picture:
        game_records += record(b"PIC1.PNG;1", 20, len(png_pixel(50, 100, 190)), False)
        picture_size = len(png_pixel(50, 100, 190))
    if source_executable is not None:
        game_records += record(b"SYSDIR", 21, sector_size, True)
    image[17 * sector_size : 17 * sector_size + len(root_records)] = root_records
    image[18 * sector_size : 18 * sector_size + len(game_records)] = game_records
    image[19 * sector_size : 19 * sector_size + len(icon_image)] = icon_image
    if picture_size:
        image[20 * sector_size : 20 * sector_size + picture_size] = png_pixel(50, 100, 190)
    if source_executable is not None:
        sysdir_records = (
            record(b"\x00", 21, sector_size, True)
            + record(b"\x01", 18, sector_size, True)
            + record(b"EBOOT.BIN;1", 22, len(source_executable), False)
        )
        image[21 * sector_size : 21 * sector_size + len(sysdir_records)] = sysdir_records
        image[22 * sector_size : 22 * sector_size + len(source_executable)] = source_executable
    path.write_bytes(image)


def synthetic_pbp_package(path: Path, *, disc_id: str = "TEST00424",
                          title: str = "Store Package") -> None:
    """Write a synthetic PlayStation Store package named like a disc image.

    The package carries "\0PBP" at offset 0 and a PARAM.SFO in its first
    section. Nothing in it is executed or extracted; it exists only so the
    player has a file whose extension lies about its format.
    """
    entries = sorted([
        ("DISC_ID", disc_id.encode("utf-8") + b"\0"),
        ("TITLE", title.encode("utf-8") + b"\0"),
    ])
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()
    for key, value in entries:
        key_offset = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")
        data_offset = len(data_table)
        data_table.extend(value)
        while len(data_table) % 4:
            data_table.append(0)
        entry_table.extend(
            struct.pack("<HHIII", key_offset, 0x0204, len(value), len(value), data_offset)
        )
    key_table_start = 20 + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4:
        key_table.append(0)
        data_table_start += 1
    sfo = bytes(
        struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00",
                    key_table_start, data_table_start, len(entries))
    ) + bytes(entry_table) + bytes(key_table) + bytes(data_table)

    header = bytearray(40)
    header[:4] = b"\x00PBP"
    struct.pack_into("<I", header, 4, 0x00010000)
    offsets = [40] + [40 + len(sfo)] * 7
    for index, value in enumerate(offsets):
        struct.pack_into("<I", header, 8 + index * 4, value)
    path.write_bytes(bytes(header) + sfo)


def synthetic_stale_package(runtime_root: Path) -> None:
    package = runtime_root / "packages" / "TEST00006"
    package.mkdir(parents=True)
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 1,
        "title": {
            "id": "stale-synthetic-title",
            "display_name": "Synthetic stale package",
            "kind": "retail",
            "manifest_sha256": "0" * 64,
            "protected_digest": "0" * 64,
        },
        "inputs": {},
        "runtime": {},
        "executable": {"path": "stale-synthetic.exe"},
        "cache": {},
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }
    (package / "package.json").write_text(json.dumps(package_json), encoding="utf-8")
    (package / "build-report.json").write_text("{}", encoding="utf-8")
    (package / "stale-synthetic.exe").write_bytes(b"synthetic stale executable")


def break_package_manifest(runtime_root: Path) -> None:
    """Make a built package unreadable, as an upgrade would: the validator calls it incompatible."""
    package = runtime_root / "packages" / "TEST00006" / "package.json"
    assert package.is_file()
    package.write_text("{ not a package manifest", encoding="utf-8")


def synthetic_ready_package(
    runtime_root: Path,
    source_executable: bytes,
    *,
    data_root_present: bool = True,
    disc_id: str = "TEST00006",
    title_id: str = "display-smoke-v1",
) -> None:
    """Write a source-owned package accepted by the real native validator."""
    package = runtime_root / "packages" / disc_id
    package.mkdir(parents=True)
    executable_name = f"{title_id}.exe"
    image_name = f"{title_id}_image.bin"
    executable_bytes = b"synthetic ready package executable\n"
    input_executable_hash = hashlib.sha256(source_executable).hexdigest()
    executable_hash = hashlib.sha256(executable_bytes).hexdigest()
    zero_hash = "0" * 64

    def write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value, separators=(",", ":")) + "\n", encoding="utf-8")

    def canonical_hash(value: object) -> str:
        canonical = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    identity = {
        "container": None,
        "disc": {"disc_version": None, "id": disc_id, "region": "TEST"},
        "format": "nakagawa-title-input-identity",
        "main_executable": {"name": "EBOOT.BIN", "sha256": input_executable_hash},
        "manifest": {"id": title_id, "schema_version": 1},
        "modules": [],
        "param_sfo": None,
        "psp_header": None,
        "schema_version": 2,
        "source_media": {
            "executable": {
                "path": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "sha256": input_executable_hash,
            },
            "modules": [],
        },
    }
    identity_text = json.dumps(identity, separators=(",", ":")) + "\n"
    identity_digest = canonical_hash(identity)
    identity_path = runtime_root / "title-input-identities" / disc_id / "title-input-identity.json"
    identity_path.parent.mkdir(parents=True)
    identity_path.write_text(identity_text, encoding="utf-8")

    aot_components = {
        "analyzer_codegen_epoch": "analyzer-codegen-v1",
        "analyzer_sha256": zero_hash,
        "codegen_options_sha256": canonical_hash({}),
        "codegen_sha256": zero_hash,
        "executable_sha256": input_executable_hash,
        "generated_code_abi_epoch": package_cache.GENERATED_CODE_ABI_EPOCH,
        "manifest_sha256": zero_hash,
        "modules_sha256": canonical_hash([]),
        "psp_header_sha256": None,
        "runtime_abi_epoch": package_cache.RUNTIME_ABI_EPOCH,
        "title_input_identity_sha256": identity_digest,
    }
    native_components = {
        "compile_flags": "",
        "compiler_identity": "gcc-fixture",
        "compiler_target": "fixture-target",
        "generated_code_digest": zero_hash,
        "link_flags": "",
        "runtime_abi_epoch": package_cache.RUNTIME_ABI_EPOCH,
        "runtime_source_digest": zero_hash,
    }
    cache_key = {
        "schema_version": 2,
        "aot": {"digest": canonical_hash(aot_components), "components": aot_components},
        "native": {"digest": canonical_hash(native_components), "components": native_components},
    }
    cache = {
        "format": "nakagawa-aot-cache",
        "schema_version": 2,
        "key": cache_key,
        "codegen_options": {},
        "runtime_abi_compatibility": {
            "current_epoch": package_cache.RUNTIME_ABI_EPOCH, "generated_code_reusable": True,
        },
    }
    inputs = {
        "manifest": {"sha256": zero_hash},
        "executable": {"sha256": input_executable_hash},
        "modules": [],
        "psp_header": None,
    }
    report = {
        "format": "nakagawa-build-report",
        "schema_version": 1,
        "title_id": title_id,
        "runtime_abi": {"name": "CpuState", "version": CPU_STATE_ABI_VERSION},
        "cache": cache,
        "input_hashes": inputs,
        "tools": {},
        "coverage": {},
        "unsupported": {"imports": [], "instructions": [], "regions": []},
        "analysis_diagnostics": [],
        "artifacts": {},
    }
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 2,
        "cache": cache,
        "title": {
            "id": title_id,
            "display_name": "Synthetic fixture",
            "kind": "retail",
            "manifest_sha256": zero_hash,
            "protected_digest": zero_hash,
        },
        "inputs": inputs,
        "title_input_identity": identity,
        "runtime": {
            "abi": "CpuState",
            "abi_version": CPU_STATE_ABI_VERSION,
            "abi_header_sha256": zero_hash,
            "run_entry": "0x00000000",
            "runtime_contract": None,
            "runtime_bindings": {},
            "required_runtime_bindings": [],
        },
        "executable": {
            "path": executable_name,
            "sha256": executable_hash,
            "guest_entry": "0x00000000",
        },
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }

    executable = package / executable_name
    image = package / image_name
    executable.write_bytes(executable_bytes)
    image.write_bytes(b"synthetic runtime image\n")
    write_json(package / "build-report.json", report)
    write_json(package / "package.json", package_json)

    completion = {
        "format": "nakagawa-aot-cache-completion",
        "schema_version": 2,
        "status": "complete",
        "cache_key": cache_key,
        "title_input_identity": identity,
        "artifacts": [
            {"path": "package.json", "sha256": hashlib.sha256((package / "package.json").read_bytes()).hexdigest()},
            {"path": "build-report.json", "sha256": hashlib.sha256((package / "build-report.json").read_bytes()).hexdigest()},
            {"path": executable_name, "sha256": hashlib.sha256(executable.read_bytes()).hexdigest()},
            {"path": image_name, "sha256": hashlib.sha256(image.read_bytes()).hexdigest()},
        ],
    }
    write_json(package / "completion-manifest.json", completion)
    if data_root_present:
        (runtime_root / "fixtures" / "display_smoke").mkdir(parents=True, exist_ok=True)


def synthetic_experimental_profile(
    runtime_root: Path, disc_id: str, source_executable: bytes | None
) -> None:
    """Write a synthetic experimental profile for an imported or built title.

    ``None`` is the valid imported state when EBOOT.BIN was encrypted and no
    executable has been selected. Bytes model a profile after a passing build.
    """
    title_id = f"experimental-{disc_id.lower()}"
    executable_hash = (
        hashlib.sha256(source_executable).hexdigest()
        if source_executable is not None else None
    )
    profile = {
        "schema_version": 1,
        "manifest": {
            "schema_version": 1,
            "id": title_id,
            "game_name": title_id,
            "display_name": "Synthetic experiment",
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
                "data_root": "",
                "memory_stick_root": "savedata",
                "device_prefixes": ["disc0:", "ms0:"],
            },
            "hle_profile": "generic",
            "feature_requirements": [],
            "verification_profile": "experimental-unverified",
        },
        "input_identity": {
            "disc_id": disc_id,
            "selected_executable": (
                "PSP_GAME/SYSDIR/EBOOT.BIN" if source_executable is not None else ""
            ),
            "executable_sha256": executable_hash,
            "elf_sha256": executable_hash,
        },
    }
    profile_path = runtime_root / "experimental" / disc_id / "profile.json"
    profile_path.parent.mkdir(parents=True)
    profile_path.write_text(json.dumps(profile, separators=(",", ":")) + "\n", encoding="utf-8")


def badge_rect(frame: dict[str, str]) -> tuple[int, int, int, int]:
    """The status badge rectangle the renderer reported for this frame."""
    x, y, w, h = (int(value) for value in frame["badge"].split(","))
    if w <= 0 or h <= 0:
        raise AssertionError(f"frame {frame.get('frame')} drew no status badge")
    return x, y, w, h


def has_amber_badge(bmp: tuple[int, int, int, bytes], rect: tuple[int, int, int, int]) -> bool:
    """True when the reported status badge is drawn in the amber accent."""
    width, height, _, _ = bmp
    x0, y0, w, h = rect
    right = min(width, x0 + w)
    bottom = min(height, y0 + h)
    return any(
        (lambda color: color[0] > 180 and 90 < color[1] < 205 and color[2] < 90)(
            pixel_rgb(bmp, x, y)
        )
        for y in range(max(0, y0), bottom)
        for x in range(max(0, x0), right)
    )


ICON_ART_RGB = (50, 190, 120)


def has_icon_art(bmp: tuple[int, int, int, bytes]) -> bool:
    width, height, _, _ = bmp
    return any(
        all(
            abs(channel - expected) <= 12
            for channel, expected in zip(
                pixel_rgb(bmp, x, y), ICON_ART_RGB, strict=True
            )
        )
        for y in range(112, min(196, height))
        for x in range(min(1120, width), min(1210, width))
    )


def parse_frames(output: str) -> list[dict[str, str]]:
    frames = []
    for line in output.splitlines():
        if not line.startswith("[PLAYER_UI_TEST] frame="):
            continue
        fields = dict(re.findall(r"([a-z_]+)=(\S+)", line))
        if "frame" not in fields or "view" not in fields:
            raise AssertionError(f"malformed player frame record: {line}")
        frames.append(fields)
    return frames


DEFAULT_BASE_TIMEOUT_SECONDS: float = 30.0
TIMEOUT_BUFFER_SECONDS: float = 15.0


def compute_effective_timeout(
    event_script: Sequence[str],
    explicit_timeout: float | None = None,
) -> float:
    """Compute adaptive subprocess timeout based on scripted UI test events.

    Scripted wait events parse as:
      - WAIT_MS=<duration_ms>: waits for explicit millisecond duration.
      - WAIT_VIEW=<view_mask>,<timeout_ms>: waits up to timeout_ms for view match.
      - WAIT_PACKAGE_STATUS=<status>,<timeout_ms>: waits for package validation.

    When explicit_timeout is None, sums all wait event bounds and adds
    TIMEOUT_BUFFER_SECONDS over DEFAULT_BASE_TIMEOUT_SECONDS to ensure slow
    or heavily loaded test runners do not trigger premature TimeoutExpired.
    """
    if explicit_timeout is not None:
        return explicit_timeout
    effective = DEFAULT_BASE_TIMEOUT_SECONDS
    for ev in event_script:
        if ev.startswith("WAIT_MS="):
            try:
                effective += int(ev.split("=", 1)[1]) / 1000.0
            except ValueError:
                pass
        elif ev.startswith("WAIT_VIEW=") and "," in ev:
            try:
                effective += int(ev.split(",", 1)[1]) / 1000.0
            except ValueError:
                pass
        elif ev.startswith("WAIT_PACKAGE_STATUS=") and "," in ev:
            try:
                effective += int(ev.rsplit(",", 1)[1]) / 1000.0
            except ValueError:
                pass
    return effective + TIMEOUT_BUFFER_SECONDS


class LegacyRendererSourceTests(unittest.TestCase):
    """The shipped player never reaches the old renderer (owner, v0.0.1)."""

    def test_ui_render_frame_is_called_only_in_the_regression_build(self) -> None:
        for source in sorted((ROOT / "src" / "player").rglob("*.c")):
            if source.name == "ui_renderer.c":
                continue
            depth = 0
            guarded = []
            for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
                stripped = line.strip()
                if stripped.startswith("#if"):
                    depth += 1
                    guarded.append("NK_PLAYER_UI_REGRESSION_TEST" in stripped and
                                   not stripped.startswith("#ifndef"))
                elif stripped.startswith("#else") or stripped.startswith("#elif"):
                    if guarded:
                        guarded[-1] = False
                elif stripped.startswith("#endif"):
                    depth -= 1
                    if guarded:
                        guarded.pop()
                elif "ui_render_frame(" in stripped and not stripped.startswith("void "):
                    self.assertTrue(any(guarded),
                                    f"{source.name}:{number} calls ui_render_frame outside "
                                    "the NK_PLAYER_UI_REGRESSION_TEST build")

    def test_shipped_player_has_no_legacy_switch(self) -> None:
        main = (ROOT / "src" / "player" / "main.c").read_text(encoding="utf-8")
        self.assertNotIn('getenv("NK_UI")', main)
        help_text = main[main.index('"Usage: nakagawa_player'):]
        help_text = help_text[:help_text.index("return 0;")]
        self.assertNotIn("--ui=", help_text)
        legacy = main.index('strcmp(argv[i], "--ui=legacy")')
        self.assertGreater(legacy, main.rindex("#ifdef NK_PLAYER_UI_REGRESSION_TEST", 0, legacy))
        self.assertGreater(main.index("#endif", legacy), legacy)


UPDATE_RELEASES_URL = "https://api.github.com/repos/Jstar269/nakagawa-recomp/releases?per_page=10"
UPDATE_DOWNLOAD_PREFIX = "https://github.com/Jstar269/nakagawa-recomp/releases/download/"


def write_update_fixture(
    folder: Path, version: str = "0.0.3", *, wrong_checksum: bool = False,
    missing_file: bool = False,
) -> None:
    """Synthetic GitHub answers for Check for updates, as the regression build's
    transport reads them from NK_UI_TEST_UPDATE_DIR: releases.json for
    the Releases API, then each asset by its file name. The ZIP has the release
    ZIP's layout (bin/nakagawa_player.exe and SHA256SUMS.txt)."""
    import io
    import zipfile

    name = f"nakagawa-recomp-{version}-windows-x64.zip"
    files = {"bin/nakagawa_player.exe": b"synthetic player\n", "START HERE.txt": b"synthetic\n"}
    listed = dict(files)
    if missing_file:
        listed["bin/SDL3.dll"] = b"listed but left out of the ZIP\n"
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {path}\n" for path, data in listed.items())
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as package:
        for path, data in files.items():
            package.writestr(path, data)
        package.writestr("SHA256SUMS.txt", sums)
    data = archive.getvalue()
    (folder / name).write_bytes(data)
    digest = "0" * 64 if wrong_checksum else hashlib.sha256(data).hexdigest()
    (folder / f"{name}.sha256").write_bytes(f"{digest}  {name}".encode("ascii") + b"\r\n")
    tag = f"v{version}"
    release = {
        "tag_name": tag,
        "html_url": f"https://github.com/Jstar269/nakagawa-recomp/releases/tag/{tag}",
        "draft": False,
        "prerelease": True,
        "published_at": "2026-11-04T12:00:00Z",
        "body": "Second preview. Faster builds.",
        "assets": [
            {"name": name, "size": len(data), "state": "uploaded",
             "browser_download_url": f"{UPDATE_DOWNLOAD_PREFIX}{tag}/{name}"},
            {"name": f"{name}.sha256", "size": 105, "state": "uploaded",
             "browser_download_url": f"{UPDATE_DOWNLOAD_PREFIX}{tag}/{name}.sha256"},
        ],
    }
    (folder / "releases.json").write_text(json.dumps([release]), encoding="utf-8")


def update_report(stdout: str) -> tuple[list[dict[str, str]], list[str]]:
    """The update steps the player settled on, and the URLs it asked for. A step
    in progress (checking, downloading, installing) and the quiet return to idle
    after a failed check at start are seen only when a frame falls inside them,
    so they are left out."""
    steps = []
    requests = []
    for line in stdout.splitlines():
        if line.startswith("[PLAYER_UI_TEST] update="):
            step = dict(re.findall(r"([a-z_]+)=(\S+)", line))
            if step["update"] not in ("checking", "downloading", "installing", "idle"):
                steps.append(step)
        elif line.startswith("[PLAYER_UI_TEST] update_request="):
            requests.append(line.split("=", 1)[1])
    return steps, requests


class NativePlayerUiTests(unittest.TestCase):
    # The next UI is the only launcher. These tests pin behaviour of the old
    # renderer, which only the regression build keeps (--ui=legacy): its
    # library layout and hit boxes, its disc-art workers, its package-status
    # badges and its build and error-card text metrics (the next UI's cards are
    # covered by tests/native/test_player_screens.c). They run with --ui=legacy
    # unless they name a mode themselves.
    LEGACY_RENDERER_TESTS = frozenset((
        "test_build_screen_follows_the_build_state",
        "test_cli_not_found_error_text_fits_at_minimum_window",
        "test_long_error_text_fits_at_minimum_window",
        "test_completed_retry_clears_failure_when_cache_generation_changes",
        "test_controller_bind_conflict_calibration_and_profile_save",
        "test_corrupt_art_is_failed_after_bounded_retries",
        "test_decoder_rejection_is_failed_after_bounded_retries",
        "test_exit_path_joins_iso_art_workers_and_tears_down_textures",
        "test_failed_post_build_worker_start_survives_cache_invalidation_and_retries",
        "test_iso_art_failures_reach_a_terminal_state_after_bounded_retries",
        "test_iso_art_retry_rereads_only_the_image_that_is_still_missing",
        "test_late_mounted_iso_art_retries_after_cooldown",
        "test_library_keyboard_gamepad_navigation_and_picker",
        "test_incompatible_card_offers_rebuild_package_in_the_legacy_renderer",
        "test_lost_worker_events_are_reclaimed_instead_of_stalling_the_card",
        "test_mouse_click_adds_disc_through_sdl_event_loop",
        "test_one_title_offline_art_and_stale_package_stay_off_frame_path",
        "test_package_status_thread_creation_failure_is_unresolved_and_retryable",
        "test_per_title_controller_mapping_choice",
        "test_readable_art_without_png_decoder_is_unsupported",
        "test_repeated_post_build_failures_keep_prior_title_retryable",
        "test_settings_about_opens_the_pinned_license_list",
        "test_settings_presentation_choice_saves_with_native_default_schema",
        "test_settings_removal_requires_confirmation_and_keeps_other_app_data",
        "test_settings_scaling_choice_cycles_and_saves_by_name",
        "test_settings_screen_returns_to_library",
        "test_settings_ui_sounds_toggle_is_the_last_stop_and_saves",
        "test_staged_card_exposes_runtime_required_status",
        "test_worker_start_failure_does_not_block_other_title_builds",
    ))

    def run_player(
        self,
        view: str,
        events: tuple[str, ...] = (),
        *,
        error_code: str | None = None,
        runtime_ready: bool = False,
        invalid_profile: bool = False,
        drop_invalid_iso: bool = False,
        drop_pbp_iso: bool = False,
        drop_bytes: tuple[str, bytes] | None = None,
        art_iso: str | None = None,
        stale_package: bool = False,
        incompatible_package: bool = False,
        wait_background: bool = False,
        catalog_reload_count: int = 0,
        build_ready_test: bool = False,
        experimental_build_ready: bool = False,
        post_build_ready: bool = False,
        downloaded_tool_payload: bool = False,
        data_root_present: bool = True,
        profile_refusal: bool = False,
        build_preflight: str | None = None,
        legacy_data: bool = False,
        env_extra: dict[str, str] | None = None,
        ui_mode: str | None = None,
        width: int = 1280,
        height: int = 720,
        timeout: float | None = None,
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            appdata = scratch / "appdata"
            localappdata = scratch / "localappdata"
            userprofile = scratch / "profile"
            for directory in (appdata, localappdata, userprofile):
                directory.mkdir()
            legacy_root = userprofile / "Nakagawa" / "data"
            if legacy_data:
                legacy_root.mkdir(parents=True)
            profile = scratch / "controller-profile.json"
            if invalid_profile:
                profile.write_text("{ definitely not a valid controller profile", encoding="utf-8")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()
            tool_marker: Path | None = None
            if downloaded_tool_payload:
                tool_marker = runtime_root / "prerequisites" / "synthetic-tool" / "marker.txt"
                tool_marker.parent.mkdir(parents=True)
                tool_marker.write_text("synthetic downloaded tool", encoding="utf-8")
            if profile_refusal:
                manifest_dir = runtime_root / "manifests"
                manifest_dir.mkdir()
                (manifest_dir / "retired-binding.json").write_text(
                    json.dumps({
                        "schema_version": 1,
                        "id": "retired-profile-fixture-v1",
                        "display_name": "Retired Profile Fixture",
                        "kind": "retail",
                        "disc": {
                            "id": "TEST00001",
                            "region": "NA",
                            "revision_policy": "exact-disc-id",
                        },
                        "executable": {
                            "base": "0x08800000",
                            "entry": "0x08804000",
                            "bss_metadata_source": "elf",
                            "extra_executable_spans": [],
                        },
                        "modules": [],
                        "filesystem": {
                            "data_root": "fixtures/profile_zero",
                            "memory_stick_root": "savedata",
                            "device_prefixes": ["disc0:", "ms0:"],
                        },
                        "hle_profile": "generic",
                        "feature_requirements": [],
                        "verification_profile": "unverified",
                        "runtime_bindings": {
                            "schema_version": 1,
                            "vblank_frame_counter_addr": "0x08804000",
                        },
                    }, separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )
            ready_source_iso: Path | None = None
            if build_ready_test:
                source_executable = b"synthetic input executable\n"
                ready_source_iso = scratch / "ready-source.iso"
                synthetic_art_iso(
                    ready_source_iso, source_executable=source_executable
                )
                synthetic_ready_package(
                    runtime_root, source_executable,
                    data_root_present=data_root_present,
                )
            elif experimental_build_ready:
                source_executable = b"synthetic input executable\n"
                ready_source_iso = scratch / "ready-source.iso"
                synthetic_art_iso(
                    ready_source_iso, source_executable=source_executable
                )
                synthetic_experimental_profile(runtime_root, "TEST00009", source_executable)
                synthetic_ready_package(
                    runtime_root, source_executable,
                    disc_id="TEST00009", title_id="experimental-test00009",
                )
            elif post_build_ready or incompatible_package:
                source_executable = (
                    build_plain_mips_elf() if incompatible_package
                    else b"synthetic input executable\n"
                )
                ready_source_iso = scratch / "ready-source.iso"
                if incompatible_package:
                    create_test_iso_with_executables(
                        ready_source_iso, source_executable, disc_id="TEST00006",
                        title="Synthetic Incompatible Package",
                    )
                    synthetic_ready_package(
                        runtime_root, source_executable,
                        disc_id="TEST00006", title_id="experimental-test00006",
                        data_root_present=data_root_present,
                    )
                else:
                    synthetic_art_iso(
                        ready_source_iso, source_executable=source_executable
                    )
                    synthetic_ready_package(
                        runtime_root, source_executable,
                        data_root_present=data_root_present,
                    )
            catalog_overlay: Path | None = None
            if catalog_reload_count:
                catalog_overlay = scratch / "catalog-overlay.json"
                catalog_overlay.write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "id": "synthetic-ui-catalog-reload-v1",
                            "display_name": "Synthetic UI catalog reload fixture",
                            "kind": "retail",
                            "disc": {
                                "id": "TEST00999",
                                "region": "NA",
                                "revision_policy": "exact-disc-id",
                            },
                            "executable": {
                                "base": "0x08800000",
                                "entry": "0x08800000",
                                "bss_metadata_source": "elf",
                                "extra_executable_spans": [],
                            },
                            "modules": [],
                            "filesystem": {
                                "data_root": "data",
                                "memory_stick_root": "ms",
                                "device_prefixes": ["host0:"],
                            },
                            "hle_profile": "standard",
                            "feature_requirements": ["allegrex"],
                            "verification_profile": "smoke",
                        },
                        separators=(",", ":"),
                    ),
                    encoding="utf-8",
                )
            ui_iso_path: Path | None = None
            art_mount_source: Path | None = None
            if build_preflight is not None:
                if build_preflight == "encrypted":
                    executable = build_psp_container()
                elif build_preflight == "missing-manifest":
                    executable = build_plain_mips_elf()
                else:
                    raise ValueError(f"unknown build preflight fixture: {build_preflight}")
                ui_iso_path = scratch / "build-preflight.iso"
                create_test_iso_with_executables(
                    ui_iso_path, executable, disc_id="TEST00009",
                    title="Synthetic Build Preflight",
                )
                if build_preflight == "encrypted":
                    synthetic_experimental_profile(runtime_root, "TEST00009", None)
            if art_iso is not None:
                if art_iso == "saturated":
                    # The cache exerciser appends a per-slot suffix. Keep
                    # printf tokens in this caller-owned path to prove it is
                    # always passed as data to a fixed format string.
                    ui_iso_path = scratch / "art_%s_%p.iso"
                else:
                    ui_iso_path = scratch / "source-owned-synthetic.iso"
                    synthetic_art_iso(
                        ui_iso_path,
                        include_picture=art_iso != "icon-only",
                        icon_bytes=(
                            undecodable_png() if art_iso == "undecodable" else
                            corrupt_png_header() if art_iso == "corrupt" else None
                        ),
                    )
                    if art_iso == "unavailable":
                        ui_iso_path.rename(scratch / "source-owned-synthetic.iso.offline")
                    elif art_iso == "late":
                        art_mount_source = ui_iso_path
                        ui_iso_path = scratch / "late-mounted-synthetic.iso"
                    elif art_iso not in ("available", "icon-only", "undecodable", "corrupt"):
                        raise ValueError(f"unknown synthetic ISO mode: {art_iso}")
            if ready_source_iso is not None:
                ui_iso_path = ready_source_iso
            if stale_package:
                synthetic_stale_package(runtime_root)
            if incompatible_package:
                break_package_manifest(runtime_root)

            env = os.environ.copy()
            env.update(
                {
                    "SDL_VIDEODRIVER": "dummy",
                    "SDL_RENDER_DRIVER": "software",
                    "APPDATA": str(appdata),
                    "LOCALAPPDATA": str(localappdata),
                    "USERPROFILE": str(userprofile),
                    "HOME": str(userprofile),
                    "XDG_CONFIG_HOME": str(userprofile / "config"),
                    "XDG_DATA_HOME": str(userprofile / "data"),
                    "XDG_CACHE_HOME": str(userprofile / "cache"),
                    "NK_INPUT_PROFILE": str(profile),
                }
            )
            if env_extra:
                env.update(env_extra)
            if incompatible_package:
                env["NK_UI_TEST_BIND_EBOOT"] = "1"
            if build_ready_test:
                view = "build-ready"
            elif experimental_build_ready:
                view = "build-ready-experimental"
            args = [str(PLAYER_EXE), f"--view={view}"]
            if ui_mode is None and self._testMethodName in self.LEGACY_RENDERER_TESTS:
                ui_mode = "legacy"
            if ui_mode is not None:
                args.append(f"--ui={ui_mode}")
            event_script = list(events)
            if drop_invalid_iso:
                invalid_iso = scratch / "source-owned-invalid.iso"
                invalid_iso.write_bytes(b"synthetic invalid PSP disc image\n")
                event_script.insert(0, f"DROP_FILE={invalid_iso}")
            elif drop_pbp_iso:
                package_iso = scratch / "source-owned-store-package.iso"
                synthetic_pbp_package(package_iso)
                event_script.insert(0, f"DROP_FILE={package_iso}")
            elif drop_bytes is not None:
                dropped = scratch / drop_bytes[0]
                dropped.write_bytes(drop_bytes[1])
                event_script.insert(0, f"DROP_FILE={dropped}")
            args.append("--ui-test-events=" + ";".join(event_script))
            screenshot = scratch / "last-frame.bmp"
            args.append(f"--ui-test-screenshot={screenshot}")
            if ui_iso_path is not None:
                args.append(f"--ui-test-iso-path={ui_iso_path}")
            if wait_background:
                args.append("--ui-test-wait-background")
            if catalog_overlay is not None:
                args.extend(
                    (
                        f"--ui-test-catalog-reload-path={catalog_overlay}",
                        f"--ui-test-catalog-reload-count={catalog_reload_count}",
                    )
                )
            if art_mount_source is not None:
                args.append(f"--ui-test-art-mount-source={art_mount_source}")
            args.append(f"--width={width}")
            args.append(f"--height={height}")
            if error_code:
                args.append(f"--ui-test-error-code={error_code}")

            if runtime_ready:
                package = runtime_root / "build" / "display-smoke-v1"
                package.mkdir(parents=True)
                # The launcher resolves a developer package by path existence;
                # these empty files are synthetic sentinels, never executed.
                (package / "display-smoke-v1.exe").write_bytes(b"")
                (package / "display-smoke-v1").write_bytes(b"")
                (package / "display-smoke-v1_image.bin").write_bytes(b"")
                (runtime_root / "fixtures" / "display_smoke").mkdir(parents=True, exist_ok=True)
            args.append(f"--runtime-root={runtime_root}")

            effective_timeout = compute_effective_timeout(event_script, timeout)

            started = time.perf_counter()
            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=effective_timeout,
                check=False,
            )
            wall_seconds = time.perf_counter() - started
            if completed.returncode != 0:
                self.fail(
                    f"player returned {completed.returncode}\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            frames = parse_frames(completed.stdout)
            screenshot_line = next(
                (line for line in completed.stdout.splitlines() if line.startswith("[PLAYER_UI_TEST] screenshot=")),
                "",
            )
            self.assertIn("screenshot=PASS", screenshot_line, completed.stdout)
            self.assertGreaterEqual(len(frames), 2, completed.stdout)
            self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in frames))
            self.assertTrue(screenshot.is_file())
            bmp = read_bmp(screenshot)
            self.assertEqual((bmp[0], bmp[1]), (width, height))
            persisted_settings = None
            for settings_path in scratch.rglob("settings.json"):
                try:
                    candidate = json.loads(settings_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    continue
                if isinstance(candidate, dict) and "fps_cap" in candidate:
                    persisted_settings = candidate
                    break
            return {
                "frames": frames,
                "bmp": bmp,
                "profile_exists": profile.is_file(),
                "profile_text": profile.read_text(encoding="utf-8") if profile.is_file() else "",
                "stdout": completed.stdout,
                "wall_seconds": wall_seconds,
                "stderr": completed.stderr,
                "legacy_root": legacy_root,
                "legacy_root_exists": legacy_root.is_dir(),
                "settings": persisted_settings,
                "downloaded_tool_marker_exists": (
                    tool_marker.is_file() if tool_marker is not None else None
                ),
            }

    def test_compute_effective_timeout(self) -> None:
        """Effective timeout scales with scripted wait events and respects explicit bounds."""
        self.assertEqual(compute_effective_timeout([], explicit_timeout=12.5), 12.5)
        self.assertEqual(
            compute_effective_timeout([]),
            DEFAULT_BASE_TIMEOUT_SECONDS + TIMEOUT_BUFFER_SECONDS,
        )
        events = [
            "WAIT_MS=2500",
            "WAIT_VIEW=ready_library,10000",
            "WAIT_PACKAGE_STATUS=INCOMPATIBLE,3000",
            "WAIT_VIEW=error,5000",
            "QUIT",
        ]
        expected = DEFAULT_BASE_TIMEOUT_SECONDS + 2.5 + 10.0 + 3.0 + 5.0 + TIMEOUT_BUFFER_SECONDS
        self.assertEqual(compute_effective_timeout(events), expected)

    @unittest.skipUnless(sys.platform == "win32", "legacy %USERPROFILE%\\Nakagawa\\data migration warning is Windows-only")
    def test_legacy_data_root_is_reported_once_without_moving_it(self) -> None:
        run = self.run_player("empty", legacy_data=True)
        stderr = run["stderr"]
        legacy_root = run["legacy_root"]
        assert isinstance(stderr, str)
        assert isinstance(legacy_root, Path)
        expected_old_path = str(legacy_root.parent.parent) + "\\Nakagawa\\data"
        marker = f'LEGACY_DATA_DIR_FOUND: old_path="{expected_old_path}"'
        self.assertEqual(stderr.count(marker), 1, stderr)
        self.assertTrue(run["legacy_root_exists"])

    @unittest.skipUnless(sys.platform == "win32", "player-output.txt is the Windows GUI player's console stand-in")
    def test_player_output_file_holds_the_output_of_a_console_less_launch(self) -> None:
        """A double-click start has no console and no standard handles, so the player writes
        its output to logs/player-output.txt. Started like that (no console to attach to, null
        standard handles), the file must hold what the player printed."""
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            env = os.environ.copy()
            for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE"):
                folder = scratch / name.lower()
                folder.mkdir()
                env[name] = str(folder)
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIO_DRIVER"] = "dummy"
            # The launcher runs detached, so there is no parent console the player could
            # attach to, and it starts the player detached with null standard handles: what
            # Explorer gives the GUI-subsystem player (the test build is console-subsystem).
            completed = subprocess.run(
                [sys.executable, "-I", "-c", CONSOLE_LESS_LAUNCHER, str(PLAYER_EXE), "--help"],
                cwd=scratch,
                env=env,
                creationflags=subprocess.DETACHED_PROCESS,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=90,
                check=False,
            )
            self.assertEqual(completed.returncode, 0)
            outputs = list(scratch.rglob("player-output.txt"))
            self.assertEqual(len(outputs), 1, outputs)
            text = outputs[0].read_text(encoding="utf-8", errors="replace")
            self.assertIn("Usage: nakagawa_player", text)

    def test_empty_library_wizard_picker_and_font_confirmation(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "library")
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")
        self.assertEqual(frames[3]["online_mode"], "0", "CONTINUE moves on with both off")
        self.assertEqual(frames[4]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])

        fonts = self.run_player("wizard4", ("KEY_RETURN",))
        font_frames = fonts["frames"]
        assert isinstance(font_frames, list)
        self.assertEqual(font_frames[1]["wizard_step"], "ready_launch")
        self.assertEqual(font_frames[1]["font_confirmed"], "1")

    def test_wizard_options_step_sets_the_switches_and_skip_clears_them(self) -> None:
        # Focus order on the step: CONTINUE, SKIP, BACK, then the two switches.
        run = self.run_player(
            "wizard-options",
            ("KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN", "KEY_TAB", "KEY_RETURN"),
            ui_mode="legacy",
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["wizard_step"], "online_options")
        self.assertEqual(frames[4]["online_mode"], "1")
        self.assertEqual(frames[6]["developer_mode"], "1")
        skipped = self.run_player(
            "wizard-options",
            ("KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN", "KEY_UP", "KEY_UP", "KEY_RETURN"),
            ui_mode="legacy",
        )
        frames = skipped["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[4]["online_mode"], "1")
        self.assertEqual(frames[-1]["wizard_step"], "select_game")
        self.assertEqual(frames[-1]["online_mode"], "0", "SKIP turns online mode back off")
        self.assertEqual(frames[-1]["developer_mode"], "0")

    def test_developer_log_panel_needs_developer_mode(self) -> None:
        # The next UI draws its own panel; --ui=legacy keeps the existing one.
        for ui_mode in ("next", "legacy"):
            with self.subTest(ui_mode=ui_mode):
                panel = self.run_player("developer-log", ("KEY_F12",), ui_mode=ui_mode)
                frames = panel["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "developer_log")
                self.assertNotEqual(frames[0]["dev_log_lines"], "0")
                self.assertEqual(frames[1]["view"], "library", "F12 closes the panel")
                closed = self.run_player("developer-log", ("KEY_ESCAPE",), ui_mode=ui_mode)
                frames = closed["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[1]["view"], "library", "Esc closes the panel")
                off = self.run_player("library", ("KEY_F12",), ui_mode=ui_mode)
                frames = off["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[-1]["view"], "library",
                                 "without developer mode F12 does nothing")

    def test_online_settings_open_from_settings_and_back(self) -> None:
        for ui_mode in ("next", "legacy"):
            with self.subTest(ui_mode=ui_mode):
                run = self.run_player("online-settings", ("KEY_ESCAPE",), ui_mode=ui_mode)
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "online_settings")
                self.assertEqual(frames[1]["view"], "settings")

    def test_build_cache_page_leads_to_tool_removal_and_back(self) -> None:
        """#1306: Settings > Build tools and cache holds the shared-library switch, the
        clear button and tool removal; cancelling a removal returns to the page."""
        for ui_mode in ("next", "legacy"):
            with self.subTest(ui_mode=ui_mode):
                run = self.run_player(
                    "build-cache",
                    ("KEY_TAB", "KEY_TAB", "KEY_RETURN", "KEY_ESCAPE", "KEY_ESCAPE"),
                    ui_mode=ui_mode,
                )
                frames = run["frames"]
                assert isinstance(frames, list)
                views = [frame["view"] for frame in frames]
                visited = [view for i, view in enumerate(views) if i == 0 or views[i - 1] != view]
                self.assertEqual(
                    visited, ["build_cache", "confirm_remove_tools", "build_cache", "settings"], views
                )

    def test_next_ui_library_card_shows_the_disc_icon_once_read(self) -> None:
        """The next UI reads ICON0.PNG from the disc in the background and shows
        it on the card; a disc that cannot be read keeps the placeholder."""
        shown = self.run_player("ready", ("WAIT_MS=1500",), art_iso="available", ui_mode="next")
        missing = self.run_player("ready", ("WAIT_MS=1500",), art_iso="unavailable", ui_mode="next")
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in shown["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        self.assertIn("[PLAYER_UI_TEST] next_ui_disc_icon index=0 width=1 height=1", shown["stdout"],
                      "the disc's 1 x 1 ICON0.PNG is decoded and shown on its card")
        self.assertNotIn("next_ui_disc_icon", missing["stdout"],
                         "an unreadable disc keeps the placeholder")

    def test_next_ui_is_the_only_launcher(self) -> None:
        """The owner's rule for v0.0.1: the new launcher or nothing.

        A player whose launcher cannot start (an extraction that lost
        assets/fonts/ui) says why and exits; it never draws the old renderer."""
        marker = "[PLAYER_UI_TEST] UI_NEXT_ACTIVE"
        default_ui = self.run_player("empty")
        if marker not in default_ui["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            env = os.environ.copy()
            env.update({
                # The same headless drivers as run_player: a hosted Linux runner has no display,
                # and SDL must start for the player to reach its launcher check.
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIO_DRIVER": "dummy",
                "LOCALAPPDATA": str(scratch),
                "APPDATA": str(scratch),
                "NK_UI_TEST_HIDE_UI_FONTS": "1",
            })
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={scratch / 'data'}",
                 "--ui-test-events=WAIT_MS=100;QUIT",
                 f"--ui-test-screenshot={scratch / 'shot.bmp'}"],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=60, check=False,
            )
        self.assertEqual(completed.returncode, 4, completed.stderr)
        self.assertIn("LAUNCHER_UNAVAILABLE", completed.stderr)
        self.assertIn("Extract the ZIP again", completed.stderr)
        self.assertIn("assets\\fonts\\ui", completed.stderr)
        self.assertNotIn(marker, completed.stdout)
        self.assertNotIn("frame=", completed.stdout, "nothing is drawn after the message")

    def test_every_view_is_drawn_by_the_launcher(self) -> None:
        """Every PlayerView in player_state.h is drawn by the nk_ui launcher."""
        header = (ROOT / "src" / "player" / "player_state.h").read_text(encoding="utf-8")
        body = header[header.index("typedef enum {\n    VIEW_LIBRARY"):]
        body = body[:body.index("} PlayerView;")]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        declared = re.findall(r"\b((?:PLAYER_)?VIEW_[A-Z_]+)\b", body)
        run = self.run_player("library", ("WAIT_MS=50",),
                              env_extra={"NK_UI_TEST_REPORT_VIEWS": "1"})
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        coverage = re.findall(r"\[PLAYER_UI_TEST\] view_coverage value=(\d+) name=(\S+) nk=(\d)",
                              str(run["stdout"]))
        self.assertEqual(len(coverage), len(declared), (coverage, declared))
        self.assertEqual([name for _, name, nk in coverage if nk != "1"], [],
                         "views the launcher does not draw")
        preparing = self.run_player("preparing", ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"))
        self.assertIn("[PLAYER_UI_TEST] UI_NEXT_ACTIVE", preparing["stdout"])
        self.assertEqual(self.screens(preparing)[-1][0], "library",
                         "Return to Library leaves the preparation card")

    def test_settings_psp_fonts_imports_and_removes(self) -> None:
        """Walkthrough blocker: the PSP font import is reachable from Settings."""
        run = self.run_player("psp-fonts", ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "psp_fonts")
        self.assertIn(str(0x451E), self.focus_trail(run), "Import fonts folder has focus")
        back = self.run_player("psp-fonts", ("WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"))
        self.assertEqual(self.screens(back)[-1][0], "settings", "Esc goes back to Settings")

    def test_next_ui_mouse_and_keyboard_reach_the_add_card(self) -> None:
        click = self.run_player("library", ("MOUSE_MOVE=794,257", "MOUSE_DOWN", "MOUSE_UP"))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in click["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = click["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1", "a click on the add card opens the picker")
        # The focus graph needs one laid-out frame before arrows can move.
        keys = self.run_player("library", ("WAIT_MS=100", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN"))
        frames = keys["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1", "arrow keys reach the add card")

    @staticmethod
    def screens(run: dict[str, object]) -> list[tuple[str, str]]:
        """The (view, next UI route) a run showed, with repeats collapsed."""
        frames = run["frames"]
        assert isinstance(frames, list)
        shown: list[tuple[str, str]] = []
        for frame in frames:
            screen = (frame["view"], frame["next_route"])
            if not shown or shown[-1] != screen:
                shown.append(screen)
        return shown

    def test_next_ui_about_tab_opens_by_mouse_keyboard_and_controller(self) -> None:
        """#1281: the ABOUT tab opens About, and Back returns where it came from."""
        library, about, title = ("library", "1"), ("library", "3"), ("library", "2")
        runs = {
            "mouse": ("WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN", "MOUSE_UP",
                      "KEY_ESCAPE"),
            "keyboard": ("WAIT_MS=100", "KEY_UP", "KEY_RIGHT", "KEY_RETURN", "KEY_ESCAPE"),
            "d-pad": ("WAIT_MS=100", "PAD_DPAD_UP", "PAD_DPAD_RIGHT", "PAD_SOUTH",
                      "PAD_EAST"),
            "shoulder": ("WAIT_MS=100", "PAD_RIGHT_SHOULDER", "PAD_LEFT_SHOULDER"),
        }
        for name, events in runs.items():
            run = self.run_player("library", events)
            if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
            with self.subTest(input=name):
                self.assertEqual(self.screens(run), [library, about, library])
        empty = self.run_player("empty", runs["mouse"])
        self.assertEqual(self.screens(empty), [library, about, library],
                         "ABOUT opens with an empty library too")
        from_title = self.run_player(
            "library",
            ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN",
             "MOUSE_UP", "KEY_ESCAPE", "KEY_ESCAPE"),
        )
        self.assertEqual(self.screens(from_title), [library, title, about, title, library],
                         "Back from About returns to the title page it was opened from")

    def test_next_ui_settings_tab_opens_by_mouse_keyboard_and_controller(self) -> None:
        """#1281: Settings has a visible SETTINGS tab, and the menu prompt opens it."""
        library, settings = ("library", "1"), ("settings", "1")
        runs = {
            "mouse": ("WAIT_MS=100", "MOUSE_MOVE=470,30", "MOUSE_DOWN", "MOUSE_UP",
                      "KEY_ESCAPE"),
            "keyboard": ("WAIT_MS=100", "KEY_UP", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN",
                         "KEY_ESCAPE"),
            "menu key": ("WAIT_MS=100", "KEY_F1", "KEY_ESCAPE"),
            "d-pad": ("WAIT_MS=100", "PAD_DPAD_UP", "PAD_DPAD_RIGHT", "PAD_DPAD_RIGHT",
                      "PAD_SOUTH", "PAD_EAST"),
            "start": ("WAIT_MS=100", "PAD_START", "PAD_EAST"),
        }
        for name, events in runs.items():
            run = self.run_player("library", events)
            if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
            with self.subTest(input=name):
                self.assertEqual(self.screens(run), [library, settings, library])
        empty = self.run_player("empty", runs["mouse"])
        self.assertEqual(self.screens(empty), [library, settings, library],
                         "the SETTINGS tab works with an empty library too")
        from_about = self.run_player(
            "library", ("WAIT_MS=100", "PAD_RIGHT_SHOULDER", "PAD_RIGHT_SHOULDER", "PAD_EAST")
        )
        self.assertEqual(
            self.screens(from_about),
            [library, ("library", "3"), ("settings", "3"), ("library", "3")],
            "R steps from About to Settings, and Back returns to About",
        )

    def next_ui_run(self, view: str, events: tuple[str, ...], **kwargs: object) -> dict[str, object]:
        run = self.run_player(view, events, ui_mode="next", **kwargs)  # type: ignore[arg-type]
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        return run

    @staticmethod
    def disc_card(run: dict[str, object]) -> tuple[dict[str, str], list[dict[str, str]]]:
        """The game-found card the next UI drew first: its kind and its check rows."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        card: dict[str, str] = {}
        rows: list[dict[str, str]] = []
        for line in stdout.splitlines():
            if line.startswith("[PLAYER_UI_TEST] next_ui_disc ") and not card:
                card = dict(part.split("=", 1) for part in line.split()[2:])
            elif line.startswith("[PLAYER_UI_TEST] next_ui_disc_row ") and card and \
                    len(rows) < int(card["rows"]):
                fields = line[len("[PLAYER_UI_TEST] next_ui_disc_row "):]
                status, rest = fields.split(" tone=", 1)
                tone, rest = rest.split(" label=", 1)
                label, message = rest.split(" message=", 1)
                rows.append({"status": status[len("status="):], "tone": tone,
                             "label": label, "message": message})
        return card, rows

    def assert_plain_rows(self, rows: list[dict[str, str]], stdout: object) -> None:
        self.assertTrue(rows, stdout)
        for row in rows:
            with self.subTest(row=row["label"]):
                self.assertTrue(row["label"] and row["message"] and row["status"], row)
                self.assertNotIn("Not supported", row["status"])
                # Developer codes and messages stay in the log (#1280, #1302).
                self.assertNotRegex(row["message"], r"\b[A-Z][A-Z_]{3,}\b:")
                self.assertNotIn(" .", row["message"])
                self.assertNotIn("PARAM.SFO", row["message"])
                self.assertNotIn("jpn0", row["message"])
                self.assertTrue(row["message"].endswith("."), row)

    def test_next_ui_game_found_cards_use_plain_words_and_offer_add_honestly(self) -> None:
        """#1302: the three discs' cards in the next UI, with their real checks."""
        supported = self.next_ui_run("supported", ("WAIT_MS=100", "KEY_ESCAPE"))
        card, rows = self.disc_card(supported)
        self.assertEqual((card.get("kind"), card.get("add")), ("1", "1"), supported["stdout"])
        self.assert_plain_rows(rows, supported["stdout"])
        self.assertNotIn("error", [row["tone"] for row in rows],
                         "nothing beside Add to library reads as an error")
        fonts = [row for row in rows if row["label"] == "Fonts"]
        self.assertEqual(len(fonts), 1, rows)
        self.assertEqual((fonts[0]["status"], fonts[0]["tone"]), ("Some missing", "warning"),
                         "missing PSP fonts are a warning the game can still be added with")
        self.assertEqual(self.screens(supported)[-1][0], "library")

        untested = self.next_ui_run("experimental", ("WAIT_MS=100", "KEY_RETURN"))
        card, rows = self.disc_card(untested)
        frames = untested["frames"]
        assert isinstance(frames, list)
        games_before = int(frames[0]["games"])
        self.assertEqual((card.get("kind"), card.get("add")), ("2", "1"), untested["stdout"])
        self.assert_plain_rows(rows, untested["stdout"])
        self.assertNotIn("error", [row["tone"] for row in rows])
        self.assertEqual(frames[-1]["view"], "library", "Add to library adds and returns")
        self.assertEqual(int(frames[-1]["games"]), games_before + 1)

        blocked = self.next_ui_run("experimental-blocked", ("WAIT_MS=100", "KEY_RETURN"))
        card, rows = self.disc_card(blocked)
        self.assertEqual((card.get("kind"), card.get("add")), ("3", "0"), blocked["stdout"])
        self.assert_plain_rows(rows, blocked["stdout"])
        self.assertNotIn("Game program", [row["label"] for row in rows],
                         "the check that keeps the disc out is the card's reason, not a row")
        frames = blocked["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1", "Choose another disc opens the picker")
        self.assertEqual(frames[-1]["games"], frames[0]["games"], "nothing is added")

        unsupported = self.next_ui_run("unsupported", ("WAIT_MS=100", "KEY_RETURN"))
        card, _ = self.disc_card(unsupported)
        self.assertEqual((card.get("kind"), card.get("add")), ("4", "0"), unsupported["stdout"])
        self.assertEqual(self.screens(unsupported)[-1][0], "library")

    # The next UI's Settings controls (player_ui_nk.h), as the frame line prints them.
    SETTINGS_FOCUS = {
        "resolution": str(0x4501), "pacing": str(0x4502), "done": str(0x450C),
        "online": str(0x450D), "notices": str(0x450E), "remove_tools": str(0x450F),
        "build_cache": str(0x451A), "share_libraries": str(0x451B),
        "online_mode": str(0x4511), "back": str(0x4517), "confirm_remove": str(0x4519),
        "controller": str(0x450B), "scaling": str(0x4505),
        "launcher_fullscreen": str(0x450A),
    }
    # From Resolution, Down runs through the Game card's five rows to the button
    # row, which it enters at Third-party notices.
    TO_SETTINGS_BUTTONS = ("KEY_DOWN",) * 5

    def focus_trail(self, run: dict[str, object]) -> list[str]:
        """The controls the next UI focused, with repeats collapsed."""
        frames = run["frames"]
        assert isinstance(frames, list)
        trail: list[str] = []
        for frame in frames:
            if not trail or trail[-1] != frame["next_focus"]:
                trail.append(frame["next_focus"])
        return trail

    def test_next_ui_settings_changes_are_saved_and_done_returns(self) -> None:
        """#1302: Settings in the next UI runs the same handlers and saves each change."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100", "KEY_RETURN", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertEqual(self.focus_trail(run)[:2], [focus["resolution"], focus["pacing"]],
                         "Settings opens on Resolution")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("resolution_scale"), 2, "Resolution steps 1x to 2x")
        self.assertEqual(settings.get("fps_cap"), 0, "Frame pacing switches to no extra cap")
        self.assertEqual(self.screens(run)[-1][0], "library")

        done = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + ("KEY_LEFT", "KEY_LEFT", "KEY_RETURN"),
        )
        self.assertIn(focus["done"], self.focus_trail(done))
        self.assertEqual(self.screens(done), [("settings", "1"), ("library", "1")],
                         "Done saves and returns to the library")

        tabs = self.next_ui_run(
            "settings", ("WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN", "MOUSE_UP")
        )
        self.assertEqual(self.screens(tabs), [("settings", "1"), ("library", "3")],
                         "the ABOUT tab on Settings closes it and opens About")

    def test_next_ui_settings_pages_open_and_return(self) -> None:
        """#1302: the online, notices, build tools and cache (#1306) and removal pages are
        next-UI cards too."""
        focus = self.SETTINGS_FOCUS
        online = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS +
            ("KEY_LEFT", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(online)],
                         ["settings", "online_settings", "settings"])
        trail = self.focus_trail(online)
        self.assertEqual(trail[-3:], [focus["online"], focus["online_mode"], focus["online"]],
                         "the page opens on its first switch and Back focuses its button")
        frames = online["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["online_mode"], "1")

        notices = self.next_ui_run(
            "settings", ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + ("KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(notices)],
                         ["settings", "prereq_about"])
        self.assertEqual(self.focus_trail(notices)[-1], focus["back"])

        # Build tools and cache opens on its switch; Tab reaches Remove build tools.
        # Right comes straight after the removal card opens, before its buttons are
        # laid out; nk_ui holds it a frame instead of losing it (a slow CI runner did).
        to_removal = ("KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100", "KEY_TAB", "KEY_TAB",
                      "KEY_RETURN")
        removal = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + to_removal +
            ("WAIT_MS=1", "KEY_RIGHT", "KEY_RETURN"),
            downloaded_tool_payload=True,
        )
        self.assertEqual([view for view, _ in self.screens(removal)],
                         ["settings", "build_cache", "confirm_remove_tools", "settings"])
        trail = self.focus_trail(removal)
        self.assertIn(focus["build_cache"], trail)
        self.assertIn(focus["share_libraries"], trail, "the page opens on its switch")
        self.assertIn(focus["remove_tools"], trail)
        self.assertIn(focus["confirm_remove"], trail)
        self.assertFalse(removal["downloaded_tool_marker_exists"])

        kept = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + to_removal +
            ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"),
            downloaded_tool_payload=True,
        )
        self.assertTrue(kept["downloaded_tool_marker_exists"],
                        "the removal card opens on the choice that keeps the tools")
        self.assertEqual([view for view, _ in self.screens(kept)],
                         ["settings", "build_cache", "confirm_remove_tools", "build_cache"],
                         "keeping the tools returns to build tools and cache")
        self.assertEqual(self.focus_trail(kept)[-1], focus["remove_tools"],
                         "and focuses the button that opened the removal card")

        shared = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS +
            ("KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(shared)],
                         ["settings", "build_cache", "settings"])
        self.assertEqual(self.focus_trail(shared)[-1], focus["build_cache"],
                         "Back focuses the button that opened the page")
        settings = shared["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("share_system_libraries"), False,
                      "the switch turns library sharing off and saves it")

    # The next UI's controller page (player_ui_nk.h), as the frame line prints it.
    CONTROLLER_FOCUS = {
        "select": str(0x4600), "start": str(0x4601), "calibrate": str(0x4625),
        "back": str(0x4628), "scope": str(0x4629), "calibration_cancel": str(0x462B),
    }
    # From the first binding, two steps right and two down reach Calibrate stick.
    TO_CALIBRATE = ("KEY_RIGHT", "KEY_RIGHT", "KEY_DOWN", "KEY_DOWN")

    def test_next_ui_controller_rebind_calibrate_save_and_back(self) -> None:
        """Controller settings in the next UI run the same handlers as the existing renderer."""
        focus = self.CONTROLLER_FOCUS
        bind = self.next_ui_run(
            "controller", ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "PAD_SOUTH", "WAIT_MS=100"),
        )
        frames = bind["frames"]
        assert isinstance(frames, list)
        self.assertEqual(self.focus_trail(bind)[:2], [focus["select"], focus["start"]],
                         "the page opens on the first binding")
        self.assertIn("1", [frame["controller_capturing"] for frame in frames],
                      "choosing a binding waits for a button")
        self.assertEqual(frames[-1]["controller_capturing"], "0",
                         "the controller's button is taken, not routed to the page")
        self.assertEqual(frames[-1]["controller_conflicts"], "1")
        self.assertNotEqual(frames[0]["start_binding"], frames[-1]["start_binding"])

        cancel = self.next_ui_run(
            "controller", ("WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        frames = cancel["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["controller_capturing"] for frame in frames])
        self.assertEqual(frames[-1]["view"], "controller", "Esc while waiting only cancels")
        self.assertEqual(frames[-1]["controller_capturing"], "0")
        self.assertEqual(frames[-1]["select_binding"], frames[0]["select_binding"])

        calibrate = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE +
            ("KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        frames = calibrate["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["calibrating"] for frame in frames])
        self.assertEqual(frames[-1]["calibrating"], "0")
        self.assertEqual(frames[-1]["view"], "controller")
        self.assertEqual(self.focus_trail(calibrate)[-3:],
                         [focus["calibrate"], focus["calibration_cancel"], focus["calibrate"]],
                         "the rest step opens on Cancel and Esc returns to Calibrate stick")

        # Down from Calibrate stick reaches Back, at the right end of the button
        # row; the mapping choice is next to it and Save profile is first.
        scope = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_LEFT", "KEY_RETURN",
                                                     "WAIT_MS=100"),
        )
        frames = scope["frames"]
        assert isinstance(frames, list)
        self.assertEqual(self.focus_trail(scope)[-3:],
                         [focus["calibrate"], focus["back"], focus["scope"]])
        self.assertEqual(frames[0]["input_scope"], "global")
        self.assertEqual(frames[-1]["input_scope"], "TEST00005")

        save = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN",) + ("KEY_LEFT",) * 3 +
            ("KEY_RETURN", "WAIT_MS=100"),
        )
        frames = save["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["profile_save_notice"], "1")
        self.assertTrue(save["profile_exists"])

        back = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(back)], ["controller", "settings"])
        self.assertEqual(self.focus_trail(back)[-1], self.SETTINGS_FOCUS["controller"],
                         "Back returns to Settings on Controls")

    def test_next_ui_keyboard_keys_rebind_refuse_reset_and_save(self) -> None:
        """#1299: KEYBOARD shows the keys; a chosen button takes the next key pressed,
        a key another button uses is refused, Reset to defaults resets the keys, and
        Save profile writes them to the profile the game is handed."""
        # Up from the first binding is KEYBOARD; Down twice from it is Start's key.
        to_start = ("WAIT_MS=100", "KEY_UP", "KEY_RETURN", "WAIT_MS=100", "KEY_DOWN", "KEY_DOWN")
        # Down from L Trigger, the last row of the first column, is Reset to defaults.
        to_reset = ("KEY_DOWN",) * 6
        save = self.next_ui_run(
            "controller",
            to_start + ("KEY_RETURN", "KEY_C", "WAIT_MS=100") + to_reset +
            ("KEY_LEFT", "KEY_RETURN", "WAIT_MS=100"),
        )
        frames = save["frames"]
        assert isinstance(frames, list)
        self.assertEqual(self.focus_trail(save)[1:4], [str(0x462D), str(0x4600), str(0x4601)])
        self.assertEqual(frames[0]["input_device"], "controller")
        self.assertEqual(frames[-1]["input_device"], "keyboard")
        self.assertIn("1", [frame["controller_capturing"] for frame in frames],
                      "choosing Start's key waits for a key")
        self.assertEqual((frames[0]["start_key"], frames[-1]["start_key"]), ("40", "6"),
                         "the C key pressed is Start's key, not routed to the page")
        self.assertEqual(frames[-1]["profile_save_notice"], "1")
        profile = json.loads(str(save["profile_text"]))
        keys = {entry["control"]: entry["primary"] for entry in profile["keyboard_bindings"]}
        self.assertEqual(keys["start"], "c")

        refuse = self.next_ui_run(
            "controller",
            to_start + ("KEY_RETURN", "KEY_X", "WAIT_MS=100", "KEY_RETURN", "PAD_SOUTH",
                        "WAIT_MS=100", "KEY_RETURN", "KEY_C", "WAIT_MS=100") + to_reset +
            ("KEY_RETURN", "WAIT_MS=100"),
        )
        frames = refuse["frames"]
        assert isinstance(frames, list)
        refused = [frame for frame in frames if frame["key_note"] == "1"]
        self.assertTrue(refused, "X, Cross's key, is refused with a note")
        self.assertEqual({frame["start_key"] for frame in refused}, {"40"})
        self.assertEqual({frame["controller_capturing"] for frame in refused}, {"0"})
        self.assertIn("6", [frame["start_key"] for frame in frames])
        self.assertEqual(frames[-1]["start_key"], "40", "Reset to defaults resets the keys")
        self.assertEqual(frames[-1]["start_binding"], frames[0]["start_binding"])
        self.assertEqual(frames[-1]["view"], "controller")

    def test_next_ui_settings_opens_controls_and_esc_returns(self) -> None:
        """Settings > Controls opens the next UI's controller page; Esc comes back.

        Right from Scaling, the last GAME row, is Controls beside it: a settings
        row keeps Left and Right in its row (it reached the button row before)."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 4 +
            ("KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(run)],
                         ["settings", "controller", "settings"])
        self.assertEqual(self.focus_trail(run)[-4:],
                         [focus["scaling"], focus["controller"], self.CONTROLLER_FOCUS["select"],
                          focus["controller"]])
        pacing = self.next_ui_run("settings", ("WAIT_MS=100", "KEY_DOWN", "KEY_RIGHT"))
        self.assertEqual(self.focus_trail(pacing)[-1], focus["launcher_fullscreen"],
                         "Right from Frame pacing is Launcher fullscreen (F11) beside it, not "
                         "the top bar")
        mouse = self.next_ui_run(
            "settings", ("WAIT_MS=100", "MOUSE_MOVE=1010,392", "MOUSE_DOWN", "MOUSE_UP",
                         "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(mouse)], ["settings", "controller"])

    def test_next_ui_developer_log_buttons(self) -> None:
        """The developer log opens on Close; Copy keeps it open."""
        closed = self.next_ui_run("developer-log", ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"))
        self.assertEqual(self.focus_trail(closed)[0], str(0x4703))
        self.assertEqual([view for view, _ in self.screens(closed)], ["developer_log", "library"])
        copied = self.next_ui_run(
            "developer-log", ("WAIT_MS=100", "KEY_LEFT", "KEY_LEFT", "KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual(self.focus_trail(copied)[-1], str(0x4701))
        self.assertEqual([view for view, _ in self.screens(copied)], ["developer_log"])

    def test_next_ui_title_detail_and_mapping_follow_the_chosen_title(self) -> None:
        """The second card's Title detail, and the game's own mapping chosen from
        there, are the second title's (#1301: a frame without an action no
        longer reselects the first title)."""
        run = self.next_ui_run(
            "library",
            ("WAIT_MS=100", "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=200",
             "MOUSE_MOVE=483,31", "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=200",
             "MOUSE_MOVE=1010,392", "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=200") +
            self.TO_CALIBRATE + ("KEY_DOWN", "KEY_LEFT", "KEY_RETURN", "WAIT_MS=200"),
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        title = [frame for frame in frames if frame["next_route"] == "2"]
        self.assertTrue(title, "the Title detail page opened")
        self.assertEqual({frame["selected_disc"] for frame in title}, {"TEST00006"})
        self.assertEqual([view for view, _ in self.screens(run)][-1], "controller")
        self.assertEqual(frames[-1]["input_scope"], "TEST00006")

    def test_build_package_waits_for_title_preflight(self) -> None:
        """#1286: the title page must catch build refusals before BUILD PACKAGE."""
        for fixture in ("encrypted", "missing-manifest"):
            with self.subTest(fixture=fixture):
                run = self.run_player(
                    f"build-preflight-{fixture}",
                    ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=200"),
                    build_preflight=fixture,
                    ui_mode="next",
                )
                if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                    self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
                frames = run["frames"]
                assert isinstance(frames, list)
                title_frames = [frame for frame in frames if frame["next_route"] == "2"]
                self.assertTrue(title_frames, run["stdout"])
                self.assertEqual(
                    title_frames[-1]["next_primary_label"],
                    "disabled:BUILD_PACKAGE",
                    run["stdout"],
                )
                self.assertEqual(title_frames[-1]["package_building"], "0")
                self.assertEqual(
                    title_frames[-1]["next_preflight_action"], "OPEN_FOLDER"
                )
                if fixture == "encrypted":
                    self.assertIn("decrypted", title_frames[-1]["next_preflight"].lower())
                else:
                    self.assertIn("title_settings", title_frames[-1]["next_preflight"].lower())

    def test_build_package_waits_for_preflight_in_legacy_renderer(self) -> None:
        """#1286: the legacy Library disables the same refused package inputs."""
        for fixture in ("encrypted", "missing-manifest"):
            with self.subTest(fixture=fixture):
                run = self.run_player(
                    f"build-preflight-{fixture}",
                    ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=200"),
                    build_preflight=fixture,
                    ui_mode="legacy",
                )
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertTrue(frames, run["stdout"])
                self.assertEqual(frames[-1]["package_building"], "0", run["stdout"])
                self.assertEqual(frames[-1]["error"], "NONE", run["stdout"])
                self.assertEqual(
                    frames[-1]["next_preflight_action"], "OPEN_FOLDER", run["stdout"]
                )
                if fixture == "encrypted":
                    self.assertEqual(frames[-1]["next_preflight_code"], "EXECUTABLE")
                    self.assertIn("ENCRYPTED_EXECUTABLE", frames[-1]["next_preflight_detail"])
                    self.assertIn("decrypted", frames[-1]["next_preflight"].lower())
                else:
                    self.assertEqual(frames[-1]["next_preflight_code"], "TITLE_PROFILE")
                    self.assertIn("TITLE_PROFILE_", frames[-1]["next_preflight_detail"])
                    self.assertIn("title_settings", frames[-1]["next_preflight"].lower())

    def test_library_keyboard_gamepad_navigation_and_picker(self) -> None:
        run = self.run_player("library", ("KEY_RIGHT", "PAD_DPAD_RIGHT", "KEY_TAB", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual([frames[i]["selected"] for i in range(1, 3)], ["1", "1"])
        self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])
        self.assertEqual(frames[3]["focus"], "1")
        self.assertEqual(frames[4]["picker"], "1")

    def update_run(self, view: str, events: tuple[str, ...], fixture: dict[str, bool] | None,
                   **kwargs: object) -> tuple[dict[str, object], list[dict[str, str]], list[str]]:
        """A next-UI run whose update answers come from a synthetic fixture (None:
        an empty folder, so every request is offline)."""
        with tempfile.TemporaryDirectory(prefix=".update-fixture-", dir=ROOT) as tmp:
            folder = Path(tmp)
            if fixture is not None:
                write_update_fixture(folder, **fixture)
            env = dict(kwargs.pop("env_extra", None) or {})  # type: ignore[arg-type]
            env["NK_UI_TEST_UPDATE_DIR"] = str(folder)
            run = self.next_ui_run(view, events, env_extra=env, **kwargs)
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        steps, requests = update_report(stdout)
        for url in requests:
            self.assertTrue(
                url == UPDATE_RELEASES_URL or url.startswith(UPDATE_DOWNLOAD_PREFIX),
                f"an update request left this repository's releases: {url}")
        return run, steps, requests

    def test_update_check_finds_downloads_and_installs_a_newer_release(self) -> None:
        """Owner request 2026-10-28: CHECK FOR UPDATES asks GitHub once; Download and
        Install each need their own click; the ZIP is checked against the published
        SHA-256 before Install is offered, and Install unpacks into a new folder."""
        run, steps, requests = self.update_run(
            "updates",
            ("WAIT_MS=200", "KEY_RETURN", "WAIT_MS=800", "KEY_RETURN", "WAIT_MS=1500",
             "KEY_RETURN", "WAIT_MS=5000"),
            {},
        )
        names = [step["update"] for step in steps]
        self.assertEqual(steps[0]["newest"], "v0.0.3")
        self.assertIn("available", names)
        self.assertEqual(requests[0], UPDATE_RELEASES_URL)
        if sys.platform != "win32":
            # No native download or SHA-256 check off Windows: the release page only.
            self.assertEqual(names, ["available"], steps)
            self.assertEqual(len(requests), 1)
            self.assertIn("open_url=https://github.com/Jstar269/nakagawa-recomp/releases/tag/v0.0.3",
                          run["stdout"])
            return
        self.assertEqual(names, ["available", "downloaded", "installed"], steps)
        self.assertEqual(
            requests[1:],
            [UPDATE_DOWNLOAD_PREFIX + "v0.0.3/nakagawa-recomp-0.0.3-windows-x64.zip.sha256",
             UPDATE_DOWNLOAD_PREFIX + "v0.0.3/nakagawa-recomp-0.0.3-windows-x64.zip"],
            "the checksum file first, then the ZIP, each once")
        installed = steps[-1]
        self.assertTrue(installed["zip"].endswith(
            os.sep.join(("Nakagawa", "data", "downloads", "nakagawa-recomp-0.0.3-windows-x64.zip"))),
            installed)
        self.assertTrue(installed["install"].endswith("nakagawa-recomp-0.0.3-windows-x64"), installed)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "updates")

    def test_update_check_offline_shows_one_plain_card_and_no_retries(self) -> None:
        run, steps, requests = self.update_run(
            "updates", ("WAIT_MS=200", "KEY_RETURN", "WAIT_MS=1500"), None)
        self.assertEqual([step["update"] for step in steps], ["failed"], steps)
        self.assertEqual(steps[0]["error"], "UPDATE_OFFLINE")
        self.assertEqual(requests, [UPDATE_RELEASES_URL], "one request, no retry")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "updates")

    def test_update_download_that_does_not_match_its_checksum_is_refused(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the update download runs on Windows only")
        _, steps, _ = self.update_run(
            "updates", ("WAIT_MS=200", "KEY_RETURN", "WAIT_MS=800", "KEY_RETURN", "WAIT_MS=1500"),
            {"wrong_checksum": True})
        self.assertEqual(steps[-1]["update"], "failed", steps)
        self.assertEqual(steps[-1]["error"], "UPDATE_HASH_MISMATCH")
        self.assertEqual(steps[-1]["zip"], "none", "nothing is kept or offered to install")

    def test_update_install_that_loses_a_listed_file_is_refused(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the update install runs on Windows only")
        _, steps, _ = self.update_run(
            "updates",
            ("WAIT_MS=200", "KEY_RETURN", "WAIT_MS=800", "KEY_RETURN", "WAIT_MS=1500",
             "KEY_RETURN", "WAIT_MS=5000"),
            {"missing_file": True})
        self.assertEqual(steps[-1]["update"], "failed", steps)
        self.assertEqual(steps[-1]["error"], "UPDATE_INSTALL_INCOMPLETE")
        self.assertEqual(steps[-1]["install"], "none")

    def test_update_check_at_start_is_off_by_default_and_quiet_when_offline(self) -> None:
        """No setting, no request: the player is offline by default (AGENTS.md section 5)."""
        run, steps, requests = self.update_run("library", ("WAIT_MS=800",), {})
        self.assertEqual((steps, requests), ([], []))
        settings = run["settings"]
        if isinstance(settings, dict):
            self.assertIs(settings.get("check_updates_at_start"), False)
        # Turned on: one request at start, and a newer version is one quiet button.
        run, steps, requests = self.update_run(
            "library", ("WAIT_MS=800",), {}, env_extra={"NK_UI_TEST_CHECK_AT_START": "1"})
        self.assertEqual(requests, [UPDATE_RELEASES_URL])
        self.assertEqual([(step["update"], step["notice"]) for step in steps],
                         [("available", "1")], steps)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library", "the check never takes the screen")
        # Offline at start: one request, and nothing on screen.
        run, steps, requests = self.update_run(
            "library", ("WAIT_MS=1500",), None, env_extra={"NK_UI_TEST_CHECK_AT_START": "1"})
        self.assertEqual(requests, [UPDATE_RELEASES_URL])
        self.assertEqual(steps, [], "a failed check at start stays in the developer log")

    def test_update_page_switch_saves_and_escape_returns_to_settings(self) -> None:
        run, steps, requests = self.update_run(
            "updates", ("WAIT_MS=200", "KEY_UP", "KEY_RETURN", "WAIT_MS=200", "KEY_ESCAPE",
                        "WAIT_MS=200"), {})
        self.assertEqual((steps, requests), ([], []), "the switch asks GitHub nothing")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("check_updates_at_start"), True)
        self.assertEqual([view for view, _ in self.screens(run)], ["updates", "settings"])

    def test_settings_screen_returns_to_library(self) -> None:
        run = self.run_player("settings", ("KEY_TAB", "KEY_ESCAPE"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "settings")
        self.assertEqual(frames[1]["focus"], "1")
        self.assertEqual(frames[2]["view"], "library")
        self.assertNotEqual(frames[0]["pixels"], frames[2]["pixels"])

    def test_settings_presentation_choice_saves_with_native_default_schema(self) -> None:
        events = ("KEY_TAB",) * 4 + ("KEY_RETURN",) + ("KEY_TAB",) * 11 + ("KEY_RETURN",)
        run = self.run_player("settings", events)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["focus_count"], "18")
        self.assertEqual(frames[4]["focus"], "4")
        self.assertEqual(frames[5]["focus"], "4")
        self.assertNotEqual(frames[4]["pixels"], frames[5]["pixels"])
        self.assertEqual(frames[-1]["view"], "library")
        settings = run["settings"]
        self.assertIsInstance(settings, dict)
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("schema_version"), 2)
        self.assertEqual(settings.get("fps_cap"), 0)
        self.assertIs(settings.get("ui_sounds"), False, "launcher sounds are off by default")
        renderer_source = (ROOT / "src" / "player" / "ui_renderer.c").read_text(
            encoding="utf-8"
        )
        self.assertIn('case -1: return "PSP TIMING";', renderer_source)
        self.assertIn('case 0:  return "NO EXTRA CAP";', renderer_source)
        self.assertNotIn("30 FPS CAP", renderer_source)
        self.assertNotIn("60 FPS CAP", renderer_source)

    def test_settings_scaling_choice_cycles_and_saves_by_name(self) -> None:
        """Game-frame scaling (#1129) is the eighth stop and is saved as its name."""
        events = ("KEY_TAB",) * 7 + ("KEY_RETURN",) + ("KEY_TAB",) * 8 + ("KEY_RETURN",)
        run = self.run_player("settings", events)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[7]["focus"], "7")
        self.assertEqual(frames[8]["focus"], "7")
        self.assertNotEqual(frames[7]["pixels"], frames[8]["pixels"])
        self.assertEqual(frames[-1]["view"], "library")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("present_scaling"), "integer")

    def test_settings_ui_sounds_toggle_is_the_last_stop_and_saves(self) -> None:
        """The launcher sounds switch is focused after every older control, so their stops keep their places."""
        run = self.run_player("settings", ("KEY_TAB",) * 17 + ("KEY_RETURN",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["focus_count"], "18")
        self.assertEqual(frames[17]["focus"], "17")
        self.assertNotEqual(frames[17]["pixels"], frames[18]["pixels"])
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("ui_sounds"), True)

    def test_settings_about_opens_the_pinned_license_list(self) -> None:
        run = self.run_player("settings", ("KEY_TAB",) * 13 + ("KEY_RETURN",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["focus_count"], "18")
        self.assertEqual(frames[13]["focus"], "13")
        self.assertEqual(frames[-1]["view"], "prereq_about")

    def test_settings_removal_requires_confirmation_and_keeps_other_app_data(self) -> None:
        # Settings > BUILD TOOLS AND CACHE, then that page's REMOVE DOWNLOADED BUILD TOOLS.
        events = ("KEY_TAB",) * 14 + ("KEY_RETURN", "KEY_TAB", "KEY_TAB", "KEY_RETURN", "KEY_RETURN")
        run = self.run_player("settings", events, downloaded_tool_payload=True)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[14]["focus"], "14")
        self.assertEqual(frames[15]["view"], "build_cache")
        self.assertEqual(frames[18]["view"], "confirm_remove_tools")
        self.assertEqual(frames[-1]["view"], "settings")
        self.assertFalse(run["downloaded_tool_marker_exists"])

    def test_settings_layout_avoids_launcher_save_overlap_at_client_widths(self) -> None:
        cases = (
            (1044, 900, "1"),
            (1187, 900, "1"),
            (1043, 900, "0"),
            (800, 720, "0"),
            (1188, 619, "0"),
            (1188, 620, "1"),
        )
        for width, height, expected_two_col in cases:
            with self.subTest(width=width, height=height):
                run = self.run_player("settings", width=width, height=height)
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["settings_two_col"], expected_two_col)

    def test_settings_text_is_drawn_with_the_font_it_was_laid_out_with(self) -> None:
        """Long settings text (the VBlank note) never switches to the bitmap font.

        Wrapping is measured with the TTF font. A string longer than the glyph
        cache used to fall back to the wider bitmap font, so the note ran past
        the panel's right edge. No frame may draw text that way while TTF is on."""
        run = self.run_player("settings", width=1280, height=720)
        frames = run["frames"]
        assert isinstance(frames, list)
        ttf_frames = [frame for frame in frames if frame["font"] == "ttf"]
        if not ttf_frames:
            self.skipTest("the system TTF font is not available in this environment")
        for frame in ttf_frames:
            self.assertEqual(frame["bitmap_text_draws"], "0", run["stdout"])

    def test_script_can_wait_for_a_view_and_a_minimum_duration(self) -> None:
        started = time.monotonic()
        run = self.run_player(
            "settings",
            (
                "KEY_ESCAPE",
                "WAIT_VIEW=library|settings,1000",
                "WAIT_MS=100",
                "ASSERT_VIEW=library",
                "KEY_S",
            ),
        )
        elapsed = time.monotonic() - started
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(elapsed, 0.08)
        self.assertIn("[PLAYER_UI_TEST] wait_view actual=library result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] wait_ms result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] assert_view result=PASS actual=library", run["stdout"])
        self.assertEqual(frames[-1]["view"], "settings")

    def test_mouse_click_adds_disc_through_sdl_event_loop(self) -> None:
        run = self.run_player(
            "library", ("MOUSE_MOVE=400,360", "MOUSE_DOWN", "MOUSE_UP")
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[2]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[2]["pixels"])

    def test_library_card_states_include_experimental_and_ready(self) -> None:
        # The ordinary library auto-discovers built showcase packages beside
        # the test executable. Use the in-memory staged-but-unbuilt card so the
        # missing-runtime state is stable whether or not `make showcase` ran.
        missing = self.run_player("ready-library", (), runtime_ready=False)
        missing_frame = missing["frames"][0]  # type: ignore[index]
        self.assertEqual(missing_frame["selected_prepared"], "0")
        self.assertEqual(missing_frame["selected_package_status"], "1")

        experimental = self.run_player("experimental-library", ())
        experimental_frame = experimental["frames"][0]  # type: ignore[index]
        self.assertEqual(experimental_frame["selected_experimental"], "1")

        ready = self.run_player(
            "ready", ("KEY_LEFT",) * 4, runtime_ready=True, wait_background=True
        )
        ready_frames = ready["frames"]
        assert isinstance(ready_frames, list)
        ready_frame = ready_frames[-1]
        self.assertEqual(ready_frame["selected_runtime"], "1")
        self.assertEqual(ready_frame["selected_prepared"], "0")
        self.assertNotEqual(missing_frame["pixels"], ready_frame["pixels"])

    def test_valid_package_with_missing_data_root_is_not_marked_ready(self) -> None:
        run = self.run_player(
            "ready", (), post_build_ready=True, data_root_present=False,
            wait_background=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        ready_package_frames = [
            frame for frame in frames if frame["selected_package_status"] == "0"
        ]
        self.assertTrue(ready_package_frames, run["stdout"])
        self.assertTrue(
            all(frame["selected_runtime"] == "0" for frame in ready_package_frames),
            run["stdout"],
        )
        self.assertTrue(
            all(frame["selected_prepared"] == "0" for frame in ready_package_frames),
            run["stdout"],
        )

    def test_refused_local_title_profile_is_shown_on_the_game_card(self) -> None:
        normal = self.run_player("supported")
        refused = self.run_player("supported", profile_refusal=True)
        normal_frames = normal["frames"]
        refused_frames = refused["frames"]
        assert isinstance(normal_frames, list) and isinstance(refused_frames, list)
        self.assertEqual(refused_frames[0]["view"], "supported")
        self.assertIn("vblank_frame_counter_addr", refused["stderr"])
        self.assertNotEqual(normal_frames[0]["pixels"], refused_frames[0]["pixels"])

    def test_one_title_offline_art_and_stale_package_stay_off_frame_path(self) -> None:
        # The counters are the load-bearing gate: package validation and ISO
        # reads must never run on the UI thread, on any frame.
        # `package_validations` counts the UI-thread wrapper, so it is a proxy
        # for the whole validation path: a regression that inlined the core
        # call on the frame path would still leave it at zero, and only a
        # sanitizer or an interactive profile would see that. The wall-clock
        # bounds below are deliberately generous because the headless
        # software renderer, not the product's GPU path, sets the absolute
        # per-frame floor here; they only reject the maintainer-reported shape
        # (a first frame of seconds, steady frames of hundreds of ms).
        events = ("KEY_LEFT",) * 24
        offline = self.run_player(
            "ready",
            events,
            art_iso="unavailable",
            stale_package=True,
            wait_background=True,
        )
        frames = offline["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(len(frames), 20)
        validation_calls = [int(frame["package_validations"]) for frame in frames]
        self.assertEqual(validation_calls, [0] * len(frames))
        elapsed = [int(frame["render_ns"]) for frame in frames]
        self.assertLess(elapsed[0], 1_000_000_000)
        self.assertLess(max(elapsed[1:]), 250_000_000)
        self.assertLess(sorted(elapsed[1:])[len(elapsed[1:]) // 2], 60_000_000)
        # Coarse end-to-end guard over process start through the last frame:
        # the maintainer-reported 3 fps interactive loop would need ~9s here.
        self.assertLess(offline["wall_seconds"], 6.0)
        self.assertEqual(frames[-1]["selected_package_status"], "3")
        self.assertEqual(frames[-1]["selected_runtime"], "0")
        # An absent disc image must show the initials placeholder, never a
        # blank or a blocked frame waiting for the drive.
        self.assertFalse(has_icon_art(offline["bmp"]))

        available = self.run_player(
            "ready",
            ("KEY_LEFT",) * 8,
            art_iso="available",
            stale_package=True,
            wait_background=True,
        )
        art_frames = available["frames"]
        assert isinstance(art_frames, list)
        self.assertEqual(
            [int(frame["package_validations"]) for frame in art_frames],
            [0] * len(art_frames),
        )
        self.assertEqual(art_frames[-1]["selected_package_status"], "3")
        # Synthetic icon art arrives without blocking the UI frame.
        self.assertTrue(has_icon_art(available["bmp"]))

    # An upgrade can leave a built package the validator no longer accepts. The
    # card then offers REBUILD PACKAGE (#1147) with the same consent, progress
    # and restore as a first build, and says in one line why.
    INCOMPATIBLE_REASON = "Built_for_another_player_version,_or_its_package_file_is_unreadable."

    def assert_rebuild_started(self, frames: list, stdout: object) -> None:
        # The dead pill left the view on the library. REBUILD PACKAGE takes the
        # first-build path, which this host answers with the build screen, the
        # build tools consent, or (no build tools on Linux) the error card.
        self.assertIn(
            frames[-1]["view"],
            ("building_package", "prereq_consent", "prereq_progress", "error"),
            stdout,
        )

    def test_incompatible_card_offers_rebuild_package_in_the_legacy_renderer(self) -> None:
        shown = self.run_player("ready", (), incompatible_package=True, wait_background=True)
        frames = shown["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["selected_package_status"], "2", shown["stdout"])
        self.assertEqual(frames[-1]["incompatible_reason"], self.INCOMPATIBLE_REASON)
        # The button is a focus stop (it was a dead pill, with no stop).
        self.assertEqual(frames[-1]["focus_count"], "4")
        run = self.run_player(
            "ready",
            ("WAIT_PACKAGE_STATUS=INCOMPATIBLE,10000", "KEY_RETURN"),
            incompatible_package=True,
            wait_background=True,
        )
        self.assertIn("[PLAYER_UI_TEST] wait_package_status result=PASS", run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "ready_library")
        self.assertTrue(
            any(frame["selected_package_status"] == "2" for frame in frames), run["stdout"]
        )
        self.assert_rebuild_started(frames, run["stdout"])

    def test_incompatible_title_offers_rebuild_package_in_the_next_ui(self) -> None:
        shown = self.run_player(
            "ready", ("WAIT_MS=100",), incompatible_package=True, wait_background=True,
            ui_mode="next",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in shown["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = shown["frames"]
        assert isinstance(frames, list)
        last = frames[-1]
        self.assertEqual(last["selected_package_status"], "2", shown["stdout"])
        self.assertEqual(last["next_primary_label"], "REBUILD_PACKAGE")
        self.assertEqual(last["next_detail"], self.INCOMPATIBLE_REASON)
        run = self.run_player(
            "ready",
            # Open the title: its primary action has focus (launcher review B6).
            ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300"),
            incompatible_package=True, wait_background=True, ui_mode="next",
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assert_rebuild_started(frames, run["stdout"])

    def test_package_status_worker_and_catalog_reload_do_not_deadlock_or_corrupt_status(
        self,
    ) -> None:
        """Two threads share the title catalog: the worker validates a package
        while the UI thread reloads the overlay 128 times. The load-bearing
        claims are that both sides finish and that the status the worker read
        is the one it stored; a data race between them is only visible to a
        sanitizer, which hosted CI does not run."""
        run = self.run_player(
            "ready",
            wait_background=True,
            catalog_reload_count=128,
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] catalog_reload result=PASS "
            r"reloads=128 status_checks=128 status=1",
        )

    def test_exit_path_joins_iso_art_workers_and_tears_down_textures(self) -> None:
        """ui_font_shutdown() is the only place the ISO-art workers are joined
        and the only font/texture teardown. Every exit has to reach it, so the
        launcher never destroys the SDL queue or renderer while a worker is
        still inside it."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_DELAY_ACTIVE", "QUIT"),
            art_iso="available",
            env_extra={"PLAYER_UI_TEST_ART_DELAY_MS": "5000"},
        )
        self.assertIn(
            "[PLAYER_UI_TEST] wait_art_delay_active result=PASS", run["stdout"]
        )
        shutdown = re.findall(
            r"\[PLAYER_UI_TEST\] font_shutdown "
            r"art_jobs_active_at_shutdown=(\d+) art_jobs_drained=(\d+)",
            run["stdout"],
        )
        self.assertEqual(len(shutdown), 1, run["stdout"])
        self.assertEqual(shutdown[0], ("1", "1"), run["stdout"])

    def test_iso_art_failures_reach_a_terminal_state_after_bounded_retries(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="unavailable",
            env_extra={"PLAYER_UI_TEST_NO_PNG_DECODER": "1"},
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        # A missing image remains retryable, then ends specifically as absent;
        # it is not the same result as corrupt bytes or an unsupported decoder.
        self.assertEqual(attempts[-1][5:], ("absent", "absent"), run["stdout"])

    def test_corrupt_art_is_failed_after_bounded_retries(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="corrupt",
            wait_background=True,
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        self.assertEqual(attempts[-1][5:], ("failed", "loaded"), run["stdout"])

    def test_readable_art_without_png_decoder_is_unsupported(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "WAIT_MS=2500"),
            art_iso="available",
            wait_background=True,
            env_extra={"PLAYER_UI_TEST_NO_PNG_DECODER": "1"},
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 1, run["stdout"])
        self.assertEqual(
            attempts[0][2:],
            ("0", "0", "3", "unsupported", "unsupported"),
            run["stdout"],
        )
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_decoder_rejection_is_failed_after_bounded_retries(self) -> None:
        """A decoder rejection is a failed image, not a build capability result."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="undecodable",
            wait_background=True,
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        self.assertEqual(
            attempts[0][2:],
            ("0", "1", "3", "pending", "loaded"),
            run["stdout"],
        )
        self.assertEqual(attempts[-1][5:], ("failed", "loaded"), run["stdout"])
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_iso_art_retry_rereads_only_the_image_that_is_still_missing(self) -> None:
        """A disc with ICON0.PNG and no PIC1.PNG must not re-read its icon
        (up to 1 MiB) on every retry: the attempt after the first asks only for
        the picture it is still missing."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "WAIT_MS=1500"),
            art_iso="icon-only",
            wait_background=True,
        )
        self.assertTrue(has_icon_art(run["bmp"]), run["stdout"])
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d)",
            run["stdout"],
        )
        self.assertGreaterEqual(len(attempts), 2, run["stdout"])
        self.assertEqual(attempts[0][2:], ("1", "0", "3"))
        for attempt in attempts[1:]:
            # 2 == NK_ISO_ART_PICTURE: the loaded icon is not read again.
            self.assertEqual(attempt[2:], ("1", "0", "2"), run["stdout"])

    def test_every_queued_title_claims_a_check_before_its_first_result(self) -> None:
        """A title whose package status is not known yet must be claimed as
        checking. Leaving it unclaimed shows "BUILD PACKAGE" for a title that
        may already be prepared."""
        run = self.run_player("library")
        frame = run["frames"][0]  # type: ignore[index]
        titles = int(frame["games"])
        self.assertGreater(titles, 1, run["stdout"])
        self.assertEqual(frame["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frame["titles_pending"], str(titles), run["stdout"])

    def test_lost_worker_events_are_reclaimed_instead_of_stalling_the_card(self) -> None:
        """A worker whose completion event never reaches the queue must not
        leave its card pending for the session. With every push refused the
        launcher still finishes both jobs, so the harness reaches its quit
        instead of waiting on background work that can never finish."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=1200",),
            art_iso="available",
            stale_package=True,
            wait_background=True,
            env_extra={"NK_UI_TEST_DROP_WORKER_EVENT": "1"},
        )
        self.assertIn("ISO art for", run["stderr"])
        self.assertIn("Package status for", run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["running"], "0")
        # The package job was applied from the worker result, so no title is
        # left claiming a check that will never finish.
        self.assertEqual(frames[-1]["titles_pending"], "0", run["stdout"])
        self.assertEqual(frames[-1]["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "3", run["stdout"])
        # The art job was reclaimed, so the icon is retried rather than lost.
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_package_status_thread_creation_failure_is_unresolved_and_retryable(
        self,
    ) -> None:
        """A failed worker start stays unresolved until the user retries.

        The 2-second library rescan must not turn one create failure into a
        repeating thread-creation loop. The retry button then enters the same
        production queue and lets the real validator establish MISSING.
        """
        run = self.run_player(
            "ready",
            events=("WAIT_MS=2200", "KEY_RETURN", "WAIT_MS=100"),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["selected_package_status"] == "4"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["package_check_failed_badge"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["titles_failed"] == "1" and
                            frame["titles_pending"] == "0" and
                            frame["titles_unclaimed"] == "0"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(int(frame["ticks_ms"]) >= 2000 and
                            frame["package_status_thread_attempts"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            for frame in frames), run["stdout"])
        self.assertEqual(frames[-1]["package_status_thread_attempts"], "2")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["titles_failed"], "0")
        self.assertEqual(frames[-1]["titles_pending"], "0")
        self.assertEqual(frames[-1]["titles_unclaimed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")
        self.assertEqual(frames[-1]["package_building"], "0")

    def test_failed_post_build_worker_start_survives_cache_invalidation_and_retries(self) -> None:
        run = self.run_player(
            "ready",
            events=(
                "WAIT_VIEW=library,5000",
                "INVALIDATE_PACKAGE_CACHE",
                "KEY_RETURN",
                "WAIT_MS=100",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
            ),
            wait_background=True,
            build_ready_test=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=0", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=ready_library",
            run["stdout"],
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] invalidate_package_cache generation=\d+",
        )
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["package_check_failed_badge"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            and frame["view"] == "library"
                            for frame in failed_frames), run["stdout"])
        self.assertEqual(frames[-1]["view"], "ready_library")
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3
        )

    def test_worker_start_failure_automatically_retries_after_bounded_backoff(self) -> None:
        run = self.run_player(
            "ready",
            events=("WAIT_MS=6200",),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["package_status_thread_attempts"] == "1" and
                int(frame["ticks_ms"]) < 5000
                for frame in frames),
            run["stdout"],
        )
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 2,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")

    def test_completed_retry_clears_failure_when_cache_generation_changes(self) -> None:
        run = self.run_player(
            "ready",
            events=(
                "WAIT_MS=100",
                "KEY_RETURN",
                "WAIT_MS=100",
                "INVALIDATE_PACKAGE_CACHE",
                "WAIT_MS=1700",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1",
                "NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "600",
            },
        )
        self.assertIn("[PLAYER_UI_TEST] invalidate_package_cache", run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0",
                         run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "1",
                         run["stdout"])

    def test_closing_during_the_startup_package_check_exits_promptly(self) -> None:
        """Launcher review B5: closing does not wait for the package check.

        NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS stands in for a check that
        hashes a large package (about 4 s for each showcase demo). The run quits
        300 ms in, while that check runs. Failing-before: shutdown joined the
        check's thread, so the window stayed up until the check ended (up to
        about 25 s with three titles); now the check stops at its next read."""
        run = self.run_player(
            "ready", ("WAIT_MS=300",), ui_mode="next",
            env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "5000"},
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(int(frames[-1]["package_status_thread_attempts"]), 1,
                                run["stdout"])
        wall = run["wall_seconds"]
        assert isinstance(wall, float)
        shutdown = wall - int(frames[-1]["ticks_ms"]) / 1000.0
        self.assertLess(shutdown, 2.5,
                        f"closing waited {shutdown:.1f} s for the package check")

    def test_library_card_says_checking_until_the_package_check_ends(self) -> None:
        """Launcher review B4: a card reads CHECKING, not NEEDS SETUP, while its check runs.

        Failing-before: the showcase demos read NEEDS SETUP for the first 4-9 s of
        every start, until their checks finished, and then flipped to READY."""
        run = self.run_player(
            "ready", ("WAIT_MS=200",), wait_background=True, ui_mode="next",
            env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "1500"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        statuses = [
            line.split("status=", 1)[1]
            for line in str(run["stdout"]).splitlines()
            if line.startswith("[PLAYER_UI_TEST] next_ui_library_status index=0 ")
        ]
        self.assertTrue(statuses, run["stdout"])
        self.assertEqual(statuses[0], "Checking", statuses)
        # The check then decides: this synthetic title has no package to run.
        self.assertGreaterEqual(len(statuses), 2, statuses)
        self.assertNotEqual(statuses[-1], "Checking", statuses)

    def test_failed_post_build_check_does_not_suppress_later_title_build(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=1 disc=TEST00006",
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=0 disc=TEST00005",
            run["stdout"],
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=1", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_failed_post_build_check_same_disc_cache_epoch_is_not_stale(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "INVALIDATE_AND_START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        action_line = next(
            line for line in run["stdout"].splitlines()
            if line.startswith(
                "[PLAYER_UI_TEST] invalidate_cache_and_synthetic_build"
            )
        )
        action_match = re.search(
            r"result=PASS index=1 disc=TEST00006 generation=(\d+) "
            r"build_session_generation=(\d+)$",
            action_line,
        )
        self.assertIsNotNone(action_match, run["stdout"])
        assert action_match is not None
        self.assertGreater(int(action_match.group(1)), 0)
        self.assertGreater(int(action_match.group(2)), 0)
        post_match = re.search(
            r"\[PLAYER_UI_TEST\] post_build_validation status=1 .*?"
            r"build_session_generation=(\d+) "
            r"current_build_session_generation=(\d+)",
            run["stdout"],
        )
        self.assertIsNotNone(post_match, run["stdout"])
        assert post_match is not None
        self.assertEqual(post_match.group(1), post_match.group(2), run["stdout"])
        self.assertEqual(post_match.group(1), action_match.group(2), run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_worker_start_failure_does_not_block_other_title_builds(self) -> None:
        """A's worker-start failure cannot strand the post-build latch for B.

        A fails before either B build starts, and B completes both builds. The
        failure selector is title-keyed so this does not depend on worker
        scheduling or a global create-attempt ordinal.
        """
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR": "TEST00005"
            },
        )
        post_build_lines = [
            line for line in run["stdout"].splitlines()
            if "post_build_transition" in line and "disc=TEST00006" in line
        ]
        self.assertEqual(len(post_build_lines), 2, run["stdout"])
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(len(unresolved), 1, run["stderr"])
        self.assertTrue(all("TEST00005" in line for line in unresolved), run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00005" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "0")

    def test_repeated_post_build_failures_keep_prior_title_retryable(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_PACKAGE_FAILURE=TEST00006,1",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                "KEY_RIGHT",
                "KEY_RETURN",
                "WAIT_PACKAGE_FAILURE=TEST00006,2",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                # B fails on its post-build check, A fails on the next build,
                # and B fails again on the explicit retry. The following
                # repeated failures must stay attributed to their titles.
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR":
                    "TEST00005,TEST00006"
            },
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        self.assertIn(
            "Package status for TEST00005 is unresolved", run["stderr"]
        )
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(
            sum("TEST00005" in line for line in unresolved), 2, run["stderr"]
        )
        self.assertGreaterEqual(
            sum("TEST00006" in line for line in unresolved), 2, run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00006" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "1")
        self.assertEqual(frames[-1]["selected_package_status"], "4")
        self.assertEqual(frames[-1]["titles_pending"], "0")

    def test_texture_cache_saturation_with_in_flight_jobs_does_not_double_free_or_format_percent_tokens(
        self,
    ) -> None:
        """When all 64 texture cache slots have in-flight art jobs, a new
        request must return no slot rather than evicting and freeing a job whose
        completion event is already in the SDL event queue. Evicting the in-flight
        job would cause a use-after-free and double-free when its completion
        event is subsequently delivered. The caller-owned path contains literal
        %s/%p tokens to catch its accidental use as a printf format string."""
        run = self.run_player(
            "ready",
            ("SATURATE_ART_CACHE",),
            art_iso="saturated",
        )
        self.assertIn(
            "[PLAYER_UI_TEST] saturate_art_cache result=PASS slots=64 delivered=64 overflow_refused=1",
            run["stdout"],
        )

    def test_async_build_validation_enters_ready_library(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_VIEW=ready_library,10000", "ASSERT_VIEW=ready_library"),
            build_ready_test=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        ready_frames = [frame for frame in frames if frame["view"] == "ready_library"]
        self.assertTrue(ready_frames)
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_validation status=0 reason=Runtime package v2 is valid",
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_transition view=ready_library "
            r"disc=TEST00006 found=1 prepared=1 status=\d+",
        )

    def test_experimental_build_without_selected_executable_ends_ready(self) -> None:
        """#1276: a passing build of a disc saved with no selected executable.

        The build bound EBOOT.BIN in the experimental profile; the check after
        it used the empty library value, refused the new package and showed
        "Package Build Failed". It must end on READY with a playable package
        (runtime=1 is what makes its card offer PLAY)."""
        run = self.run_player(
            "ready",
            ("WAIT_VIEW=ready_library,10000", "ASSERT_VIEW=ready_library"),
            experimental_build_ready=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_validation status=0 "
            r"reason=Runtime package v\d is valid for TEST00009",
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_transition view=ready_library "
            r"disc=TEST00009 found=1 prepared=1 status=\d+ runtime=1",
        )
        self.assertEqual(frames[-1]["view"], "ready_library")
        self.assertFalse(any(frame["view"] == "error" for frame in frames), run["stdout"])

    def test_build_failure_card_dismisses_and_cancel_stays_away(self) -> None:
        """#1277: a finished build's outcome is shown once per session.

        The failure card was re-opened on every frame, so Return to Library
        could not dismiss it, and a cancelled build pulled every later view
        back to the Library."""
        failed = self.run_player(
            "build-failed",
            ("WAIT_VIEW=error,5000", "KEY_RETURN", "WAIT_MS=300", "ASSERT_VIEW=library"),
        )
        self.assertIn("[PLAYER_UI_TEST] assert_view result=PASS actual=library", failed["stdout"])
        failed_frames = failed["frames"]
        assert isinstance(failed_frames, list)
        self.assertEqual(failed_frames[-1]["view"], "library")

        cancelled = self.run_player(
            "building",
            ("KEY_ESCAPE", "WAIT_MS=200", "KEY_S", "WAIT_MS=300", "ASSERT_VIEW=settings"),
            ui_mode="next",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in cancelled["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=settings", cancelled["stdout"]
        )
        cancelled_frames = cancelled["frames"]
        assert isinstance(cancelled_frames, list)
        self.assertEqual(cancelled_frames[-1]["view"], "settings")

    def test_late_mounted_iso_art_retries_after_cooldown(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "MOUNT_ART_ISO", "WAIT_ART_ATTEMPT=2"),
            art_iso="late",
            wait_background=True,
        )
        self.assertTrue(has_icon_art(run["bmp"]))
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 2, run["stdout"])
        self.assertEqual(attempts[0][2:], ("0", "0"))
        self.assertEqual(attempts[1][2:], ("1", "1"))
        retry_delay_ns = int(attempts[1][1]) - int(attempts[0][1])
        self.assertGreaterEqual(retry_delay_ns, 1_000_000_000)
        self.assertLessEqual(retry_delay_ns, 2_500_000_000)

    def test_inspection_and_support_screens_render_and_return(self) -> None:
        for view in ("inspecting", "supported", "experimental", "unsupported", "preparing"):
            with self.subTest(view=view):
                run = self.run_player(view, ("KEY_ESCAPE",))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], view)
                self.assertEqual(frames[-1]["view"], "library")
                self.assertNotEqual(frames[0]["pixels"], frames[-1]["pixels"])

    def test_staged_card_exposes_runtime_required_status(self) -> None:
        run = self.run_player("ready")
        frame = run["frames"][0]  # type: ignore[index]
        self.assertEqual(frame["selected_staged"], "1")
        self.assertEqual(frame["selected_runtime"], "0")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(has_amber_badge(run["bmp"], badge_rect(frames[-1])))

    def test_staging_and_package_build_cancellation_render_progress(self) -> None:
        staging = self.run_player("wizard-staging", ("KEY_ESCAPE",))
        stage_frames = staging["frames"]
        assert isinstance(stage_frames, list)
        self.assertEqual(stage_frames[0]["extracting"], "1")
        self.assertEqual(stage_frames[0]["extraction_percent"], "67")
        self.assertEqual(stage_frames[1]["extraction_cancel"], "1")
        self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in stage_frames[:2]))

        building = self.run_player("building", ("KEY_ESCAPE",))
        build_frames = building["frames"]
        assert isinstance(build_frames, list)
        self.assertEqual(build_frames[0]["view"], "building_package")
        self.assertEqual(build_frames[0]["package_building"], "1")
        self.assertEqual(build_frames[1]["package_cancelled"], "1")
        self.assertEqual(build_frames[1]["view"], "library")

    def test_build_screen_follows_the_build_state(self) -> None:
        """The build screen names the state the session is in.

        A package that has been written reads PACKAGE BUILT while it is checked.
        It used to keep reading BUILDING RUNTIME PACKAGE, because only the stage
        chips looked at completion."""
        running = self.run_player("building", ("KEY_ESCAPE",))
        running_frames = running["frames"]
        assert isinstance(running_frames, list)
        self.assertEqual(running_frames[0]["build_badge"], "BUILDING_RUNTIME_PACKAGE")

        built = self.run_player("build-ready")
        built_frames = built["frames"]
        assert isinstance(built_frames, list)
        self.assertEqual(built_frames[0]["view"], "building_package")
        self.assertEqual(built_frames[0]["build_badge"], "PACKAGE_BUILT", built["stdout"])

    def test_build_bar_shows_the_compiled_share_of_parts(self) -> None:
        """A compile stage that counts its parts fills the bar to that share.

        Without a count the bar sweeps and claims no percentage; a package build
        once sat on that sweep for minutes with no sign of how far it had got.
        The same count carries a calm note that other apps may feel slower, drawn
        under the bar; a build without one draws no note. The next UI's card is
        checked by nk-ui-player-screens-selftest."""
        sweeping = self.run_player("building", ("KEY_ESCAPE",), ui_mode="legacy")
        sweeping_frames = sweeping["frames"]
        assert isinstance(sweeping_frames, list)
        self.assertEqual(sweeping_frames[0]["build_badge"], "BUILDING_RUNTIME_PACKAGE")
        self.assertEqual(sweeping_frames[0]["build_bar_permille"], "-1")
        self.assertEqual(sweeping_frames[0]["build_notice"], "0")

        counted = self.run_player("building-parts", ("KEY_ESCAPE",), ui_mode="legacy")
        counted_frames = counted["frames"]
        assert isinstance(counted_frames, list)
        self.assertEqual(counted_frames[0]["view"], "building_package")
        self.assertEqual(counted_frames[0]["build_bar_permille"], "300", counted["stdout"])
        self.assertEqual(counted_frames[0]["build_notice"], "1", counted["stdout"])
        self.assertEqual(counted_frames[1]["build_bar_permille"], "-1")
        self.assertEqual(counted_frames[1]["build_notice"], "0")

    def test_controller_bind_conflict_calibration_and_profile_save(self) -> None:
        bind = self.run_player("controller", ("KEY_TAB", "KEY_RETURN", "PAD_SOUTH"))
        bind_frames = bind["frames"]
        assert isinstance(bind_frames, list)
        self.assertEqual(bind_frames[2]["controller_capturing"], "1")
        self.assertEqual(bind_frames[3]["controller_capturing"], "0")
        self.assertEqual(bind_frames[3]["controller_conflicts"], "1")
        self.assertNotEqual(bind_frames[0]["start_binding"], bind_frames[3]["start_binding"])

        calibrate = self.run_player(
            "controller", tuple(["KEY_TAB"] * 18 + ["KEY_RETURN", "KEY_ESCAPE"])
        )
        cal_frames = calibrate["frames"]
        assert isinstance(cal_frames, list)
        self.assertEqual(cal_frames[19]["calibrating"], "1")
        self.assertEqual(cal_frames[20]["calibrating"], "0")
        self.assertEqual(cal_frames[20]["view"], "controller")

        invalid = self.run_player("controller", (), invalid_profile=True)
        invalid_frame = invalid["frames"][0]  # type: ignore[index]
        self.assertEqual(invalid_frame["profile_fallback"], "1")

        save = self.run_player("controller", tuple(["KEY_TAB"] * 19 + ["KEY_RETURN"]))
        save_frame = save["frames"][-2]  # type: ignore[index]
        self.assertEqual(save_frame["profile_save_notice"], "1")
        self.assertTrue(save["profile_exists"])

    def test_corrupt_disc_and_error_card_recovery(self) -> None:
        corrupt = self.run_player("library", ("KEY_RETURN",), drop_invalid_iso=True)
        error_frames = corrupt["frames"]
        assert isinstance(error_frames, list)
        self.assertEqual(error_frames[1]["view"], "error")
        self.assertEqual(error_frames[1]["error"], "ISO_CORRUPT")
        self.assertEqual(error_frames[2]["picker"], "1")

        missing_source = self.run_player("error", ("KEY_RETURN",))
        missing_source_frames = missing_source["frames"]
        assert isinstance(missing_source_frames, list)
        self.assertEqual(missing_source_frames[0]["error"], "SOURCE_NOT_FOUND")
        self.assertEqual(missing_source_frames[1]["picker"], "1")

    def test_invalid_disc_in_wizard_is_visible_without_a_selected_iso(self) -> None:
        for ui_mode in (None, "legacy"):
            with self.subTest(ui_mode=ui_mode or "default"):
                baseline = self.run_player("wizard2", ui_mode=ui_mode)
                rejected = self.run_player(
                    "wizard2", drop_invalid_iso=True, ui_mode=ui_mode
                )
                baseline_frames = baseline["frames"]
                rejected_frames = rejected["frames"]
                assert isinstance(baseline_frames, list)
                assert isinstance(rejected_frames, list)
                baseline_frame = baseline_frames[-1]
                rejected_frame = rejected_frames[-1]
                self.assertEqual(rejected_frame["view"], "wizard")
                self.assertEqual(rejected_frame["wizard_step"], "select_game")
                self.assertEqual(rejected_frame["bad_disc_notice"], "1")
                if ui_mode == "legacy":
                    self.assertNotEqual(
                        rejected_frame["pixels"], baseline_frame["pixels"],
                        "the legacy renderer must show the invalid-file sentence",
                    )

        recovery_codes = (
            "EXPERIMENTAL_PROFILE_FAILED",
            "SOURCE_NOT_FOUND",
            "PROCESS_SPAWN_FAILED",
            "STAGED_EXECUTABLE_INVALID",
            "MANIFEST_MISMATCH",
            "RUNTIME_NOT_FOUND",
            "LIBRARY_WRITE_FAILED",
            "RUNTIME_PACKAGE_NOT_READY",
            "PYTHON_NOT_FOUND",
            "CLI_NOT_FOUND",
            "DATA_DIR_UNAVAILABLE",
            "SPAWN_FAILED",
            "PACKAGE_BUILD_FAILED",
            "RUNTIME_PREMATURE_EXIT",
            "RUNTIME_ERROR_EXIT",
        )
        # Both launchers: the next UI applies a control's action after the frame
        # that drew it, so the frame of the press still shows the card and the
        # library is drawn from the next frame; compare a frame after the wait.
        for ui_mode in ("next", "legacy"):
            for code in recovery_codes:
                with self.subTest(code=code, ui_mode=ui_mode):
                    recovered = self.run_player(
                        "library", ("KEY_RETURN", "WAIT_MS=100"), error_code=code,
                        ui_mode=ui_mode,
                    )
                    frames = recovered["frames"]
                    assert isinstance(frames, list)
                    self.assertEqual(frames[0]["view"], "error")
                    self.assertEqual(frames[0]["error"], code)
                    self.assertEqual(frames[1]["view"], "library")
                    self.assertEqual(frames[-1]["view"], "library")
                    self.assertNotEqual(frames[0]["pixels"], frames[-1]["pixels"],
                                        "the library replaces the error card on screen")
                    # Only a missing or corrupt source reopens the file picker.
                    expected_picker = "1" if code in ("ISO_CORRUPT", "SOURCE_NOT_FOUND") else "0"
                    self.assertEqual(frames[1]["picker"], expected_picker)

    def test_pbp_package_error_card_names_the_package_boundary(self) -> None:
        """A store package dropped as an ISO is named as a package, not a broken disc.

        Reading it as ISO9660 only ever said the volume descriptor was missing,
        which points a person at repairing a file that was never a disc. The
        card must carry the package boundary, and its recovery action must still
        reopen the file picker so another file can be chosen.
        """
        package = self.run_player("library", ("KEY_RETURN",), drop_pbp_iso=True)
        frames = package["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[1]["view"], "error")
        self.assertEqual(frames[1]["error"], "PBP_PACKAGE_UNSUPPORTED")
        self.assertEqual(frames[2]["picker"], "1")

    def test_compressed_and_non_psp_disc_images_get_their_own_cards(self) -> None:
        """A .cso is a compressed disc image, and an ISO9660 image without a PSP_GAME folder
        is a disc image of something else: each gets its own plain card, not the generic
        "isn't a PSP disc image" card or an "unknown PSP title" page."""
        # A CISO header (magic, header size, uncompressed size, block size, version).
        cso = b"CISO" + struct.pack("<IQIBB2x", 0x18, 2048 * 32, 2048, 1, 0) + bytes(4096)
        # A valid ISO9660 image whose root holds only a text file.
        sector = 2048
        image = bytearray(sector * 20)

        def both32(value: int) -> bytes:
            return struct.pack("<I", value) + struct.pack(">I", value)

        def record(name: bytes, lba: int, size: int, directory: bool) -> bytes:
            length = 33 + len(name) + (1 - len(name) % 2)
            body = bytearray(length)
            body[0] = length
            body[2:10] = both32(lba)
            body[10:18] = both32(size)
            body[25] = 2 if directory else 0
            body[28:32] = struct.pack("<H", 1) + struct.pack(">H", 1)
            body[32] = len(name)
            body[33:33 + len(name)] = name
            return bytes(body)

        pvd = image[16 * sector:17 * sector]
        pvd[0] = 1
        pvd[1:6] = b"CD001"
        pvd[6] = 1
        pvd[40:72] = b"SYNTHETIC_DATA_DISC".ljust(32)
        pvd[156:156 + 34] = record(b"\x00", 18, sector, True)
        image[16 * sector:17 * sector] = pvd
        image[17 * sector] = 255
        image[17 * sector + 1:17 * sector + 6] = b"CD001"
        root = (
            record(b"\x00", 18, sector, True)
            + record(b"\x01", 18, sector, True)
            + record(b"README.TXT;1", 19, 16, False)
        )
        image[18 * sector:18 * sector + len(root)] = root
        image[19 * sector:19 * sector + 16] = b"synthetic text\n\n"
        cases = (
            ("source-owned-compressed.cso", cso, "DISC_IMAGE_COMPRESSED", "Compressed_Disc_Image"),
            ("source-owned-data-disc.iso", bytes(image), "DISC_NOT_PSP", "Not_a_PSP_Disc_Image"),
        )
        for name, payload, code, title in cases:
            with self.subTest(code=code):
                run = self.run_player("library", ("KEY_RETURN",), drop_bytes=(name, payload))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[1]["view"], "error", run["stdout"])
                self.assertEqual(frames[1]["error"], code)
                self.assertEqual(frames[1]["error_title"], title)
                self.assertEqual(frames[2]["picker"], "1")

    def test_long_error_text_fits_at_minimum_window(self) -> None:
        run = self.run_player("error", error_code="UI_TEST_LONG_ERROR",
                              width=960, height=540)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["error"], "UI_TEST_LONG_ERROR")
        self.assertEqual(frames[0]["error_text_complete"], "1",
                         "the error view clipped a long message")
        self.assertGreater(int(frames[0]["error_text_lines"]), 4)

    def test_cli_not_found_error_text_fits_at_minimum_window(self) -> None:
        run = self.run_player("error", error_code="CLI_NOT_FOUND",
                              width=960, height=540)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["error"], "CLI_NOT_FOUND")
        self.assertEqual(frames[0]["error_text_complete"], "1",
                         "the error view clipped the CLI_NOT_FOUND message")
        self.assertEqual(frames[0]["error_details_complete"], "1")
        self.assertGreater(int(frames[0]["error_details_lines"]), 0)
        self.assertEqual(frames[0]["error_details_available"], "1")

    def test_per_title_controller_mapping_choice(self) -> None:
        """A disc can get its own mapping, or go back to the global one."""
        # The library card carries the choice: three tabs reach it, because the
        # primary action, add and remove come first and paging never applies to
        # a one-title library.
        card = self.run_player("library", ("KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN"))
        card_frames = card["frames"]
        assert isinstance(card_frames, list)
        self.assertEqual(card_frames[0]["input_scope"], "global")
        self.assertEqual(card_frames[0]["input_titles"], "0")
        self.assertEqual(card_frames[4]["input_scope"], "TEST00005")
        self.assertEqual(card_frames[4]["input_titles"], "1")
        self.assertNotEqual(card_frames[0]["pixels"], card_frames[4]["pixels"])

        # The same control in Controller Settings, which says which mapping it
        # is editing, and which is the 23rd focus stop of that view.
        settings = self.run_player(
            "controller", tuple(["KEY_TAB"] * 22 + ["KEY_RETURN"])
        )
        settings_frames = settings["frames"]
        assert isinstance(settings_frames, list)
        self.assertEqual(settings_frames[0]["input_scope"], "global")
        self.assertEqual(settings_frames[0]["focus_count"], "23")
        self.assertEqual(settings_frames[23]["input_scope"], "TEST00005")
        self.assertNotEqual(settings_frames[0]["pixels"], settings_frames[23]["pixels"])

        # Toggling back returns the disc to the global mapping.
        back = self.run_player(
            "controller", tuple(["KEY_TAB"] * 22 + ["KEY_RETURN"] * 2)
        )
        back_frames = back["frames"]
        assert isinstance(back_frames, list)
        self.assertEqual(back_frames[23]["input_scope"], "TEST00005")
        self.assertEqual(back_frames[24]["input_scope"], "global")
        self.assertEqual(back_frames[24]["input_titles"], "0")

    def test_quit_from_library_runs_through_sdl_event_loop(self) -> None:
        run = self.run_player("library", ("KEY_ESCAPE",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library")
        self.assertEqual(frames[-1]["running"], "0")

    # The next UI (--ui=next) reaches every flow the existing library reaches:
    # the setup wizard from an empty library, Settings, the file picker and quit.
    # The screens it hands over to keep drawing in the existing renderer.

    def test_empty_library_starts_the_setup_wizard(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN"), ui_mode="next")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "library")
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")

    def test_library_shortcuts_reach_settings_picker_and_quit(self) -> None:
        settings = self.run_player("library", ("KEY_S",), ui_mode="next")
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in settings["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = settings["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "settings")
        picker = self.run_player("library", ("KEY_O",), ui_mode="next")
        frames = picker["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1")
        quit_run = self.run_player("library", ("KEY_ESCAPE",), ui_mode="next")
        frames = quit_run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["running"], "0")

    # The next UI draws the setup wizard too, with the existing wizard's handlers.
    def test_next_ui_wizard_options_step_switches_and_skip(self) -> None:
        # The switches sit above the buttons: Up from Continue reaches Developer
        # mode, Up again Online mode.
        run = self.run_player(
            "wizard-options",
            ("WAIT_MS=100", "KEY_UP", "KEY_UP", "KEY_RETURN", "KEY_DOWN", "KEY_RETURN"),
            ui_mode="next",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["online_mode"], "1")
        self.assertEqual(frames[-1]["developer_mode"], "1")
        self.assertEqual(frames[-1]["wizard_step"], "online_options")
        skipped = self.run_player(
            "wizard-options",
            # Down twice lands on the button row; Left twice reaches Continue
            # whichever button it lands on, and Right is Skip.
            ("WAIT_MS=100", "KEY_UP", "KEY_UP", "KEY_RETURN", "KEY_DOWN", "KEY_DOWN",
             "KEY_LEFT", "KEY_LEFT", "KEY_RIGHT", "KEY_RETURN"),
            ui_mode="next",
        )
        frames = skipped["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["online_mode"] for frame in frames])
        self.assertEqual(frames[-1]["wizard_step"], "select_game")
        self.assertEqual(frames[-1]["online_mode"], "0", "Skip turns online mode back off")

    def test_next_ui_wizard_runs_the_first_run_path(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN", "KEY_RETURN"),
                              ui_mode="next")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")
        self.assertEqual(frames[3]["online_mode"], "0", "Continue is the options step's focus")
        self.assertEqual(frames[4]["picker"], "1",
                         "Choose a disc image is the focused action on Select game")
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" in run["stdout"]:
            # Without its fonts the next UI falls back to the existing renderer.
            legacy = self.run_player("empty", ("KEY_RETURN",), ui_mode="legacy")
            legacy_frames = legacy["frames"]
            assert isinstance(legacy_frames, list)
            self.assertNotEqual(frames[1]["pixels"], legacy_frames[1]["pixels"],
                                "the next UI, not the existing renderer, draws the wizard")

        fonts = self.run_player("wizard4", ("KEY_RETURN",), ui_mode="next")
        font_frames = fonts["frames"]
        assert isinstance(font_frames, list)
        self.assertEqual(font_frames[1]["wizard_step"], "ready_launch")
        self.assertEqual(font_frames[1]["font_confirmed"], "1")

        for key in ("KEY_ESCAPE", "KEY_RETURN"):
            staging = self.run_player("wizard-staging", (key,), ui_mode="next")
            stage_frames = staging["frames"]
            assert isinstance(stage_frames, list)
            self.assertEqual(stage_frames[0]["extracting"], "1")
            self.assertEqual(stage_frames[1]["extraction_cancel"], "1", key)

    BUILD_ACTION = str(0x4001)

    # The next UI draws the build, build tool download and error screens itself and
    # keeps every action and Back meaning the existing renderer had.
    def test_next_ui_build_screen_cancels_and_returns(self) -> None:
        building = self.run_player("building", ("KEY_ESCAPE",), ui_mode="next")
        frames = building["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertEqual(frames[0]["package_building"], "1")
        self.assertEqual(frames[1]["package_cancelled"], "1")
        self.assertEqual(frames[1]["view"], "library")
        # Launcher review B6: CANCEL BUILD is never the default focus, so a stray
        # Enter leaves the build running; a move reaches it.
        stray = self.run_player("building", ("KEY_RETURN",), ui_mode="next")
        frames = stray["frames"]
        assert isinstance(frames, list)
        self.assertNotEqual(frames[0]["next_focus"], self.BUILD_ACTION,
                            "CANCEL BUILD is not focused while the build runs")
        self.assertEqual(frames[1]["package_cancelled"], "0",
                         "a stray Enter does not cancel the build")
        self.assertEqual(frames[1]["view"], "building_package")
        cancel = self.run_player("building", ("KEY_DOWN", "KEY_RETURN"), ui_mode="next")
        frames = cancel["frames"]
        assert isinstance(frames, list)
        self.assertIn(self.BUILD_ACTION, self.focus_trail(cancel),
                      "a move from no focus reaches CANCEL BUILD")
        self.assertEqual(frames[-1]["package_cancelled"], "1",
                         "Enter on CANCEL BUILD cancels the build")
        built = self.run_player("build-ready", ("KEY_RETURN",), ui_mode="next")
        frames = built["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertEqual(frames[1]["view"], "library", "BACK TO LIBRARY once the package is built")
        self.assertEqual(frames[1]["package_cancelled"], "0")

    def test_next_ui_error_card_recovers_like_the_existing_card(self) -> None:
        missing_source = self.run_player("error", ("KEY_RETURN",), ui_mode="next")
        frames = missing_source["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["error"], "SOURCE_NOT_FOUND")
        self.assertEqual(frames[1]["picker"], "1")
        for code in ("PACKAGE_BUILD_FAILED", "RUNTIME_PACKAGE_NOT_READY", "PYTHON_NOT_FOUND"):
            with self.subTest(code=code):
                recovered = self.run_player("library", ("KEY_RETURN",), error_code=code,
                                            ui_mode="next")
                frames = recovered["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[1]["view"], "library")
                self.assertEqual(frames[1]["picker"], "0")

    def test_game_stop_cards_are_plain_with_restart_and_copy(self) -> None:
        """#1282: a game that ends on its own gets a plain card, not a raw code.

        Failing-before: "Child Process Terminated Early" and "Child Process Error
        Exit" were developer text, the stop card's badge fell back to the raw code
        for the two exit cards, and Return to Library was the only action."""
        expected = {
            "RUNTIME_SEMANTIC_BOUNDARY": ("Game_Stopped_Early", "Game_stopped"),
            "RUNTIME_PREMATURE_EXIT": ("Game_Couldn't_Start", "Game_didn't_start"),
            "RUNTIME_ERROR_EXIT": ("Game_Closed_Unexpectedly", "Game_closed"),
        }
        for code, (title, badge) in expected.items():
            with self.subTest(code=code):
                run = self.run_player("library", ("WAIT_MS=50",), error_code=code,
                                      ui_mode="next")
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[0]["error_title"], title)
                self.assertEqual(frames[0]["error_badge"], badge)
                self.assertNotIn("Child", frames[0]["error_title"])
                self.assertEqual(frames[0]["error_retry"], "Restart_game")

        # Copy details: the third button. The report names the problem, its code
        # and the raw boundary, and stays on this computer (clipboard only).
        copied = self.run_player(
            "library", ("WAIT_MS=50", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100"),
            error_code="RUNTIME_SEMANTIC_BOUNDARY", ui_mode="next",
        )
        stdout = str(copied["stdout"])
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in stdout:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        report = re.search(r"\[PLAYER_UI_TEST\] copied_details clipboard=(\d) report=(.*)", stdout)
        self.assertIsNotNone(report, stdout)
        assert report is not None
        self.assertEqual(report.group(1), "1", "the report did not reach the clipboard")
        for field in ("Nakagawa Recomp v", "Problem: Game Stopped Early",
                      "Code: RUNTIME_SEMANTIC_BOUNDARY",
                      "Reason: Stopped at unsupported-import-scenetadhocinit",
                      "tracked in issue 308", "Last boot events:",
                      "BOOT_EVENT phase=stop reason=semantic-boundary"):
            self.assertIn(field, report.group(2))
        frames = copied["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "error", "Copy details keeps the card open")
        self.assertEqual(frames[-1]["error_copied"], "1")

    def test_restart_game_relaunches_the_stopped_game(self) -> None:
        """#1282: Restart game runs the same launch PLAY does for that game.

        The synthetic library game has no package, so the launch answers with
        its own Game Not Ready card: the restart reached the launch path."""
        run = self.run_player(
            "library", ("WAIT_MS=50", "KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100"),
            error_code="RUNTIME_ERROR_EXIT", ui_mode="next",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["error"], "RUNTIME_ERROR_EXIT")
        self.assertIn(frames[-1]["error"], ("RUNTIME_PACKAGE_NOT_READY",), run["stdout"])
        self.assertNotEqual(frames[-1]["error_retry"], "Restart_game")

    # Enum values of PlayerUiPackageState (src/player/ui/player_ui_nk.h).
    PACKAGE_READY = "1"
    PACKAGE_STARTING = "5"
    PACKAGE_RUNNING = "6"

    # Open the ready title: its PLAY has focus (launcher review B6), so the next
    # Enter presses it.
    PLAY_READY_TITLE = ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300")
    TITLE_PRIMARY = str(0x2002)

    def test_play_keeps_drawing_through_a_slow_start(self) -> None:
        """#1298: PLAY answers at once and the launcher keeps drawing while the game starts.

        NK_UI_TEST_SLOW_START_MS makes the launch take 1.5 s before its package
        check, standing in for a retail package whose check hashes several
        hundred MB. Failing-before: the launch ran on the UI thread, so the
        frames stopped for the whole start and PLAY showed nothing until it
        ended (about 20 s for the owner's Hot Shots Tennis package)."""
        run = self.run_player(
            "ready", (*self.PLAY_READY_TITLE, "KEY_RETURN", "WAIT_MS=2500"),
            post_build_ready=True, wait_background=True, ui_mode="next",
            env_extra={"NK_UI_TEST_SLOW_START_MS": "1500"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        before = [frame for frame in frames if frame["next_primary_label"] == "PLAY"]
        self.assertTrue(before, "the synthetic ready package offers PLAY")
        self.assertEqual(before[-1]["next_focus"], self.TITLE_PRIMARY,
                         "opening a title focuses PLAY, not the LIBRARY tab")
        pending = [frame for frame in frames if frame["launch_pending"] == "1"]
        self.assertGreater(len(pending), 1, run["stdout"])
        # The frame that took the press drew the title before the launch began;
        # every later one shows it starting.
        for frame in pending[1:]:
            self.assertEqual(frame["next_primary_label"], "disabled:STARTING...")
            self.assertEqual(frame["next_package"], self.PACKAGE_STARTING)
        first = frames.index(pending[0])
        last = frames.index(pending[-1])
        ticks = [int(frame["ticks_ms"]) for frame in frames[first:last + 2]]
        self.assertGreaterEqual(ticks[-1] - ticks[0], 1400, "the start took its 1.5 s")
        self.assertGreaterEqual(len(pending), 10, "the launcher drew through the start")
        gaps = [later - earlier for earlier, later in zip(ticks, ticks[1:], strict=False)]
        self.assertLess(max(gaps), 500, f"a frame took {max(gaps)} ms during the start")
        # The synthetic package has no staged disc and no real program, so the
        # start ends with the launch's own card, as a synchronous launch did.
        outcome = frames[-1]
        self.assertEqual(outcome["launch_pending"], "0")
        self.assertIn(outcome["error"], ("RUNTIME_NOT_FOUND", "PROCESS_SPAWN_FAILED",
                                         "RUNTIME_PREMATURE_EXIT", "RUNTIME_ERROR_EXIT"),
                      run["stdout"])

    def title_frame_with_a_game_running(self, running: str) -> dict[str, str]:
        """The open title's last frame while NK_UI_TEST_GAME_RUNNING=`running`.

        A running game makes quitting ask first, so the script closes twice
        with the confirmation unavailable (as the close-confirmation test does)."""
        run = self.run_player(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", "CLOSE", "CLOSE"),
            post_build_ready=True, wait_background=True, ui_mode="next",
            env_extra={"NK_UI_TEST_GAME_RUNNING": running,
                       "NK_UI_TEST_MESSAGEBOX_FAIL": "1"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        titled = [frame for frame in frames if frame["next_package"] != "-1"]
        self.assertTrue(titled, run["stdout"])
        return titled[-1]

    # The "library" view's tiles at 1280x720: TEST00005, then TEST00006.
    FIRST_TILE = "MOUSE_MOVE=175,260"
    SECOND_TILE = "MOUSE_MOVE=485,260"

    def opened_titles(self, frames: list[dict[str, str]]) -> list[str]:
        """The disc of each title page the run opened, in order."""
        opened: list[str] = []
        previous = None
        for frame in frames:
            route = frame["next_route"]
            if route == "2" and previous != "2":
                opened.append(frame["selected_disc"])
            elif route == "2" and opened and frame["selected_disc"] != opened[-1]:
                opened.append(frame["selected_disc"])
            previous = route
        return opened

    def test_clicking_a_tile_opens_that_tile(self) -> None:
        """#1301: a click opens the tile under the pointer, not the focused one.

        Failing-before: every frame without an action handed the launcher a
        NONE action whose title_index was 0, and the launcher selected that
        index, so each title page it opened showed the first game."""
        run = self.run_player(
            "library",
            ("WAIT_MS=300", self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300"),
            ui_mode="next", wait_background=True,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["selected_disc"], "TEST00005", "focus starts on tile 1")
        self.assertEqual(self.opened_titles(frames), ["TEST00006"], run["stdout"])
        self.assertEqual(frames[-1]["next_route"], "2")
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")

    def test_clicking_a_tile_after_a_build_opens_that_tile(self) -> None:
        """#1301: after a build, with the built title focused, a click opens the clicked tile."""
        run = self.run_player(
            "library",
            ("WAIT_MS=300", "START_SYNTHETIC_BUILD=2", "WAIT_MS=2200",
             # Back in the library, focus the built title, as the owner had it.
             "KEY_RIGHT", "WAIT_MS=200",
             self.FIRST_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300",
             "KEY_ESCAPE", "WAIT_MS=300",
             self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300"),
            ui_mode="next", wait_background=True, post_build_ready=True,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        self.assertIn("synthetic_build result=PASS index=1 disc=TEST00006", str(run["stdout"]))
        frames = run["frames"]
        assert isinstance(frames, list)
        focused_built = [frame for frame in frames
                         if frame["view"] == "ready_library" and frame["next_route"] == "1"
                         and frame["selected_disc"] == "TEST00006"]
        self.assertTrue(focused_built, "the built title had focus before the click")
        self.assertEqual(self.opened_titles(frames), ["TEST00005", "TEST00006"], run["stdout"])

    # Control IDs from src/player/ui/player_ui_nk.h, as the frame records print them.
    TITLE_REMOVE = str(0x2004)
    REMOVE_CANCEL = str(0x4401)
    REMOVE_CONFIRM = str(0x4402)
    ROUTE_REMOVE = "4"

    def library_item(self, index: int) -> str:
        return str(0x1100 + index)

    LIBRARY_ADD = str(0x1002)

    # Remove from library is the title page's lowest control, so Down presses
    # end on it whichever action-row buttons are enabled.
    DOWN_TO_REMOVE = ("KEY_DOWN", "WAIT_MS=80") * 4

    def remove_run(self, events: tuple[str, ...]) -> list[dict[str, str]]:
        run = self.run_player("library", events, ui_mode="next", wait_background=True)
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        return frames

    def test_remove_from_library_asks_then_focuses_the_next_tile(self) -> None:
        """#1304: Remove from library asks first, Cancel keeps the game, and Remove
        returns to the library with the next tile focused."""
        open_first = ("WAIT_MS=300", self.FIRST_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300")
        frames = self.remove_run((*open_first, *self.DOWN_TO_REMOVE, "KEY_RETURN", "WAIT_MS=200",
                                  "KEY_ESCAPE", "WAIT_MS=200"))
        dialog = [frame for frame in frames if frame["next_route"] == self.ROUTE_REMOVE]
        self.assertGreater(len(dialog), 1, "Remove from library opens the dialog")
        # The title frame that took the press pushed the route; the dialog
        # draws from the next frame on, with Cancel focused.
        self.assertEqual(dialog[1]["next_focus"], self.REMOVE_CANCEL, "Cancel has the first focus")
        self.assertEqual(frames[-1]["next_route"], "2", "Back closes the dialog onto the title page")
        self.assertEqual(frames[-1]["next_focus"], self.TITLE_REMOVE)
        self.assertEqual(frames[-1]["games"], frames[0]["games"], "Cancel removes nothing")

        frames = self.remove_run((*open_first, *self.DOWN_TO_REMOVE, "KEY_RETURN", "WAIT_MS=200",
                                  "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=400"))
        confirm = [frame for frame in frames if frame["next_focus"] == self.REMOVE_CONFIRM]
        self.assertTrue(confirm, "the dialog's Remove button takes focus")
        last = frames[-1]
        self.assertEqual(int(last["games"]), int(frames[0]["games"]) - 1, "the title left")
        self.assertEqual(last["next_route"], "1", "Remove returns to the library")
        self.assertEqual(last["next_at_root"], "1")
        # TEST00005 was the first tile, so TEST00006 now holds its place.
        self.assertEqual(last["next_focus"], self.library_item(0))
        self.assertEqual(last["selected_disc"], "TEST00006")

    def test_removing_the_last_tile_focuses_the_add_card(self) -> None:
        """#1304: with no tile after the removed one, focus goes to the add card."""
        frames = self.remove_run(("WAIT_MS=300", self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP",
                                  "WAIT_MS=300", *self.DOWN_TO_REMOVE, "KEY_RETURN",
                                  "WAIT_MS=200", "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN",
                                  "WAIT_MS=400"))
        last = frames[-1]
        self.assertEqual(int(last["games"]), int(frames[0]["games"]) - 1, "the title left")
        self.assertEqual(last["next_route"], "1")
        self.assertEqual(last["next_focus"], self.LIBRARY_ADD)
        self.assertEqual(last["selected_disc"], "TEST00005")

    def test_compatibility_records_carry_a_player_tier(self) -> None:
        """#1297: every title record maps to a player tier; Hot Shots Tennis is Playable.

        The category -> tier table itself is pinned against docs/COMPATIBILITY.md
        by test_player_screens.c; this checks the per-title records the
        launcher really uses."""
        run = self.run_player(
            "library", ("WAIT_MS=100",),
            env_extra={"NK_UI_TEST_REPORT_COMPATIBILITY": "1"},
        )
        records = {}
        for line in str(run["stdout"]).splitlines():
            match = re.match(
                r"\[PLAYER_UI_TEST\] compatibility key=(\S+) category_tier=(.+?) tier=(.+)$",
                line,
            )
            if match:
                records[match.group(1)] = (match.group(2), match.group(3))
        self.assertIn("UCUS98701", records, run["stdout"])
        tiers = {"Perfect", "Playable", "In-game", "Menu", "Title screen", "Won't boot"}
        for key, (category_tier, tier) in records.items():
            with self.subTest(key=key):
                self.assertIn(category_tier, tiers, "the record's docs category has a tier")
                self.assertIn(tier, tiers)
        self.assertEqual(records["UCUS98701"], ("Title screen", "Playable"))
        self.assertEqual(records["UCUS-98701"], ("Title screen", "Playable"))
        self.assertEqual(records["PAC-MAN_Championship_Edition"][1], "In-game")
        self.assertEqual(records["pspdev-phase5-v1"][1], "Won't boot")

    def test_title_page_while_the_game_runs(self) -> None:
        """#1297: the running game's title page follows the package, not the STOP GAME action.

        The drawn texts are checked by test_player_screens.c; this pins the
        state the launcher hands the screen."""
        cases = {
            "TEST00006": ("STOP_GAME", self.PACKAGE_RUNNING),
            "starting:TEST00006": ("STOP_GAME", self.PACKAGE_STARTING),
        }
        for running, (label, state) in cases.items():
            with self.subTest(running=running):
                last = self.title_frame_with_a_game_running(running)
                self.assertEqual(last["selected_disc"], "TEST00006")
                self.assertEqual(last["selected_runtime"], "1", "the package is ready")
                self.assertEqual(last["next_primary_label"], label)
                self.assertEqual(last["next_package"], state)
                self.assertNotIn("boundary", last["next_expect"])
        # Another game running leaves this title's package card alone.
        other = self.title_frame_with_a_game_running("TEST00001")
        self.assertEqual(other["next_package"], self.PACKAGE_READY)

    def test_preflight_refusal_is_not_a_build_failure(self) -> None:
        """#1286: a build refused at preflight never started, so its card is not
        "Package Build Failed" and its badge is not a code."""
        run = self.run_player("library", ("WAIT_MS=50",), error_code="PACKAGE_BUILD_REFUSED",
                              ui_mode="next")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "error")
        self.assertEqual(frames[0]["error_title"], "Build_Not_Started")
        self.assertEqual(frames[0]["error_badge"], "Build_not_started")
        self.assertEqual(frames[0]["error_retry"], "Try_again")
        # An unknown code still gets a plain badge.
        other = self.run_player("library", ("WAIT_MS=50",), error_code="PYTHON_NOT_FOUND",
                                ui_mode="next")
        other_frames = other["frames"]
        assert isinstance(other_frames, list)
        self.assertEqual(other_frames[0]["error_badge"], "Problem")

    def test_failed_close_confirmation_needs_a_second_explicit_close(self) -> None:
        run = self.run_player(
            "library",
            ("CLOSE", "CLOSE"),
            env_extra={
                "NK_UI_TEST_GAME_RUNNING": "1",
                "NK_UI_TEST_MESSAGEBOX_FAIL": "1",
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["game_running"], "1")
        self.assertEqual(frames[-1]["game_running"], "1")
        self.assertEqual(frames[-1]["running"], "0")
        self.assertIn("Repeat the close request", run["stderr"])
        self.assertIn("explicit force-quit", run["stderr"])
        # The first close asked; the second, after the box failed, did not.
        dialogs = self.quit_dialogs(str(run["stdout"]))
        self.assertEqual(len(dialogs), 1, run["stdout"])
        self.assertEqual(dialogs[0]["message"],
                         "A game is still running.\nQuitting closes it, and anything "
                         "since the game's last save is lost.")

    # SDL3's Windows message box does not widen a button for a long label: it
    # showed "Close game and quit" as "ose game and qu". Labels this short fit.
    DIALOG_BUTTON_LABEL_MAX = 12

    def quit_dialogs(self, stdout: str) -> list[dict[str, object]]:
        """Each quit confirmation the player asked, as the host box would show it,
        with its buttons checked against what fits and what is the default.

        The player writes the message's line break as a backslash and an n, to
        keep each report on one line; this turns it back into a line break."""
        dialogs: list[dict[str, object]] = []
        prefix = "[PLAYER_UI_TEST] QUIT_DIALOG "
        for line in stdout.splitlines():
            if not line.startswith(prefix):
                continue
            body = line[len(prefix):]
            if body.startswith("title="):
                dialogs.append({"title": body[len("title="):], "buttons": []})
            elif body.startswith("message="):
                dialogs[-1]["message"] = body[len("message="):].replace("\\n", "\n")
            else:
                match = re.fullmatch(
                    r"button id=(\d+) return_default=([01]) escape_default=([01]) label=(.*)",
                    body)
                self.assertIsNotNone(match, line)
                assert match is not None
                buttons = dialogs[-1]["buttons"]
                assert isinstance(buttons, list)
                buttons.append(match.groups())
        for dialog in dialogs:
            self.assertEqual(dialog["title"], "Quit Nakagawa?")
            # Cancel is the Return and Escape default, so a stray Enter never
            # ends the game; Quit is button 1, which the player acts on.
            self.assertEqual(dialog["buttons"],
                             [("0", "1", "1", "Cancel"), ("1", "0", "0", "Quit")])
            buttons = dialog["buttons"]
            assert isinstance(buttons, list)
            for button in buttons:
                self.assertLessEqual(len(button[3]), self.DIALOG_BUTTON_LABEL_MAX, button)
        return dialogs

    def test_quit_confirmation_names_the_running_game(self) -> None:
        run = self.run_player(
            "library",
            ("CLOSE",),
            env_extra={
                "NK_UI_TEST_GAME_RUNNING": "TEST00005",
                "NK_UI_TEST_MESSAGEBOX_FAIL": "1",
            },
        )
        dialogs = self.quit_dialogs(str(run["stdout"]))
        self.assertEqual(len(dialogs), 1, run["stdout"])
        self.assertEqual(dialogs[0]["message"],
                         "Nakagawa Test Program is still running.\nQuitting "
                         "closes it, and anything since the game's last save is lost.")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["game_running"], "1")

    def test_font_fallback_reason_is_reported(self) -> None:
        """Every frame names the active text path and, on fallback, why (#421)."""
        forced = self.run_player("library", (), env_extra={"NK_UI_NO_TTF": "1"})
        forced_frames = forced["frames"]
        assert isinstance(forced_frames, list)
        for frame in forced_frames:
            self.assertEqual(frame["font"], "bitmap")
            self.assertEqual(frame["font_reason"], "forced-off")
        self.assertIn("NK_UI_NO_TTF", str(forced["stderr"]))

        normal = self.run_player("library", ())
        normal_frames = normal["frames"]
        assert isinstance(normal_frames, list)
        for frame in normal_frames:
            self.assertIn(frame["font"], ("ttf", "bitmap"))
            if frame["font"] == "ttf":
                self.assertEqual(frame["font_reason"], "NONE")
            else:
                # A fallback without a named reason is exactly the silent
                # degradation this contract forbids.
                self.assertNotEqual(frame["font_reason"], "NONE")
                self.assertTrue(frame["font_reason"])


def library_disc_ids(scratch: Path) -> list[str]:
    """Disc IDs in every library.json this run wrote below its isolated LOCALAPPDATA."""
    ids: list[str] = []
    for path in scratch.rglob("library.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        ids.extend(entry["disc_id"] for entry in data.get("games", []))
    return ids


class FirstRunRouteTests(unittest.TestCase):
    """First-run routes: --iso= persistence and --launch-index on an empty library.

    An inspected disc is stored only when the user chooses ADD TO LIBRARY, as the
    file picker and setup wizard already do; --launch-now is the exception, since it
    must launch and a launch needs a library record. --launch-index names a library
    entry, so an empty library is an error and bundled demo entries are named SAMPLE.
    """

    def scratch_env(self, scratch: Path) -> dict[str, str]:
        profile = scratch / "profile"
        for name in ("appdata", "localappdata", "profile"):
            (scratch / name).mkdir(exist_ok=True)
        env = os.environ.copy()
        env.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIODRIVER": "dummy",
                "APPDATA": str(scratch / "appdata"),
                "LOCALAPPDATA": str(scratch / "localappdata"),
                "USERPROFILE": str(profile),
                "HOME": str(profile),
                "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"),
                "XDG_CACHE_HOME": str(profile / "cache"),
            }
        )
        return env

    def run_route(self, scratch: Path, args: Sequence[str]) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(PLAYER_EXE), *args, f"--runtime-root={scratch / 'runtime'}"],
            cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
            timeout=120, check=False,
        )

    def test_bad_startup_iso_opens_error_card_and_keeps_launcher_running(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-invalid-iso-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            bad_iso = scratch / "not a PSP disc.iso"
            bad_iso.write_bytes(b"synthetic non-ISO input\n")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()
            screenshot = scratch / "last-frame.bmp"
            completed = subprocess.run(
                [
                    str(PLAYER_EXE), f"--iso={bad_iso}",
                    "--ui-test-events=WAIT_VIEW=error,5000;KEY_RETURN;WAIT_MS=100;QUIT",
                    f"--ui-test-screenshot={screenshot}",
                    f"--runtime-root={runtime_root}",
                ],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True,
                text=True, timeout=30, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            frames = parse_frames(completed.stdout)
            self.assertTrue(frames, completed.stdout + completed.stderr)
            error_frame = next((frame for frame in frames if frame["view"] == "error"), None)
            self.assertIsNotNone(error_frame, completed.stdout + completed.stderr)
            assert error_frame is not None
            self.assertEqual(error_frame["view"], "error")
            self.assertEqual(error_frame["error"], "ISO_CORRUPT")
            self.assertEqual(error_frame["bad_disc_notice"], "1")
            self.assertEqual(error_frame["error_text_complete"], "1")
            self.assertEqual(frames[-1]["view"], "library")
            self.assertEqual(frames[-1]["picker"], "1")
            self.assertTrue(screenshot.is_file())

    def test_iso_route_stores_nothing_until_add_to_library(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-iso-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            views = [frame["view"] for frame in parse_frames(completed.stdout)]
            self.assertIn("supported", views, completed.stdout)
            self.assertEqual(library_disc_ids(scratch), [],
                             "the card still offers ADD TO LIBRARY, so nothing may be stored yet")

    def test_add_to_library_on_the_iso_card_stores_the_disc(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-add-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;KEY_RETURN;WAIT_VIEW=library,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertEqual(library_disc_ids(scratch), ["TEST00002"])

    def test_add_to_library_sets_up_an_archive_disc_before_saving_it(self) -> None:
        """ADD TO LIBRARY on an archive disc runs the wizard's staging worker.

        Failing-before: the card saved the disc without its files, so PLAY then
        found the game's data missing. Now the card hands the disc to the same
        staging transaction (progress, cancel, retry, calm messages) and the
        game reaches the library only with its files in place.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-card-archive-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}",
                 "--ui-test-events=WAIT_VIEW=supported,10000;KEY_RETURN;"
                 "WAIT_VIEW=ready_library,30000;QUIT",
                 f"--ui-test-screenshot={scratch / 'last-frame.bmp'}"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            views = [frame["view"] for frame in parse_frames(completed.stdout)]
            self.assertIn("supported", views, completed.stdout)
            self.assertEqual(views[-1], "ready_library", completed.stdout)
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            game = library["games"][0]
            self.assertTrue(game["assets_staged"])
            prepared = Path(game["prepared_root"])
            self.assertEqual(prepared.resolve(), (user_root / "games" / "ULUS99996").resolve())
            member = prepared / "xbdata" / "menu" / "assets.xb.d" / "data" / "raw.bin"
            self.assertEqual(member.read_bytes(), b"synthetic archive member")

    def test_launch_now_on_experimental_iso_reports_the_unavailable_boundary(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import build_plain_mips_elf, create_test_iso_with_executables

        with tempfile.TemporaryDirectory(prefix=".first-run-experimental-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "experimental.iso"
            executable = build_plain_mips_elf() + bytes(i % 251 for i in range(70000))
            create_test_iso_with_executables(
                iso, executable, disc_id="ULUS99998", title="Experimental Fixture"
            )
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--launch-now",
                "--ui-test-events=WAIT_MS=100;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])

        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        frames = parse_frames(completed.stdout)
        self.assertTrue(frames, completed.stdout)
        self.assertEqual(frames[0]["view"], "error")
        self.assertEqual(frames[0]["error"], "EXPERIMENTAL_LAUNCH_UNAVAILABLE")
        self.assertIn("in the works.", completed.stderr)
        self.assertNotIn("(#308)", completed.stderr)
        self.assertEqual(library_disc_ids(scratch), [])

    def test_stage_route_runs_the_wizard_worker_for_an_archive_disc(self) -> None:
        """--stage drives the setup wizard's own staging worker.

        The disc's data ships in an XB archive below USRDIR/xbdata. The worker
        runs the shared staging transaction, the game lands in the library with
        its files promoted into the per-user data root, and the wizard ends on
        the ready library card.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-archive-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}", "--stage",
                 "--ui-test-events=WAIT_VIEW=ready_library,30000;QUIT",
                 f"--ui-test-screenshot={scratch / 'last-frame.bmp'}"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            frames = parse_frames(completed.stdout)
            self.assertTrue(frames, completed.stdout)
            self.assertEqual(frames[-1]["view"], "ready_library", completed.stdout)
            self.assertEqual(frames[-1]["selected_staged"], "1")
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            game = library["games"][0]
            self.assertTrue(game["assets_staged"])
            prepared = Path(game["prepared_root"])
            self.assertEqual(prepared.resolve(), (user_root / "games" / "ULUS99996").resolve())
            member = prepared / "xbdata" / "menu" / "assets.xb.d" / "data" / "raw.bin"
            self.assertEqual(member.read_bytes(), b"synthetic archive member")
            self.assertFalse((user_root / "games" / ".staging_ULUS99996").exists())
            self.assertEqual(library_disc_ids(scratch / "localappdata"), [],
                             "--user-data-root keeps the library out of the default data folder")

    def test_headless_launch_now_sets_up_archive_files_before_play(self) -> None:
        """--launch-now stands in for PLAY NOW on a disc whose files are not set up.

        The same staging transaction runs first, the game is recorded with its
        files in place, and the launch then reaches PLAY's own next boundary
        (no package has been built here) instead of a missing data folder.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-launch-now-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}",
                 "--launch-now", "--headless-launch"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            output = completed.stdout + completed.stderr
            self.assertIn("STAGING_RESULT status=PASS", completed.stdout, output)
            self.assertEqual(completed.returncode, 4, output)
            self.assertIn("--launch-now failed: Runtime package is missing", completed.stderr)
            self.assertNotIn("data folder", completed.stderr)
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            self.assertTrue(library["games"][0]["assets_staged"])
            self.assertTrue((user_root / "games" / "ULUS99996" / "xbdata" / "menu" /
                             "assets.xb.d" / "data" / "raw.bin").is_file())

    def test_launch_index_on_an_empty_library_fails_with_a_clear_message(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-empty-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--launch-index=0", "--headless-launch"])
        self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
        self.assertIn("library is empty", completed.stderr)
        self.assertNotIn("has no prepared runtime", completed.stderr)

    def test_demo_launch_index_names_its_bundled_entry_as_a_sample(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-demo-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--demo", "--launch-index=0", "--headless-launch"])
        # No runtime is built in the scratch root, so the bounded launch refuses.
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        self.assertIn("SAMPLE", completed.stderr)


if __name__ == "__main__":
    unittest.main()


class PlayerVersionIdentityTests(unittest.TestCase):
    """The About page's build date comes from SOURCE_DATE_EPOCH when it is set.

    A dry run of the player link shows the define the build passes; nothing is
    compiled. The date itself (MM-DD-YYYY from the epoch) is checked by
    tests/native/test_player_screens.c.
    """

    def _player_defines(self, env_extra: dict[str, str]) -> list[str]:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if make is None:
            self.skipTest("make is not on PATH")
        env = dict(os.environ)
        env.update(env_extra)
        with tempfile.TemporaryDirectory(prefix="nk-player-identity-") as scratch:
            build_root = Path(scratch).as_posix()
            exe = f"{build_root}/nakagawa_player{'.exe' if os.name == 'nt' else ''}"
            run = subprocess.run(
                [make, "--no-print-directory", "-n", "-B", f"BUILD_ROOT={build_root}", exe],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=600,
            )
        if run.returncode != 0:
            self.skipTest(f"the player dry run needs its build dependencies: {run.stderr[-400:]}")
        return re.findall(r"-DNK_PLAYER_BUILD_[A-Z]+=\S+", run.stdout)

    def test_source_date_epoch_fixes_the_build_date(self) -> None:
        defines = self._player_defines({"SOURCE_DATE_EPOCH": "1792713600"})
        self.assertIn("-DNK_PLAYER_BUILD_EPOCH=1792713600LL", defines)
        again = self._player_defines({"SOURCE_DATE_EPOCH": "1792713600"})
        self.assertEqual(defines, again, "the same epoch gives the same defines")
