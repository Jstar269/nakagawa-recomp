# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate and qualify the source-owned full-production smoke guest.

The committed fixture is this recipe, not a binary.  It deterministically emits a
small ELF32 PSP PRX and a matching ``~PSP`` header into the ignored build tree.
The guest has two load segments, real type-A PSP relocations, one import, three
discoverable functions, a BSS extent, and a relocation-dependent result pointer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]

BASE = 0x08804000
ENTRY = BASE
HELPER = BASE + 0x28
IMPORT_STUB = BASE + 0x78
DATA_BASE = BASE + 0x1000
RESULT_POINTER = DATA_BASE + 0x68
RESULT = DATA_BASE + 0x6C
SENTINEL = 0x13579BDF
NID = 0x7591C7DB
LIBRARY = "SysMemUserForUser"

TEXT_FILE_OFFSET = 0x100
TEXT_FILE_SIZE = 0x80
DATA_FILE_OFFSET = 0x200
DATA_FILE_SIZE = 0x70
DATA_MEMORY_SIZE = 0xB0
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE
RELOCATION_FILE_OFFSET = 0x280

R_MIPS_NONE = 0
R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
SHT_PRX_RELOC = 0x700000A0


def _r(rs: int, rt: int, rd: int, shift: int, function: int) -> int:
    return (
        ((rs & 31) << 21)
        | ((rt & 31) << 16)
        | ((rd & 31) << 11)
        | ((shift & 31) << 6)
        | (function & 63)
    )


def _i(opcode: int, rs: int, rt: int, immediate: int) -> int:
    return (
        ((opcode & 63) << 26)
        | ((rs & 31) << 21)
        | ((rt & 31) << 16)
        | (immediate & 0xFFFF)
    )


def _j(opcode: int, target: int) -> int:
    return ((opcode & 63) << 26) | ((target >> 2) & 0x03FFFFFF)


def _words(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}I", *values)


def relocation_info(relocation_type: int, offset_segment: int, target_segment: int) -> int:
    return relocation_type | (offset_segment << 8) | (target_segment << 16)


def relocation_records() -> list[tuple[int, int]]:
    """Return the ordered PSP type-A relocation table.

    The final record owns the result pointer.  The mutation campaign changes
    precisely that record from R_MIPS_32 to R_MIPS_NONE.
    """

    return [
        (0x08, relocation_info(R_MIPS_26, 0, 0)),  # entry -> helper
        (0x14, relocation_info(R_MIPS_26, 0, 0)),  # entry -> import stub
        (0x28, relocation_info(R_MIPS_HI16, 0, 1)),
        (0x2C, relocation_info(R_MIPS_LO16, 0, 1)),
        (0x2C, relocation_info(R_MIPS_32, 1, 1)),  # module libstub
        (0x30, relocation_info(R_MIPS_32, 1, 1)),  # module libstubend
        (0x50, relocation_info(R_MIPS_32, 1, 1)),  # library name
        (0x5C, relocation_info(R_MIPS_32, 1, 1)),  # NID table
        (0x60, relocation_info(R_MIPS_32, 1, 0)),  # first import stub
        (0x68, relocation_info(R_MIPS_32, 1, 1)),  # result pointer (load-bearing)
    ]


def build_text_segment() -> bytes:
    text = bytearray(TEXT_FILE_SIZE)
    entry_words = [
        _i(0x09, 29, 29, -16),          # addiu sp, sp, -16
        _i(0x2B, 29, 31, 12),           # sw ra, 12(sp)
        _j(0x03, 0x28),                  # jal helper (relocated)
        0,
        _r(2, 0, 4, 0, 0x21),           # addu a0, v0, zero
        _j(0x03, 0x78),                  # jal import stub (relocated)
        0,
        _i(0x23, 29, 31, 12),           # lw ra, 12(sp)
        _r(31, 0, 0, 0, 0x08),          # jr ra
        _i(0x09, 29, 29, 16),           # addiu sp, sp, 16 (delay slot)
    ]
    helper_words = [
        _i(0x0F, 0, 8, 0),              # lui t0, %hi(result_pointer)
        _i(0x23, 8, 8, 0x68),           # lw t0, %lo(result_pointer)(t0)
        _i(0x0F, 0, 9, SENTINEL >> 16),
        _i(0x0D, 9, 9, SENTINEL),
        _i(0x2B, 8, 9, 0),              # sw t1, 0(t0)
        _i(0x23, 8, 2, 0),              # lw v0, 0(t0)
        _r(31, 0, 0, 0, 0x08),          # jr ra
        0,
    ]
    text[0 : len(entry_words) * 4] = _words(entry_words)
    text[0x28 : 0x28 + len(helper_words) * 4] = _words(helper_words)
    text[0x78:0x80] = _words([0x03E00008, 0x0000000C])  # jr ra; syscall
    return bytes(text)


def build_data_segment() -> bytes:
    data = bytearray(DATA_FILE_SIZE)
    module_name = b"production_smoke"
    library_name = LIBRARY.encode("ascii") + b"\0"
    data[0:52] = struct.pack(
        "<HH28s5I",
        0,
        0x0100,
        module_name + b"\0" * (28 - len(module_name)),
        0,
        0,
        0,
        0x50,
        0x64,
    )
    if len(library_name) > 0x50 - 0x34:
        raise AssertionError("synthetic library name no longer fits the fixed layout")
    data[0x34 : 0x34 + len(library_name)] = library_name
    data[0x50:0x64] = struct.pack(
        "<IHHBBHII",
        0x34,
        0x0101,
        0x0009,
        5,
        0,
        1,
        0x64,
        0x78,
    )
    struct.pack_into("<I", data, 0x64, NID)
    struct.pack_into("<I", data, 0x68, 0x6C)
    struct.pack_into("<I", data, 0x6C, 0)
    return bytes(data)


def build_prx() -> bytes:
    text = build_text_segment()
    data = build_data_segment()
    relocations = relocation_records()
    relocation_bytes = b"".join(struct.pack("<II", *record) for record in relocations)

    section_names = (
        b"\0.text\0.sceStub.text\0.rodata.sceModuleInfo\0.rodata\0.lib.stub\0"
        b".rodata.sceNid\0.data\0.reloc.sceModuleInfo\0.bss\0.shstrtab\0"
    )
    names = {
        name: section_names.index(name.encode("ascii"))
        for name in (
            ".text",
            ".sceStub.text",
            ".rodata.sceModuleInfo",
            ".rodata",
            ".lib.stub",
            ".rodata.sceNid",
            ".data",
            ".reloc.sceModuleInfo",
            ".bss",
            ".shstrtab",
        )
    }
    shstr_offset = RELOCATION_FILE_OFFSET + len(relocation_bytes)
    section_table_offset = (shstr_offset + len(section_names) + 3) & ~3
    section_count = 11

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    elf_header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        0xFFA0,                         # ET_SCE_PRX
        8,                              # EM_MIPS
        1,
        0,                              # entry, rebased by the loader
        52,
        section_table_offset,
        0x10,
        52,
        32,
        2,
        40,
        section_count,
        section_count - 1,
    )
    program_headers = b"".join(
        [
            struct.pack(
                "<8I", 1, TEXT_FILE_OFFSET, 0, 0,
                TEXT_FILE_SIZE, TEXT_FILE_SIZE, 5, 0x1000,
            ),
            struct.pack(
                "<8I", 1, DATA_FILE_OFFSET, 0x1000, 0x1000,
                DATA_FILE_SIZE, DATA_MEMORY_SIZE, 6, 0x1000,
            ),
        ]
    )

    def section(
        name: str,
        section_type: int,
        flags: int,
        address: int,
        offset: int,
        size: int,
        alignment: int,
        entry_size: int = 0,
    ) -> bytes:
        return struct.pack(
            "<10I",
            names[name],
            section_type,
            flags,
            address,
            offset,
            size,
            0,
            0,
            alignment,
            entry_size,
        )

    sections = [struct.pack("<10I", *([0] * 10))]
    sections.extend(
        [
            section(".text", 1, 6, 0, TEXT_FILE_OFFSET, 0x48, 4),
            section(".sceStub.text", 1, 6, 0x78, TEXT_FILE_OFFSET + 0x78, 8, 4),
            section(".rodata.sceModuleInfo", 1, 2, 0x1000, DATA_FILE_OFFSET, 52, 4),
            section(".rodata", 1, 2, 0x1034, DATA_FILE_OFFSET + 0x34, 0x1C, 1),
            section(".lib.stub", 1, 2, 0x1050, DATA_FILE_OFFSET + 0x50, 20, 4),
            section(".rodata.sceNid", 1, 2, 0x1064, DATA_FILE_OFFSET + 0x64, 4, 4),
            section(".data", 1, 3, 0x1068, DATA_FILE_OFFSET + 0x68, 8, 4),
            section(
                ".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0,
                RELOCATION_FILE_OFFSET, len(relocation_bytes), 4, 8,
            ),
            section(".bss", 8, 3, 0x1070, DATA_FILE_OFFSET + DATA_FILE_SIZE, BSS_SIZE, 16),
            section(".shstrtab", 3, 0, 0, shstr_offset, len(section_names), 1),
        ]
    )

    blob = bytearray(section_table_offset + section_count * 40)
    blob[0 : len(elf_header)] = elf_header
    blob[52 : 52 + len(program_headers)] = program_headers
    blob[TEXT_FILE_OFFSET : TEXT_FILE_OFFSET + len(text)] = text
    blob[DATA_FILE_OFFSET : DATA_FILE_OFFSET + len(data)] = data
    blob[RELOCATION_FILE_OFFSET : RELOCATION_FILE_OFFSET + len(relocation_bytes)] = relocation_bytes
    blob[shstr_offset : shstr_offset + len(section_names)] = section_names
    blob[section_table_offset : section_table_offset + len(b"".join(sections))] = b"".join(sections)
    return bytes(blob)


def build_psp_header() -> bytes:
    header = bytearray(0x80)
    header[:4] = b"~PSP"
    header[0x27] = 2
    struct.pack_into("<I", header, 0x38, BSS_SIZE)
    struct.pack_into("<4I", header, 0x54, TEXT_FILE_SIZE, DATA_MEMORY_SIZE, 0, 0)
    return bytes(header)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path: Path, data: bytes) -> bool:
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def manifest_bytes(prx: bytes, psp_header: bytes) -> bytes:
    records = relocation_records()
    manifest = {
        "schema": 1,
        "kind": "source-owned-psp-production-smoke",
        "base": f"0x{BASE:08x}",
        "entry": f"0x{ENTRY:08x}",
        "helper": f"0x{HELPER:08x}",
        "import_stub": f"0x{IMPORT_STUB:08x}",
        "result_pointer": f"0x{RESULT_POINTER:08x}",
        "result": f"0x{RESULT:08x}",
        "sentinel": f"0x{SENTINEL:08x}",
        "library": LIBRARY,
        "nid": f"0x{NID:08x}",
        "load_segments": 2,
        "bss_size": BSS_SIZE,
        "relocation_count": len(records),
        "load_bearing_relocation_index": len(records) - 1,
        "load_bearing_relocation_offset": "0x00000068",
        "prx_sha256": sha256(prx),
        "psp_header_sha256": sha256(psp_header),
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("ascii")


def generate(out_dir: Path) -> int:
    prx = build_prx()
    psp_header = build_psp_header()
    outputs = {
        out_dir / "guest.prx": prx,
        out_dir / "guest.psp": psp_header,
        out_dir / "manifest.json": manifest_bytes(prx, psp_header),
    }
    changed = [str(path) for path, data in outputs.items() if write_if_changed(path, data)]
    state = "updated" if changed else "unchanged"
    print(
        "PRODUCTION_SMOKE_FIXTURE "
        f"state={state} prx_sha256={sha256(prx)} psp_sha256={sha256(psp_header)}"
    )
    return 0


def _read_manifest(fixture_dir: Path) -> dict[str, object]:
    return json.loads((fixture_dir / "manifest.json").read_text(encoding="ascii"))


def verify(build_dir: Path) -> int:
    fixture_dir = build_dir / "fixture"
    manifest = _read_manifest(fixture_dir)
    prx = (fixture_dir / "guest.prx").read_bytes()
    psp_header = (fixture_dir / "guest.psp").read_bytes()
    if manifest != json.loads(manifest_bytes(prx, psp_header)):
        raise RuntimeError("fixture manifest does not match the generated bytes")

    image_path = build_dir / "production_smoke_image.bin"
    executable = build_dir / "production_smoke.exe"
    map_path = build_dir / "production_smoke.map"
    main_c = build_dir / "production_smoke_recomp.c"
    funcs_h = build_dir / "production_smoke_recomp_funcs.h"
    imports_toml = build_dir / "production_smoke_imports.toml"
    required_files = (image_path, executable, map_path, main_c, funcs_h, imports_toml)
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        raise RuntimeError("production build is missing: " + ", ".join(missing))

    chunk_sources = sorted(build_dir.glob("production_smoke_recomp_[0-9]*.c"))
    chunk_objects = sorted(build_dir.glob("production_smoke_recomp_[0-9]*.o"))
    if len(chunk_sources) != 3 or len(chunk_objects) != 3:
        raise RuntimeError(
            "expected three generated chunk sources and objects; "
            f"found sources={len(chunk_sources)} objects={len(chunk_objects)}"
        )

    main_text = main_c.read_text(encoding="ascii")
    if "sr_register_all: starting 3 registrations" not in main_text:
        raise RuntimeError("generated registration count is not exactly three")
    for index in range(3):
        if f"sr_register_chunk_{index}();" not in main_text:
            raise RuntimeError(f"generated main omits chunk registration {index}")
    generated_text = "\n".join(path.read_text(encoding="ascii") for path in chunk_sources)
    for address in (ENTRY, HELPER, IMPORT_STUB):
        if f"f_{address:08x}" not in generated_text:
            raise RuntimeError(f"generated chunks omit function 0x{address:08x}")
    if f"sr_syscall(s, 0x{NID:08x}u)" not in generated_text:
        raise RuntimeError("generated import stub does not call the real HLE dispatcher")

    import_text = imports_toml.read_text(encoding="ascii")
    if LIBRARY not in import_text or f"0x{NID:08x}" not in import_text.lower():
        raise RuntimeError("generated import manifest omits the synthetic import")

    image = image_path.read_bytes()
    result_pointer_offset = RESULT_POINTER - BASE
    result_offset = RESULT - BASE
    if len(image) != 0x10B0:
        raise RuntimeError(f"flat image length is 0x{len(image):x}, expected 0x10b0")
    if struct.unpack_from("<I", image, result_pointer_offset)[0] != RESULT:
        raise RuntimeError("load-bearing R_MIPS_32 result pointer was not applied")
    if struct.unpack_from("<I", image, result_offset)[0] != 0:
        raise RuntimeError("result slot is not zero before guest execution")

    map_text = map_path.read_text(encoding="utf-8", errors="replace").replace("\\", "/")
    build_prefix = build_dir.as_posix().rstrip("/")
    required_map_objects = tuple(
        f"{build_prefix}/{name}"
        for name in (
            "production_smoke_recomp.o",
            "production_smoke_recomp_0.o",
            "production_smoke_recomp_1.o",
            "production_smoke_recomp_2.o",
            "ge.o",
            "recomp.o",
            "title_config.o",
            "vfpu_tables.o",
            "debug.o",
            "watchpoints_file.o",
            "guest_printf.o",
            "perf.o",
            "fbcap_policy.o",
            "ge_capture.o",
            "vfpu_interp.o",
            "hle.o",
            "sched.o",
            "sr_coro.o",
            "iso_unavailable.o",
            "pgd_unavailable.o",
            "mpeg.o",
            "pgf_unavailable.o",
            "gui.o",
            "audio_unavailable.o",
            "h264_mf.o",
            "h264_null.o",
            "savedata.o",
            "osk_win.o",
            "driver.o",
            "sdl3vk.o",
            "ge_gpu.o",
            "atrac3p_atrac3p_api.o",
            "atrac3p_libavcodec/atrac.o",
            "atrac3p_libavcodec/atrac3plus.o",
            "atrac3p_libavcodec/atrac3plusdec.o",
            "atrac3p_libavcodec/atrac3plusdsp.o",
            "atrac3p_libavcodec/bitstream.o",
            "atrac3p_libavcodec/fft_float.o",
            "atrac3p_libavcodec/fft_init_table.o",
            "atrac3p_libavcodec/mdct_float.o",
            "atrac3p_libavcodec/sinewin.o",
            "atrac3p_libavutil/float_dsp.o",
            "atrac3p_libavutil/intmath.o",
            "atrac3p_libavutil/log2_tab.o",
            "atrac3p_libavutil/mem.o",
            "atrac3p_libavutil/reverse.o",
            "atrac3p_bridge.o",
        )
    )
    if "gate_stub" in map_text:
        raise RuntimeError("reduced gate_stub leaked into the production link")
    absent = [name for name in required_map_objects if name not in map_text]
    if absent:
        raise RuntimeError("production link map omits: " + ", ".join(absent))

    print(
        "PRODUCTION_SMOKE_VERIFY status=PASS functions=3 chunks=3 "
        f"relocations={len(relocation_records())} image_sha256={sha256(image)}"
    )
    return 0


def run(build_dir: Path) -> int:
    executable = build_dir / "production_smoke.exe"
    image_path = build_dir / "production_smoke_image.bin"
    command = [
        str(executable),
        "--image",
        str(image_path),
        f"0x{BASE:08x}",
        f"0x{ENTRY:08x}",
        "none",
        "none",
        "--sched",
        f"--expect-u32=0x{RESULT:08x}:0x{SENTINEL:08x}",
    ]
    environment = os.environ.copy()
    environment["SR_DISPATCH_FATAL"] = "1"
    environment["SR_HLELOG"] = "1"
    completed = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
    write_if_changed(build_dir / "production_smoke.stdout.log", completed.stdout.encode("utf-8"))
    write_if_changed(build_dir / "production_smoke.stderr.log", completed.stderr.encode("utf-8"))
    combined = completed.stdout + completed.stderr
    if completed.returncode != 0:
        sys.stderr.write(combined)
        raise RuntimeError(f"production smoke runtime exited {completed.returncode}")
    markers = (
        "BOOT_EVENT phase=init public_safe=1",
        f"BOOT_EVENT phase=image_loaded entry=0x{ENTRY:08x}",
        "sr_register_all: starting 3 registrations",
        "sr_register_all: completed",
        f"BOOT_EVENT phase=runtime_registered entry=0x{ENTRY:08x}",
        f"BOOT_EVENT phase=guest_start mode=scheduler entry=0x{ENTRY:08x}",
        f"HLE: calling sceKernelSetCompiledSdkVersion (0x{NID:08x})",
        (
            f"DRIVER_EXPECT_U32 addr=0x{RESULT:08x} got=0x{SENTINEL:08x} "
            f"expected=0x{SENTINEL:08x} status=PASS"
        ),
    )
    missing = [marker for marker in markers if marker not in combined]
    if missing:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence omits: " + ", ".join(missing))
    forbidden = ("UNKNOWN NID", "NONPLT_MISS", "status=FAIL")
    present = [marker for marker in forbidden if marker in combined]
    if present:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence contains: " + ", ".join(present))
    print(
        "PRODUCTION_SMOKE_RUN status=PASS "
        f"nid=0x{NID:08x} result=0x{RESULT:08x} sentinel=0x{SENTINEL:08x}"
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--out-dir", type=Path, required=True)
    for command in ("verify", "run"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--build-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "generate":
            return generate(args.out_dir)
        if args.command == "verify":
            return verify(args.build_dir)
        if args.command == "run":
            return run(args.build_dir)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        sys.stderr.write(f"PRODUCTION_SMOKE_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
