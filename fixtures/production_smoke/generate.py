# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate and qualify the source-owned full-production smoke guest.

The committed fixture is this recipe, not a binary.  It deterministically emits
small ELF32 PSP PRX/``~PSP`` inputs into the ignored build tree according to an
explicit execution MODE.  Every mode uses the same base address, entry, import
stub, library/NID, result slot and sentinel; modes differ only in the declared
``ModePlan``: the guest tail layout, the build/codegen choices, and the runtime
expectation.

Modes
-----
``aot``
    The plain production path and the historical gate: every discovered
    function is emitted as native C, control flow is compiled calls, and the
    run must pass the relocation-dependent ``--expect-u32`` sentinel.

``aot-gap``
    The AOT/dispatch seam.  The helper at ``HELPER`` keeps its full real body
    in the guest image (inside the ordinary executable ``.text`` extent) and is
    still discovered by the analyzer, but the mode's build choice
    (``--omit-aot``) removes it from native emission/registration.  Region A's
    direct ``jal`` therefore compiles to the ordinary production
    ``dispatch(s, 0x<HELPER>)`` statement, and at runtime the guest transfer
    leaves the directly compiled destination set through the real dispatcher.
    Until a production interpreter fallback exists (issue #116), that dispatch
    misses and, with ``SR_DISPATCH_FATAL=1``, terminates the process.  The
    helper's tail transfers to registered AOT region B, so once the interpreter
    lands the SAME build executes interpreted-helper -> AOT-resume -> sentinel
    without any fixture or pipeline change.
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
REGION_B = BASE + 0x58
IMPORT_STUB = BASE + 0x78
DATA_BASE = BASE + 0x1000
RESULT_POINTER = DATA_BASE + 0x68
RESULT = DATA_BASE + 0x6C
SENTINEL = 0x13579BDF
MARKER = 0x00005A5A
NID = 0x7591C7DB
LIBRARY = "SysMemUserForUser"

TEXT_FILE_OFFSET = 0x100
TEXT_FILE_SIZE = 0x80
DATA_FILE_OFFSET = 0x200
DATA_FILE_SIZE = 0x70
DATA_MEMORY_SIZE = 0xB0
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE
RELOCATION_FILE_OFFSET = 0x280

# Guest-image extent covered by the .text SECTION header (the PT_LOAD extent is
# always the full TEXT_FILE_SIZE). The gap layout places region B and the dead
# discovery anchor inside the section-owned executable area.
TEXT_SECTION_SIZE_AOT = 0x48
TEXT_SECTION_SIZE_GAP = 0x78

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


def _entry_words() -> list[int]:
    """Region A. Identical bytes in every mode."""
    return [
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


def _helper_words_aot() -> list[int]:
    """Historical helper: writes the sentinel through the result pointer."""
    return [
        _i(0x0F, 0, 8, 0),              # lui t0, %hi(result_pointer)
        _i(0x23, 8, 8, 0x68),           # lw t0, %lo(result_pointer)(t0)
        _i(0x0F, 0, 9, SENTINEL >> 16),
        _i(0x0D, 9, 9, SENTINEL),
        _i(0x2B, 8, 9, 0),              # sw t1, 0(t0)
        _i(0x23, 8, 2, 0),              # lw v0, 0(t0)
        _r(31, 0, 0, 0, 0x08),          # jr ra
        0,
    ]


def _helper_words_gap() -> list[int]:
    """Gap-mode helper: full real head, then transfer to AOT region B.

    The body stays complete in the image; only its TAIL differs from the aot
    mode so the future interpreter path demonstrably reaches registered AOT
    region B.  The dead jr/jal pair after the transfer guarantees region B is
    statically discoverable regardless of how direct-j seeding evolves, while
    remaining architecturally unreachable (the j above is unconditional).
    """
    return [
        _i(0x0F, 0, 8, 0),              # 0x28 lui t0, %hi(result_pointer)
        _i(0x23, 8, 8, 0x68),           # 0x2C lw t0, %lo(result_pointer)(t0)
        _i(0x0F, 0, 9, MARKER >> 16),   # 0x30 lui t1, %hi(MARKER)
        _i(0x0D, 9, 9, MARKER),         # 0x34 ori t1, t1, %lo(MARKER)
        _i(0x2B, 8, 9, 0),              # 0x38 sw t1, 0(t0)   (interpreter-era progress mark)
        _j(0x02, 0x58),                  # 0x3C j REGION_B     (transfer to AOT region B)
        0,                               # 0x40 nop            (delay slot)
        _r(31, 0, 0, 0, 0x08),          # 0x44 jr ra          (dead return path)
        0,                               # 0x48 nop
        _j(0x03, 0x58),                  # 0x4C jal REGION_B   (dead discovery anchor)
        0,                               # 0x50 nop            (its delay slot)
        0,                               # 0x54 pad
    ]


def _region_b_words() -> list[int]:
    """Registered AOT region B: writes the final sentinel and returns."""
    return [
        _i(0x0F, 0, 8, 0),              # 0x58 lui t0, %hi(result_pointer)
        _i(0x23, 8, 8, 0x68),           # 0x5C lw t0, %lo(result_pointer)(t0)
        _i(0x0F, 0, 9, SENTINEL >> 16),
        _i(0x0D, 9, 9, SENTINEL),
        _i(0x2B, 8, 9, 0),              # 0x68 sw t1, 0(t0)
        _i(0x23, 8, 2, 0),              # 0x6C lw v0, 0(t0)
        _r(31, 0, 0, 0, 0x08),          # 0x70 jr ra
        0,                               # 0x74 nop
    ]


def relocation_records(mode: str = "aot") -> list[tuple[int, int]]:
    """Return the ordered PSP type-A relocation table for one mode.

    In the gap mode the final load-bearing record still owns the result
    pointer, and mutation campaigns flip precisely that record.
    """

    records = [
        (0x08, relocation_info(R_MIPS_26, 0, 0)),  # entry -> helper
        (0x14, relocation_info(R_MIPS_26, 0, 0)),  # entry -> import stub
    ]
    if mode == "aot":
        records.extend(
            [
                (0x28, relocation_info(R_MIPS_HI16, 0, 1)),
                (0x2C, relocation_info(R_MIPS_LO16, 0, 1)),
            ]
        )
    else:
        records.extend(
            [
                (0x28, relocation_info(R_MIPS_HI16, 0, 1)),
                (0x2C, relocation_info(R_MIPS_LO16, 0, 1)),
                (0x3C, relocation_info(R_MIPS_26, 0, 0)),  # j REGION_B
                (0x4C, relocation_info(R_MIPS_26, 0, 0)),  # dead anchor jal
                (0x58, relocation_info(R_MIPS_HI16, 0, 1)),
                (0x5C, relocation_info(R_MIPS_LO16, 0, 1)),
            ]
        )
    records.extend(
        [
            (0x2C, relocation_info(R_MIPS_32, 1, 1)),  # module libstub
            (0x30, relocation_info(R_MIPS_32, 1, 1)),  # module libstubend
            (0x50, relocation_info(R_MIPS_32, 1, 1)),  # library name
            (0x5C, relocation_info(R_MIPS_32, 1, 1)),  # NID table
            (0x60, relocation_info(R_MIPS_32, 1, 0)),  # first import stub
            (0x68, relocation_info(R_MIPS_32, 1, 1)),  # result pointer (load-bearing)
        ]
    )
    return records


def build_text_segment(mode: str = "aot") -> bytes:
    text = bytearray(TEXT_FILE_SIZE)
    text[0 : len(_entry_words()) * 4] = _words(_entry_words())
    helper = _helper_words_aot() if mode == "aot" else _helper_words_gap()
    text[0x28 : 0x28 + len(helper) * 4] = _words(helper)
    if mode != "aot":
        text[0x58 : 0x58 + len(_region_b_words()) * 4] = _words(_region_b_words())
    text[0x78:0x80] = _words([0x03E00008, 0x0000000C])  # jr ra; syscall
    return bytes(text)


def _hi16(value: int) -> int:
    # MIPS %hi with the standard carry adjustment into %lo.
    return ((value >> 16) + ((value >> 15) & 1)) & 0xFFFF


def _patch_pointer_pair(words: list[int]) -> list[int]:
    """Patch the HI16/LO16 immediates of a lui/lw pointer pair the way the
    production relocation pass does."""
    patched = list(words)
    patched[0] = _i(0x0F, 0, 8, _hi16(RESULT_POINTER))
    patched[1] = _i(0x23, 8, 8, RESULT_POINTER & 0xFFFF)
    return patched


def _image_word_spec(mode: str) -> list[int]:
    """Expected RELOCATED text words starting at HELPER, per mode.

    Literal words must match exactly. The two pointer-immediate words are
    pre-patched here. The two jump words are returned as -1 sentinels: their
    opcode/target encoding is owned by the production R_MIPS_26 pass, so the
    qualification checks their DECODED transfer target instead of their bits
    (see verify). This keeps the qualification independent of prxload's
    internal encoding while still proving the guest bytes exist unaltered.
    """
    if mode == "aot":
        return _patch_pointer_pair(_helper_words_aot())
    words = _patch_pointer_pair(_helper_words_gap())
    words[0x3C - 0x28 >> 2] = -1   # j REGION_B      (encoding owned by reloc pass)
    words[0x4C - 0x28 >> 2] = -1   # dead anchor jal (encoding owned by reloc pass)
    return words


def expected_helper_bytes(mode: str = "aot") -> bytes:
    """Byte form of :func:`_image_word_spec` (jump words stay raw-fixture)."""
    if mode == "aot":
        return _words(_image_word_spec(mode))
    words = list(_image_word_spec(mode))
    raw = _helper_words_gap()
    words[0x3C - 0x28 >> 2] = raw[0x3C - 0x28 >> 2]
    words[0x4C - 0x28 >> 2] = raw[0x4C - 0x28 >> 2]
    return _words(words)


def expected_region_b_bytes() -> bytes:
    """RELOCATED region-B body for the gap-mode qualification."""
    return _words(_patch_pointer_pair(_region_b_words()))


def _check_region_bytes(image: bytes, offset: int, expected: bytes, label: str) -> None:
    actual = image[offset : offset + len(expected)]
    if actual == expected:
        return
    raise RuntimeError(f"guest {label} body is missing or altered in the relocated image")


def _check_jump_targets(image: bytes, mode: str) -> None:
    """Every gap-mode jump inside the helper tail must decode to a transfer
    targeting AOT region B after relocation."""
    start = HELPER - BASE
    spec = _image_word_spec(mode)
    for index, word in enumerate(spec):
        if word != -1:
            continue
        actual = struct.unpack_from("<I", image, start + index * 4)[0]
        opcode = (actual >> 26) & 0x3F
        target = ((actual & 0x03FFFFFF) << 2) | (BASE & 0x0F000000)
        if opcode not in (2, 3) or target != REGION_B:
            raise RuntimeError(
                f"guest transfer word at 0x{HELPER + index * 4:08x} does not "
                f"target registered AOT region B (op={opcode} target=0x{target:08x})"
            )


def emitted_function_count(mode: str) -> int:
    """Functions the pipeline emits for one mode (analyzer-discovered minus omitted)."""
    return 3 if mode == "aot" else 5


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


def build_prx(mode: str = "aot") -> bytes:
    text = build_text_segment(mode)
    data = build_data_segment()
    relocations = relocation_records(mode)
    relocation_bytes = b"".join(struct.pack("<II", *record) for record in relocations)

    text_section_size = TEXT_SECTION_SIZE_AOT if mode == "aot" else TEXT_SECTION_SIZE_GAP
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
            section(".text", 1, 6, 0, TEXT_FILE_OFFSET, text_section_size, 4),
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


def manifest_bytes(prx: bytes, psp_header: bytes, mode: str = "aot") -> bytes:
    records = relocation_records(mode)
    manifest = {
        "schema": 2,
        "kind": "source-owned-psp-production-smoke",
        "mode": mode,
        "base": f"0x{BASE:08x}",
        "entry": f"0x{ENTRY:08x}",
        "helper": f"0x{HELPER:08x}",
        "region_b": f"0x{REGION_B:08x}" if mode != "aot" else None,
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


class ModePlan:
    """One execution mode: guest tail layout + build/codegen choices + runtime expectation.

    ``codegen_args`` are appended to the ordinary codegen invocation by the
    production-smoke Make targets (through ``CODEGEN_USER_ARGS``), so a mode is
    purely a build-time declaration: nothing patches generated C after codegen
    and nothing substitutes host-side helpers.
    """

    def __init__(
        self,
        *,
        codegen_args: tuple[str, ...] = (),
        env: dict[str, str] | None = None,
        extra_driver_args: tuple[str, ...] = (),
        expect: str = "success",
    ):
        self.codegen_args = codegen_args
        self.env = env or {}
        self.extra_driver_args = extra_driver_args
        self.expect = expect


MODES: dict[str, ModePlan] = {
    "aot": ModePlan(
        env={"SR_DISPATCH_FATAL": "1", "SR_HLELOG": "1"},
    ),
    # Pre-interpreter acceptance: the intentional AOT omission must terminate
    # through the genuine production dispatch-miss path under SR_DISPATCH_FATAL.
    "aot-gap": ModePlan(
        codegen_args=(f"--omit-aot=0x{HELPER:08x}",),
        env={"SR_DISPATCH_FATAL": "1", "SR_HLELOG": "1", "SR_DISPLOG": "1"},
        expect="dispatch-miss",
    ),
}


def generate(out_dir: Path, mode: str = "aot") -> int:
    if mode not in MODES:
        raise RuntimeError(f"unknown mode {mode!r}")
    prx = build_prx(mode)
    psp_header = build_psp_header()
    outputs = {
        out_dir / "guest.prx": prx,
        out_dir / "guest.psp": psp_header,
        out_dir / "manifest.json": manifest_bytes(prx, psp_header, mode),
    }
    changed = [str(path) for path, data in outputs.items() if write_if_changed(path, data)]
    state = "updated" if changed else "unchanged"
    print(
        f"PRODUCTION_SMOKE_FIXTURE mode={mode} "
        f"state={state} prx_sha256={sha256(prx)} psp_sha256={sha256(psp_header)}"
    )
    return 0


def _read_manifest(fixture_dir: Path) -> dict[str, object]:
    return json.loads((fixture_dir / "manifest.json").read_text(encoding="ascii"))


def _normalized_map_targets(map_text: str) -> set[str]:
    """Normalize link-map path spellings so relative and absolute --build-dir
    values validate the same map. Comparison only; contamination scanning stays
    raw-text based."""

    tokens = set()
    for token in map_text.replace("\\", "/").split():
        tokens.add(os.path.normpath(os.path.abspath(token)))
    return tokens


def _artifact_stem(mode: str) -> str:
    # GAME_NAME used by the corresponding Make target; every build artifact and
    # the link map derive from it.
    return "production_smoke" if mode == "aot" else "production_smoke_gap"


def verify(build_dir: Path, mode: str = "aot") -> int:
    if mode not in MODES:
        raise RuntimeError(f"unknown mode {mode!r}")
    stem = _artifact_stem(mode)
    fixture_dir = build_dir / "fixture"
    manifest = _read_manifest(fixture_dir)
    prx = (fixture_dir / "guest.prx").read_bytes()
    psp_header = (fixture_dir / "guest.psp").read_bytes()
    if manifest != json.loads(manifest_bytes(prx, psp_header, mode)):
        raise RuntimeError("fixture manifest does not match the generated bytes")

    image_path = build_dir / f"{stem}_image.bin"
    executable = build_dir / f"{stem}.exe"
    map_path = build_dir / f"{stem}.map"
    main_c = build_dir / f"{stem}_recomp.c"
    funcs_h = build_dir / f"{stem}_recomp_funcs.h"
    imports_toml = build_dir / f"{stem}_imports.toml"
    required_files = (image_path, executable, map_path, main_c, funcs_h, imports_toml)
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        raise RuntimeError("production build is missing: " + ", ".join(missing))

    chunk_sources = sorted(build_dir.glob(f"{stem}_recomp_[0-9]*.c"))
    chunk_objects = sorted(build_dir.glob(f"{stem}_recomp_[0-9]*.o"))
    emitted_count = emitted_function_count(mode)
    if len(chunk_sources) != emitted_count or len(chunk_objects) != emitted_count:
        raise RuntimeError(
            f"expected {emitted_count} generated chunk sources and objects; "
            f"found sources={len(chunk_sources)} objects={len(chunk_objects)}"
        )

    main_text = main_c.read_text(encoding="ascii")
    if f"sr_register_all: starting {emitted_count} registrations" not in main_text:
        raise RuntimeError(f"generated registration count is not exactly {emitted_count}")
    for index in range(emitted_count):
        if f"sr_register_chunk_{index}();" not in main_text:
            raise RuntimeError(f"generated main omits chunk registration {index}")

    generated_text = "\n".join(path.read_text(encoding="ascii") for path in chunk_sources)
    if mode == "aot":
        emitted = (ENTRY, HELPER, IMPORT_STUB)
        omitted = ()
    else:
        # The two auxiliary starts (dead return path and dead discovery anchor
        # inside the helper tail) are ordinary guest bytes and stay emitted.
        emitted = (ENTRY, REGION_B, IMPORT_STUB)
        omitted = (HELPER,)
    for address in emitted:
        if f"f_{address:08x}" not in generated_text:
            raise RuntimeError(f"generated chunks omit function 0x{address:08x}")
    for address in omitted:
        if f"f_{address:08x}" in generated_text:
            raise RuntimeError(f"AOT omission leaked function 0x{address:08x} into generated C")
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

    # Qualification: the omitted region's bytes must still be present in the
    # relocated guest image inside the ordinary executable extent. An AOT gap
    # is an emission choice, never a byte removal.
    helper_offset = HELPER - BASE
    spec = _image_word_spec(mode)
    for index, expected in enumerate(spec):
        if expected == -1:
            continue  # jump encoding checked by _check_jump_targets below
        actual = struct.unpack_from("<I", image, helper_offset + index * 4)[0]
        if actual != (expected & 0xFFFFFFFF):
            raise RuntimeError(
                "guest helper body is missing or altered in the relocated image "
                f"(word {index} at 0x{HELPER + index * 4:08x})"
            )
    if mode == "aot-gap":
        _check_jump_targets(image, mode)
        _check_region_bytes(image, REGION_B - BASE, expected_region_b_bytes(), "region B")

    if mode == "aot-gap":
        seam = f"dispatch(s, 0x{HELPER:08x}u)"
        if seam not in generated_text:
            raise RuntimeError("AOT-gap generated code lacks the production dispatch seam")
        resume = f"f_{REGION_B:08x}"
        if resume not in generated_text:
            raise RuntimeError("AOT-gap generated code omits registered AOT region B")

    map_text = map_path.read_text(encoding="utf-8", errors="replace")
    if "gate_stub" in map_text:
        raise RuntimeError("reduced gate_stub leaked into the production link")
    normalized_targets = _normalized_map_targets(map_text)
    absent = []
    for name in (
        f"{stem}_recomp.o",
        f"{stem}_recomp_0.o",
        f"{stem}_recomp_1.o",
        f"{stem}_recomp_2.o",
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
    ):
        candidate = os.path.normpath(os.path.abspath(str(build_dir / name)))
        if candidate not in normalized_targets:
            absent.append(name)
    if absent:
        raise RuntimeError("production link map omits: " + ", ".join(absent))

    print(
        f"PRODUCTION_SMOKE_VERIFY mode={mode} status=PASS "
        f"functions={emitted_count} chunks={emitted_count} "
        f"relocations={len(relocation_records(mode))} image_sha256={sha256(image)}"
    )
    return 0


def assert_dispatch_miss_evidence(combined: str, returncode: int) -> None:
    """Pre-interpreter acceptance for the aot-gap mode.

    Requires genuine production dispatch-miss evidence for the omitted helper
    address and rejects every look-alike failure:
      * a clean success (no miss occurred => the omission did not take effect);
      * a weakened/stubbed dispatch path (miss logged but SR_DISPATCH_FATAL
        ignored, process exits 0);
      * a transfer defect (the dispatcher was never handed the helper address).
    """

    required = (
        f"DISPATCH 0x{HELPER:08x} from",
        "NONPLT_MISS",
    )
    missing = [marker for marker in required if marker not in combined]
    if missing:
        raise RuntimeError(
            "dispatch-miss evidence omits: " + ", ".join(missing)
        )
    forbidden = (
        "DRIVER_EXPECT_U32",
        f"HLE: calling sceKernelSetCompiledSdkVersion (0x{NID:08x})",
    )
    present = [marker for marker in forbidden if marker in combined]
    if present:
        raise RuntimeError(
            "dispatch-miss run unexpectedly contains: " + ", ".join(present)
        )
    if returncode == 0:
        raise RuntimeError(
            "production dispatch miss did not terminate the process "
            "(SR_DISPATCH_FATAL weakened or dispatch replaced)"
        )


def run(build_dir: Path, mode: str = "aot") -> int:
    plan = MODES.get(mode)
    if plan is None:
        raise RuntimeError(
            f"unknown execution mode {mode!r}; known modes: " + ", ".join(sorted(MODES))
        )
    executable = build_dir / f"{_artifact_stem(mode)}.exe"
    image_path = build_dir / f"{_artifact_stem(mode)}_image.bin"
    command = [
        str(executable),
        "--image",
        str(image_path),
        f"0x{BASE:08x}",
        f"0x{ENTRY:08x}",
        "none",
        "none",
        "--sched",
        *plan.extra_driver_args,
        f"--expect-u32=0x{RESULT:08x}:0x{SENTINEL:08x}",
    ]
    environment = os.environ.copy()
    for key, value in plan.env.items():
        if value == "":
            environment.pop(key, None)
        else:
            environment[key] = value
    completed = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
    log_suffix = "" if mode == "aot" else f".{mode}"
    log_stem = _artifact_stem(mode)
    write_if_changed(build_dir / f"{log_stem}{log_suffix}.stdout.log", completed.stdout.encode("utf-8"))
    write_if_changed(build_dir / f"{log_stem}{log_suffix}.stderr.log", completed.stderr.encode("utf-8"))
    combined = completed.stdout + completed.stderr
    if plan.expect == "dispatch-miss":
        assert_dispatch_miss_evidence(combined, completed.returncode)
        print(
            f"PRODUCTION_SMOKE_RUN mode={mode} status=PASS "
            f"(expected production dispatch miss at 0x{HELPER:08x}, "
            f"exit={completed.returncode})"
        )
        return 0
    if completed.returncode != 0:
        sys.stderr.write(combined)
        raise RuntimeError(f"production smoke runtime exited {completed.returncode}")
    markers = (
        "BOOT_EVENT phase=init public_safe=1",
        f"BOOT_EVENT phase=image_loaded entry=0x{ENTRY:08x}",
        f"sr_register_all: starting {emitted_function_count(mode)} registrations",
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
        f"PRODUCTION_SMOKE_RUN mode={mode} status=PASS "
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
        command_parser.add_argument("--mode", default="aot", choices=sorted(MODES))
    generate_parser.add_argument("--mode", default="aot", choices=sorted(MODES))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "generate":
            return generate(args.out_dir, mode=args.mode)
        if args.command == "verify":
            return verify(args.build_dir, mode=args.mode)
        if args.command == "run":
            return run(args.build_dir, mode=args.mode)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        sys.stderr.write(f"PRODUCTION_SMOKE_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
