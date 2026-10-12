# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate and qualify the source-owned PSP display smoke guest.

The committed fixture is this recipe, not a binary.  It deterministically emits
a small ELF32 PSP PRX/``~PSP`` pair into the ignored build tree.  Unlike
``fixtures/production_smoke``, whose guest proves the AOT/interpreter seam with
a sentinel word, this guest proves the *presentation* path: it submits one
source-owned GE list, fills the PSP framebuffer with a moving pattern, hands
the buffer to ``sceDisplaySetFrameBuf`` and waits for vblank once per frame.

That exercises the production chain the compute fixtures never touch --
``sceGeListEnQueue`` -> GE command walk and ``sceDisplaySetFrameBuf`` -> display
latch -> vblank -> ``gui_present`` -- with no external toolchain, no retail
disc, and no private input.  The guest is
hand-assembled MIPS here for exactly the reason ``production_smoke`` is: the
fixture must build on any host in CI, and a PSPDEV toolchain is an external
input this repository does not require.

Headless (``run``) the gate asserts the guest-visible framebuffer word that the
final frame must have written, so the pattern is proven without a window.  With
``--gui`` the same image renders to the SDL3/Vulkan presenter, which is what
makes this fixture the first public-scope artifact the native player can launch
and show.

Guest program
-------------
::

    list = [GE_PRIM(type=0, vertices=0), GE_FINISH, GE_END]
    sceGeListEnQueue(list, stall=0, callback=0, callback_arg=0)
    frame = 0
    do {
        for (i = 0; i < 512 * 272; i++)
            vram[i] = colour((i >> 4) + frame);
        sceDisplaySetFrameBuf(0x04000000, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
        sceDisplayWaitVblankStart();
    } while (++frame < FRAMES);

``colour(v)`` packs ``v`` into an ABGR8888 word. The index is the linear pixel
number, so the output is horizontal striping that shifts by one band per frame.
The gate proves frame progression through the final framebuffer word; it does
not claim a particular spatial arrangement.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[2]

BASE = 0x08810000
ENTRY = BASE

# Build artifacts are named after the catalog title id because that is the only
# shape src/core/nk_launch.c probes for. Keep this equal to the Makefile's
# DISPLAY_SMOKE_NAME and to the "id" in assets/titles/display-smoke.json.
ARTIFACT_STEM = "display-smoke-v1"
TITLE_ID = ARTIFACT_STEM

# Guest text layout. The entry body occupies [0, TEXT_SECTION_SIZE); three
# import stubs form the .sceStub.text section at STUB_OFFSET.
TEXT_SECTION_SIZE = 0xD8
STUB_OFFSET = 0xE0
SETFB_STUB = STUB_OFFSET
VBLANK_STUB = STUB_OFFSET + 8
GE_LIST_ENQUEUE_STUB = STUB_OFFSET + 16
TEXT_FILE_SIZE = 0x100

# Guest data layout, mirroring the PSP module-info/import-table shape.
DATA_VADDR = 0x1000
MODULE_INFO_SIZE = 52
LIBRARY_NAME_OFFSET = 0x34
LIBSTUB_OFFSET = 0x50
GE_LIBRARY_NAME_OFFSET = 0x40
GE_LIBSTUB_OFFSET = 0x64
LIBSTUB_END_OFFSET = 0x78
NID_TABLE_OFFSET = 0x78
GE_NID_TABLE_OFFSET = 0x80
DATA_FILE_SIZE = 0x84
DATA_MEMORY_SIZE = 0x90
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE

TEXT_FILE_OFFSET = 0x100
DATA_FILE_OFFSET = 0x200
RELOCATION_FILE_OFFSET = DATA_FILE_OFFSET + DATA_FILE_SIZE

LIBRARY = "sceDisplay"
GE_LIBRARY = "sceGe_user"
NID_SET_FRAME_BUF = 0x289D82FE
NID_WAIT_VBLANK_START = 0x984C27E7
NID_GE_LIST_ENQUEUE = 0xAB49E76A
NIDS = (NID_SET_FRAME_BUF, NID_WAIT_VBLANK_START, NID_GE_LIST_ENQUEUE)

# Presentation parameters. Stride must be a multiple of 64 and the format must
# be 0..3; src/rt/hle.c rejects anything else, which is part of what the gate
# proves is being honoured.
FRAMEBUFFER = 0x04000000
GE_LIST = 0x08900000
STRIDE = 512
PIXEL_FORMAT = 3  # PSP_DISPLAY_PIXEL_FORMAT_8888
PIXELS = STRIDE * 272
DEFAULT_FRAMES = 240

_HOST_PRESENT_SUBMITTED_RE = re.compile(
    r"^HOST_PRESENT_SUBMITTED f=\d+ buf=0x[0-9a-fA-F]{8} "
    r"fmt=[0-3] stride=\d+$"
)
_OFFSCREEN_FRAME_PRESENT_RE = re.compile(
    r"^BOOT_EVENT phase=frame_present backend=offscreen frame=\d+"
    r"(?: t_ns=\d+)?$"
)

R_MIPS_NONE = 0
R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
SHT_PRX_RELOC = 0x700000A0

# Register numbers used by the hand-assembled guest.
ZERO, A0, A1, A2, A3 = 0, 4, 5, 6, 7
T0, T1, T2, T3, T4, T5, T6, T7 = 8, 9, 10, 11, 12, 13, 14, 15
S0, T8, T9, SP, RA = 16, 24, 25, 29, 31


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


def expected_colour(value: int) -> int:
    """The ABGR8888 word the guest stores for a band index.

    Kept in Python so the gate asserts the same arithmetic the MIPS performs
    rather than a copied constant.
    """
    red = value & 0xFF
    green = red ^ 0xFF
    blue = (red << 1) & 0xFF
    return 0xFF000000 | (blue << 16) | (green << 8) | red


def final_frame_first_pixel(frames: int) -> int:
    """Framebuffer word at 0x04000000 once the last frame has been drawn."""
    return expected_colour(frames - 1)


def _entry_words(frames: int) -> list[int]:
    """The guest submits a GE list, then fills and presents each frame."""
    if not 1 <= frames <= 0x7FFF:
        raise ValueError(f"frame count {frames} does not fit a signed 16-bit immediate")
    return [
        # prologue
        _i(0x09, SP, SP, -32),           # 0x00 addiu sp, sp, -32
        _i(0x2B, SP, RA, 28),            # 0x04 sw ra, 28(sp)
        _i(0x2B, SP, S0, 24),            # 0x08 sw s0, 24(sp)
        _r(ZERO, ZERO, S0, 0, 0x21),     # 0x0C addu s0, zero, zero   (frame = 0)
        # ge_list_setup (0x10): one no-vertex PRIM, FINISH, END.
        _i(0x0F, ZERO, T0, GE_LIST >> 16),  # 0x10 lui t0, 0x0890 (GE list)
        _i(0x0F, ZERO, T1, 0x0400),      # 0x14 lui t1, 0x0400 (PRIM type 0, count 0)
        _i(0x2B, T0, T1, 0),             # 0x18 sw t1, 0(t0)
        _i(0x0F, ZERO, T1, 0x0F00),      # 0x1C lui t1, 0x0f00 (FINISH)
        _i(0x2B, T0, T1, 4),             # 0x20 sw t1, 4(t0)
        _i(0x0F, ZERO, T1, 0x0C00),      # 0x24 lui t1, 0x0c00 (END)
        _i(0x2B, T0, T1, 8),             # 0x28 sw t1, 8(t0)
        _r(ZERO, T0, A0, 0, 0x21),       # 0x2C addu a0, t0, zero
        _r(ZERO, ZERO, A1, 0, 0x21),     # 0x30 addu a1, zero, zero
        _r(ZERO, ZERO, A2, 0, 0x21),     # 0x34 addu a2, zero, zero
        _r(ZERO, ZERO, A3, 0, 0x21),     # 0x38 addu a3, zero, zero
        _j(0x03, GE_LIST_ENQUEUE_STUB),  # 0x3C jal sceGeListEnQueue
        0,                               # 0x40 nop (delay slot)
        # frame_loop (0x44)
        _i(0x0F, ZERO, T0, 0x0400),      # 0x44 lui t0, 0x0400 (vram cursor)
        _r(ZERO, ZERO, T1, 0, 0x21),     # 0x48 addu t1, zero, zero (i = 0)
        _i(0x0F, ZERO, T2, PIXELS >> 16),  # 0x4C lui t2, hi(PIXELS)
        _i(0x0D, T2, T2, PIXELS & 0xFFFF),  # 0x50 ori t2, t2, lo(PIXELS)
        # pixel_loop (0x54)
        _r(ZERO, T1, T3, 4, 0x02),       # 0x54 srl t3, t1, 4
        _r(T3, S0, T3, 0, 0x21),         # 0x58 addu t3, t3, s0
        _i(0x0C, T3, T3, 0xFF),          # 0x5C andi t3, t3, 0xFF (red)
        _i(0x0E, T3, T4, 0xFF),          # 0x60 xori t4, t3, 0xFF (green)
        _r(ZERO, T3, T5, 1, 0x00),       # 0x64 sll t5, t3, 1
        _i(0x0C, T5, T5, 0xFF),          # 0x68 andi t5, t5, 0xFF (blue)
        _r(ZERO, T5, T6, 16, 0x00),      # 0x6C sll t6, t5, 16
        _r(ZERO, T4, T7, 8, 0x00),       # 0x70 sll t7, t4, 8
        _r(T6, T7, T6, 0, 0x25),         # 0x74 or t6, t6, t7
        _r(T6, T3, T6, 0, 0x25),         # 0x78 or t6, t6, t3
        _i(0x0F, ZERO, T8, 0xFF00),      # 0x7C lui t8, 0xFF00 (alpha)
        _r(T6, T8, T6, 0, 0x25),         # 0x80 or t6, t6, t8
        _i(0x2B, T0, T6, 0),             # 0x84 sw t6, 0(t0)
        _i(0x09, T0, T0, 4),             # 0x88 addiu t0, t0, 4
        _i(0x09, T1, T1, 1),             # 0x8C addiu t1, t1, 1
        _i(0x05, T1, T2, -16),           # 0x90 bne t1, t2, pixel_loop
        0,                               # 0x94 nop (delay slot)
        # present
        _i(0x0F, ZERO, A0, FRAMEBUFFER >> 16),  # 0x98 lui a0, 0x0400
        _i(0x09, ZERO, A1, STRIDE),      # 0x9C addiu a1, zero, 512
        _i(0x09, ZERO, A2, PIXEL_FORMAT),  # 0xA0 addiu a2, zero, 3
        _i(0x09, ZERO, A3, 1),           # 0xA4 addiu a3, zero, 1 (sync = 1)
        _j(0x03, SETFB_STUB),            # 0xA8 jal sceDisplaySetFrameBuf
        0,                               # 0xAC nop (delay slot)
        _j(0x03, VBLANK_STUB),           # 0xB0 jal sceDisplayWaitVblankStart
        0,                               # 0xB4 nop (delay slot)
        _i(0x09, S0, S0, 1),             # 0xB8 addiu s0, s0, 1
        _i(0x0A, S0, T9, frames),        # 0xBC slti t9, s0, FRAMES
        _i(0x05, T9, ZERO, -32),         # 0xC0 bne t9, zero, frame_loop
        0,                               # 0xC4 nop (delay slot)
        # epilogue
        _i(0x23, SP, RA, 28),            # 0xC8 lw ra, 28(sp)
        _i(0x23, SP, S0, 24),            # 0xCC lw s0, 24(sp)
        _r(RA, 0, 0, 0, 0x08),           # 0xD0 jr ra
        _i(0x09, SP, SP, 32),            # 0xD4 addiu sp, sp, 32 (delay slot)
    ]


def relocation_records() -> list[tuple[int, int]]:
    """The ordered PSP type-A relocation table.

    Segment 0 is .text, segment 1 is .data. The three R_MIPS_26 records rebase the
    calls to the import stubs; the R_MIPS_32 records rebase the module-info and
    import-table pointers. The stub-table pointer is the only data word that
    targets the text segment.
    """
    return [
        (0x3C, relocation_info(R_MIPS_26, 0, 0)),  # jal sceGeListEnQueue
        (0xA8, relocation_info(R_MIPS_26, 0, 0)),  # jal sceDisplaySetFrameBuf
        (0xB0, relocation_info(R_MIPS_26, 0, 0)),  # jal sceDisplayWaitVblankStart
        (0x2C, relocation_info(R_MIPS_32, 1, 1)),  # module libstub
        (0x30, relocation_info(R_MIPS_32, 1, 1)),  # module libstubend
        (0x50, relocation_info(R_MIPS_32, 1, 1)),  # display library name
        (0x5C, relocation_info(R_MIPS_32, 1, 1)),  # display NID table
        (0x60, relocation_info(R_MIPS_32, 1, 0)),  # display import stubs (into .text)
        (0x64, relocation_info(R_MIPS_32, 1, 1)),  # GE library name
        (0x70, relocation_info(R_MIPS_32, 1, 1)),  # GE NID table
        (0x74, relocation_info(R_MIPS_32, 1, 0)),  # GE import stub (into .text)
    ]


def build_text_segment(frames: int) -> bytes:
    entry = _entry_words(frames)
    if len(entry) * 4 != TEXT_SECTION_SIZE:
        raise AssertionError(
            f"entry body is {len(entry) * 4:#x} bytes but .text is declared {TEXT_SECTION_SIZE:#x}"
        )
    text = bytearray(TEXT_FILE_SIZE)
    text[0 : len(entry) * 4] = _words(entry)
    # Three 8-byte import slots: the recompiler pairs each with its NID and routes
    # the call to the HLE handler, so the syscall word is never executed.
    text[STUB_OFFSET : STUB_OFFSET + 24] = _words(
        [0x03E00008, 0x0000000C] * len(NIDS)
    )
    return bytes(text)


def build_data_segment() -> bytes:
    data = bytearray(DATA_FILE_SIZE)
    module_name = b"display-smoke-v1"
    library_name = LIBRARY.encode("ascii") + b"\0"
    ge_library_name = GE_LIBRARY.encode("ascii") + b"\0"
    data[0:MODULE_INFO_SIZE] = struct.pack(
        "<HH28s5I",
        0,
        0x0100,
        module_name + b"\0" * (28 - len(module_name)),
        0,
        0,
        0,
        LIBSTUB_OFFSET,
        LIBSTUB_END_OFFSET,
    )
    if len(library_name) > LIBSTUB_OFFSET - LIBRARY_NAME_OFFSET:
        raise AssertionError("library name no longer fits the fixed layout")
    if len(ge_library_name) > GE_LIBSTUB_OFFSET - GE_LIBRARY_NAME_OFFSET:
        raise AssertionError("GE library name no longer fits the fixed layout")
    data[LIBRARY_NAME_OFFSET : LIBRARY_NAME_OFFSET + len(library_name)] = library_name
    data[GE_LIBRARY_NAME_OFFSET : GE_LIBRARY_NAME_OFFSET + len(ge_library_name)] = ge_library_name
    data[LIBSTUB_OFFSET : LIBSTUB_OFFSET + 20] = struct.pack(
        "<IHHBBHII",
        LIBRARY_NAME_OFFSET,
        0x0101,
        0x0009,
        5,
        0,
        2,
        NID_TABLE_OFFSET,
        STUB_OFFSET,
    )
    data[GE_LIBSTUB_OFFSET : GE_LIBSTUB_OFFSET + 20] = struct.pack(
        "<IHHBBHII",
        GE_LIBRARY_NAME_OFFSET,
        0x0101,
        0x0009,
        5,
        0,
        1,
        GE_NID_TABLE_OFFSET,
        GE_LIST_ENQUEUE_STUB,
    )
    struct.pack_into("<2I", data, NID_TABLE_OFFSET, *NIDS[:2])
    struct.pack_into("<I", data, GE_NID_TABLE_OFFSET, NID_GE_LIST_ENQUEUE)
    return bytes(data)


def build_prx(frames: int = DEFAULT_FRAMES) -> bytes:
    text = build_text_segment(frames)
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
                "<8I", 1, DATA_FILE_OFFSET, DATA_VADDR, DATA_VADDR,
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
            section(".text", 1, 6, 0, TEXT_FILE_OFFSET, TEXT_SECTION_SIZE, 4),
            section(
                ".sceStub.text", 1, 6, STUB_OFFSET,
                TEXT_FILE_OFFSET + STUB_OFFSET, len(NIDS) * 8, 4,
            ),
            section(
                ".rodata.sceModuleInfo", 1, 2, DATA_VADDR,
                DATA_FILE_OFFSET, MODULE_INFO_SIZE, 4,
            ),
            section(
                ".rodata", 1, 2, DATA_VADDR + LIBRARY_NAME_OFFSET,
                DATA_FILE_OFFSET + LIBRARY_NAME_OFFSET,
                LIBSTUB_OFFSET - LIBRARY_NAME_OFFSET, 1,
            ),
            section(
                ".lib.stub", 1, 2, DATA_VADDR + LIBSTUB_OFFSET,
                DATA_FILE_OFFSET + LIBSTUB_OFFSET,
                LIBSTUB_END_OFFSET - LIBSTUB_OFFSET, 4,
            ),
            section(
                ".rodata.sceNid", 1, 2, DATA_VADDR + NID_TABLE_OFFSET,
                DATA_FILE_OFFSET + NID_TABLE_OFFSET, len(NIDS) * 4, 4,
            ),
            section(
                ".data", 1, 3, DATA_VADDR + NID_TABLE_OFFSET + len(NIDS) * 4,
                DATA_FILE_OFFSET + NID_TABLE_OFFSET + len(NIDS) * 4,
                DATA_FILE_SIZE - NID_TABLE_OFFSET - len(NIDS) * 4, 4,
            ),
            section(
                ".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0,
                RELOCATION_FILE_OFFSET, len(relocation_bytes), 4, 8,
            ),
            section(
                ".bss", 8, 3, DATA_VADDR + DATA_FILE_SIZE,
                DATA_FILE_OFFSET + DATA_FILE_SIZE, BSS_SIZE, 16,
            ),
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


def evidence_failure(
    evidence: str, required: tuple[str, ...], forbidden: tuple[str, ...]
) -> str | None:
    """Describe how `evidence` misses the gate's contract, or None when it holds.

    Every runtime/player gate in this recipe asks the same two questions of the
    text it captured -- did the lifecycle prove itself, and did it stay clear of
    the forbidden markers?  Keeping the judgement here means a new gate cannot
    quietly check only one half of it, and the caller decides how much evidence to
    echo before it fails.
    """
    missing = [marker for marker in required if marker not in evidence]
    if missing:
        return "omits: " + ", ".join(missing)
    present = [marker for marker in forbidden if marker in evidence]
    if present:
        return "contains: " + ", ".join(present)
    return None


def manifest_bytes(prx: bytes, psp_header: bytes, frames: int) -> bytes:
    manifest = {
        "schema": 1,
        "kind": "source-owned-psp-display-smoke",
        "title_id": TITLE_ID,
        "base": f"0x{BASE:08x}",
        "entry": f"0x{ENTRY:08x}",
        "libraries": {
            LIBRARY: [f"0x{nid:08x}" for nid in NIDS[:2]],
            GE_LIBRARY: [f"0x{NID_GE_LIST_ENQUEUE:08x}"],
        },
        "stub_table": f"0x{BASE + STUB_OFFSET:08x}",
        "framebuffer": f"0x{FRAMEBUFFER:08x}",
        "stride": STRIDE,
        "pixel_format": PIXEL_FORMAT,
        "frames": frames,
        "final_first_pixel": f"0x{final_frame_first_pixel(frames):08x}",
        "load_segments": 2,
        "bss_size": BSS_SIZE,
        "relocation_count": len(relocation_records()),
        "prx_sha256": sha256(prx),
        "psp_header_sha256": sha256(psp_header),
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("ascii")


def generate(out_dir: Path, frames: int = DEFAULT_FRAMES) -> int:
    prx = build_prx(frames)
    psp_header = build_psp_header()
    outputs = {
        out_dir / "guest.prx": prx,
        out_dir / "guest.psp": psp_header,
        out_dir / "manifest.json": manifest_bytes(prx, psp_header, frames),
    }
    changed = [str(path) for path, data in outputs.items() if write_if_changed(path, data)]
    state = "updated" if changed else "unchanged"
    print(
        f"DISPLAY_SMOKE_FIXTURE state={state} frames={frames} "
        f"prx_sha256={sha256(prx)} psp_sha256={sha256(psp_header)}"
    )
    return 0


def _read_manifest(fixture_dir: Path) -> dict[str, object]:
    return json.loads((fixture_dir / "manifest.json").read_text(encoding="ascii"))


def verify(build_dir: Path) -> int:
    """Qualify the built image against the recipe.

    The image is the rebased/relocated guest the driver actually loads, so this
    checks the two things a silent relocation bug would break: the import calls
    must resolve to the declared stub slots, and the stub slots must still be
    the import pair the NID table names.
    """
    fixture = build_dir / "fixture"
    manifest = _read_manifest(fixture)
    image_path = build_dir / f"{ARTIFACT_STEM}_image.bin"
    image = image_path.read_bytes()
    if len(image) < TEXT_FILE_SIZE:
        raise RuntimeError(f"image {image_path} is {len(image)} bytes, shorter than .text")

    for offset, expected_target, label in (
        (0x3C, GE_LIST_ENQUEUE_STUB, "sceGeListEnQueue"),
        (0xA8, SETFB_STUB, "sceDisplaySetFrameBuf"),
        (0xB0, VBLANK_STUB, "sceDisplayWaitVblankStart"),
    ):
        word = struct.unpack_from("<I", image, offset)[0]
        opcode = (word >> 26) & 0x3F
        target = ((word & 0x03FFFFFF) << 2) | (BASE & 0xF0000000)
        if opcode != 0x03 or target != BASE + expected_target:
            raise RuntimeError(
                f"import call for {label} at 0x{BASE + offset:08x} does not target its stub "
                f"(op={opcode} target=0x{target:08x} expected=0x{BASE + expected_target:08x})"
            )

    for index in range(len(NIDS)):
        slot = STUB_OFFSET + index * 8
        jr_ra, syscall = struct.unpack_from("<2I", image, slot)
        if jr_ra != 0x03E00008 or syscall != 0x0000000C:
            raise RuntimeError(
                f"import stub slot {index} at 0x{BASE + slot:08x} is not a jr ra/syscall pair"
            )

    if manifest["prx_sha256"] != sha256((fixture / "guest.prx").read_bytes()):
        raise RuntimeError("fixture manifest does not describe the emitted guest.prx")

    print(
        f"DISPLAY_SMOKE_VERIFY status=PASS imports={len(NIDS)} "
        f"frames={manifest['frames']} image_sha256={sha256(image)}"
    )
    return 0


def offscreen_present_evidence_failure(combined: str) -> str | None:
    """Require native offscreen acceptance markers to follow accepted sink writes."""
    lines = combined.splitlines()
    frame_indices = []
    for index, line in enumerate(lines):
        if not line.startswith("BOOT_EVENT phase=frame_present backend=offscreen "):
            continue
        if not _OFFSCREEN_FRAME_PRESENT_RE.fullmatch(line):
            return f"malformed offscreen frame_present marker at line {index + 1}"
        frame_indices.append(index)
    submission_indices = []
    for index, line in enumerate(lines):
        if not line.startswith("HOST_PRESENT_SUBMITTED "):
            continue
        if not _HOST_PRESENT_SUBMITTED_RE.fullmatch(line):
            return f"malformed HOST_PRESENT_SUBMITTED marker at line {index + 1}"
        submission_indices.append(index)
    if not frame_indices:
        return "omits: BOOT_EVENT phase=frame_present backend=offscreen"
    if not submission_indices:
        return "omits: HOST_PRESENT_SUBMITTED"
    if len(frame_indices) != len(submission_indices):
        return (
            "offscreen frame/submit count differs: "
            f"frames={len(frame_indices)} submissions={len(submission_indices)}"
        )
    if any(
        frame >= submission
        for frame, submission in zip(frame_indices, submission_indices, strict=True)
    ):
        return "HOST_PRESENT_SUBMITTED does not follow frame_present"
    return None


def run(
    build_dir: Path,
    gui: bool = False,
    offscreen: bool = False,
    flight_output: Path | None = None,
) -> int:
    # The runtime is spawned with cwd=ROOT, so a relative build_dir would make the
    # CHILD resolve the executable/image paths against ROOT rather than the
    # caller's cwd. Resolve once so every path handed to subprocess is absolute.
    build_dir = build_dir.resolve()
    fixture = build_dir / "fixture"
    manifest = _read_manifest(fixture)
    frames = int(manifest["frames"])
    expected = final_frame_first_pixel(frames)
    executable = (build_dir / f"{ARTIFACT_STEM}.exe").resolve()
    if not executable.exists():
        executable = (build_dir / ARTIFACT_STEM).resolve()
    image_path = (build_dir / f"{ARTIFACT_STEM}_image.bin").resolve()
    if offscreen and not gui:
        raise ValueError("--offscreen requires --gui")
    command = [
        str(executable),
        "--image",
        str(image_path),
        f"0x{BASE:08x}",
        f"0x{ENTRY:08x}",
        "none",
        "none",
        *(("--sched", "--gui") if offscreen else ("--gui",) if gui else ("--sched",)),
        f"--expect-u32=0x{FRAMEBUFFER:08x}:0x{expected:08x}",
    ]
    env = os.environ.copy()
    if offscreen:
        # This is an explicit no-window route. Override ambient selectors so a
        # developer's interactive presenter or SDL selector settings cannot
        # change the evidence. Pin both modern and legacy SDL spellings because
        # the no-window contract must survive a future shared initialization seam.
        env.update({
            "SDL_VIDEO_DRIVER": "dummy",
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIO_DRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "SR_VIDEO": "offscreen",
            "SR_PRESENT_TRACE": "1",
        })
    if flight_output is not None:
        env.update({
            "SR_FLIGHT": "ge,present;4096",
            "SR_FLIGHT_OUTPUT": str(flight_output.resolve()),
            # The present-event count below is frames // 2, i.e. a 30 Hz host cap over a
            # 60 Hz guest. Pin it: the unset default is the PSP scanout rate, which would
            # present nearly every frame and make the count depend on host timing.
            "SR_FPS_CAP": "30",
        })
    completed = subprocess.run(
        command, cwd=ROOT, env=env, capture_output=True, text=True
    )
    mode = "gui-offscreen" if offscreen else "gui" if gui else "headless"
    # Each route keeps its own logs so a later route cannot erase an earlier one's evidence.
    log_stem = ARTIFACT_STEM if mode == "headless" else f"{ARTIFACT_STEM}.{mode}"
    write_if_changed(build_dir / f"{log_stem}.stdout.log", completed.stdout.encode("utf-8"))
    write_if_changed(build_dir / f"{log_stem}.stderr.log", completed.stderr.encode("utf-8"))
    combined = completed.stdout + completed.stderr
    if completed.returncode != 0:
        sys.stderr.write(combined)
        raise RuntimeError(f"display smoke runtime exited {completed.returncode}")
    markers = (
        "BOOT_EVENT phase=init public_safe=1",
        f"BOOT_EVENT phase=image_loaded entry=0x{ENTRY:08x}",
        f"BOOT_EVENT phase=runtime_registered entry=0x{ENTRY:08x}",
        (
            f"DRIVER_EXPECT_U32 addr=0x{FRAMEBUFFER:08x} got=0x{expected:08x} "
            f"expected=0x{expected:08x} status=PASS"
        ),
    )
    forbidden = ("UNKNOWN NID", "NONPLT_MISS", "INTERP_REJECT", "status=FAIL")
    if failure := evidence_failure(combined, markers, forbidden):
        sys.stderr.write(combined)
        raise RuntimeError(f"runtime evidence {failure}")
    if offscreen and (failure := offscreen_present_evidence_failure(combined)):
        sys.stderr.write(combined)
        raise RuntimeError(f"offscreen presenter evidence {failure}")
    print(
        f"DISPLAY_SMOKE_RUN status=PASS mode={mode} "
        f"frames={frames} framebuffer=0x{FRAMEBUFFER:08x} value=0x{expected:08x}"
    )
    return 0


def run_vramdump(build_dir: Path) -> int:
    """Prove present-aligned VRAM capture and the host PNG decoder on this fixture."""
    build_dir = build_dir.resolve()
    executable = (build_dir / f"{ARTIFACT_STEM}.exe").resolve()
    if not executable.exists():
        executable = (build_dir / ARTIFACT_STEM).resolve()
    image_path = (build_dir / f"{ARTIFACT_STEM}_image.bin").resolve()
    if not executable.is_file() or not image_path.is_file():
        raise RuntimeError("display-smoke runtime and generated image must be built first")

    command = [
        str(executable), "--image", str(image_path),
        f"0x{BASE:08x}", f"0x{ENTRY:08x}", "none", "none", "--sched", "--gui",
    ]

    def launch(work_dir: Path, *, capture_vblank: int | None) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        for name in ("SR_FBDUMP", "SR_FIRST_FRAME_DUMP", "SR_VRAMDUMP",
                     "SR_VRAMDUMP_DIR", "SR_EXIT_AT_VBLANK"):
            env.pop(name, None)
        env.update({
            "SDL_VIDEO_DRIVER": "dummy", "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIO_DRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
            "SR_VIDEO": "offscreen", "SR_PRESENT_TRACE": "1",
            "SR_FIRST_FRAME_DUMP": "1",
        })
        if capture_vblank is not None:
            env["SR_VRAMDUMP"] = capture_vblank
            env["SR_VRAMDUMP_DIR"] = str(work_dir / "capture")
            (work_dir / "capture").mkdir()
        completed = subprocess.run(
            command, cwd=work_dir, env=env, capture_output=True, text=True,
            timeout=30, check=False,
        )
        if completed.returncode:
            raise RuntimeError(
                f"display-smoke {'capture' if capture_vblank is not None else 'baseline'} exited "
                f"{completed.returncode}: {completed.stdout}{completed.stderr}"
            )
        combined = completed.stdout + completed.stderr
        if "HOST_PRESENT_SUBMITTED" not in combined:
            raise RuntimeError(
                "display-smoke VRAM check did not observe a submitted present: " + combined
            )
        return completed

    with tempfile.TemporaryDirectory(prefix="display-smoke-vramdump-") as temporary:
        root = Path(temporary)
        baseline_dir = root / "baseline"
        capture_dir = root / "capture-run"
        baseline_dir.mkdir()
        capture_dir.mkdir()
        baseline = launch(baseline_dir, capture_vblank=None)
        presents = re.findall(r"HOST_PRESENT_SUBMITTED f=(\d+)",
                              baseline.stdout + baseline.stderr)
        if not presents:
            raise RuntimeError("display-smoke baseline did not report its first presented vblank")
        selected_vblank = int(presents[0])
        selected_vblanks = ",".join(str(vblank) for vblank in range(selected_vblank,
                                                                     selected_vblank + 8))
        captured = launch(capture_dir, capture_vblank=selected_vblanks)
        baseline_ppm = baseline_dir / "frame_first.ppm"
        capture_ppm = capture_dir / "frame_first.ppm"
        if not baseline_ppm.is_file() or not capture_ppm.is_file():
            raise RuntimeError("offscreen host sink did not publish its first-frame PPM")
        if baseline_ppm.read_bytes() != capture_ppm.read_bytes():
            raise RuntimeError("enabling SR_VRAMDUMP changed the presented PPM bytes")
        if f"VRAMDUMP vblank={selected_vblank} PASS" not in captured.stdout + captured.stderr:
            raise RuntimeError(f"requested vblank {selected_vblank} produced no VRAM capture")

        # PASS must mean a presented frame: with no presenter (no --gui) nothing is
        # presented, so the same selection must capture nothing and never say PASS.
        headless_dir = root / "headless-run"
        (headless_dir / "capture").mkdir(parents=True)
        headless_env = os.environ.copy()
        for name in ("SR_FBDUMP", "SR_FIRST_FRAME_DUMP", "SR_EXIT_AT_VBLANK"):
            headless_env.pop(name, None)
        headless_env.update({
            "SDL_AUDIO_DRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
            "SR_VRAMDUMP": selected_vblanks,
            "SR_VRAMDUMP_DIR": str(headless_dir / "capture"),
        })
        headless = subprocess.run(
            command[:-1], cwd=headless_dir, env=headless_env, capture_output=True,
            text=True, timeout=30, check=False,
        )
        if headless.returncode:
            raise RuntimeError(
                f"display-smoke headless VRAM check exited {headless.returncode}: "
                f"{headless.stdout}{headless.stderr}"
            )
        headless_output = headless.stdout + headless.stderr
        if re.search(r"VRAMDUMP vblank=\d+ PASS", headless_output) or list(
                (headless_dir / "capture").glob("vram_*")):
            raise RuntimeError("a run with no presenter reported a VRAM capture")

        sidecars = sorted((capture_dir / "capture").glob("vram_*.json"))
        if not sidecars:
            raise RuntimeError("no selected vblank produced a VRAM sidecar")
        ppm_header = b"P6\n480 272\n255\n"
        ppm = baseline_ppm.read_bytes()
        if not ppm.startswith(ppm_header) or len(ppm) - len(ppm_header) != 480 * 272 * 3:
            raise RuntimeError("offscreen host-sink output is not the expected 480x272 P6 frame")
        expected_rgb = ppm[len(ppm_header):]
        matching_sidecar = None
        for candidate in sidecars:
            candidate_sidecar = json.loads(candidate.read_text(encoding="utf-8"))
            display = candidate_sidecar.get("display_framebuffer", {})
            if (display.get("addr") != f"0x{FRAMEBUFFER:08x}" or
                    display.get("stride") != STRIDE or display.get("format_code") != PIXEL_FORMAT):
                raise RuntimeError(f"captured display framebuffer metadata mismatch: {display}")
            raw_path = candidate.parent / candidate_sidecar["vram"]["image_file"]
            raw_image = raw_path.read_bytes()
            matches = True
            for y in range(272):
                for x in range(480):
                    raw_offset = (y * STRIDE + x) * 4
                    ppm_offset = (y * 480 + x) * 3
                    if (raw_image[raw_offset:raw_offset + 3] !=
                            expected_rgb[ppm_offset:ppm_offset + 3]):
                        matches = False
                        break
                if not matches:
                    break
            if matches:
                matching_sidecar = candidate
                break
        if matching_sidecar is None:
            first_sidecar = json.loads(sidecars[0].read_text(encoding="utf-8"))
            raw_image = (sidecars[0].parent / first_sidecar["vram"]["image_file"]).read_bytes()
            samples = []
            for sample_y in (0, 128, 263, 264, 265, 271):
                raw_at = (sample_y * STRIDE) * 4
                ppm_at = sample_y * 480 * 3
                samples.append(
                    f"y{sample_y}:vram={raw_image[raw_at:raw_at + 4].hex()}"
                    f"/ppm={expected_rgb[ppm_at:ppm_at + 3].hex()}"
                )
            raise RuntimeError(
                f"none of {len(sidecars)} bounded VRAM captures matched the offscreen presented PPM; "
                + ",".join(samples)
            )

        output_dir = root / "png"
        decoded = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "vram",
             "--from-sidecar", str(matching_sidecar), "--out-dir", str(output_dir)],
            cwd=ROOT, capture_output=True, text=True, timeout=30, check=False,
        )
        if decoded.returncode:
            raise RuntimeError(f"nk_cli vram could not decode the runtime sidecar: {decoded.stderr}")
        png_path = output_dir / "display_framebuffer.png"
        png = png_path.read_bytes()
        if png[:8] != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError("nk_cli vram output is not a PNG")
        offset = 8
        compressed = bytearray()
        png_width = png_height = 0
        while offset < len(png):
            size = struct.unpack_from(">I", png, offset)[0]
            kind = png[offset + 4:offset + 8]
            data = png[offset + 8:offset + 8 + size]
            offset += size + 12
            if kind == b"IHDR":
                png_width, png_height, depth, color, *_ = struct.unpack(">IIBBBBB", data)
                if depth != 8 or color != 6:
                    raise RuntimeError("nk_cli vram did not produce RGBA8 pixels")
            elif kind == b"IDAT":
                compressed.extend(data)
            elif kind == b"IEND":
                break
        raw = zlib.decompress(compressed)
        if (png_width, png_height) != (480, 272) or len(raw) != 272 * (1 + 480 * 4):
            raise RuntimeError("nk_cli vram PNG dimensions or scanline length mismatch")
        for y in range(272):
            row = raw[y * (1 + 480 * 4):(y + 1) * (1 + 480 * 4)]
            if row[0] != 0:
                raise RuntimeError("nk_cli vram emitted an unexpected PNG row filter")
            row_rgba = row[1:]
            row_rgb = expected_rgb[y * 480 * 3:(y + 1) * 480 * 3]
            for x in range(480):
                rgb = row_rgb[x * 3:x * 3 + 3]
                rgba = row_rgba[x * 4:x * 4 + 4]
                ppm_rgba = rgb + bytes((255,))
                if rgba != ppm_rgba:
                    raise RuntimeError(
                        f"decoded display pixel mismatch at ({x},{y}): "
                        f"PNG={rgba.hex()} PPM={ppm_rgba.hex()}"
                    )
    print("DISPLAY_SMOKE_VRAMDUMP status=PASS bounded_sidecars=1..8 "
          "present_bytes=IDENTICAL png_pixels=IDENTICAL")


FLIGHT_KIND_PRESENT_SET_FRAMEBUF = 28
FLIGHT_KIND_PRESENT_FRAME = 29


def _without_host_present_events(bundle: dict) -> dict:
    """Project out successful-present events and renumber the rest as a complete run.

    SetFrameBuf events keep their buffer, format and stride, but their arg3 is the
    VBLANK count at the call. Paced runs service VBLANKs on the host clock, so that
    count differs between two identical runs on a loaded runner; it is zeroed here
    and the guest-determined arguments are still compared.
    """
    projected = json.loads(json.dumps(bundle))
    kept = [
        event for event in projected["events"]
        if not (event["class"] == "present" and event["kind"] == FLIGHT_KIND_PRESENT_FRAME)
    ]
    for event in kept:
        if event["class"] == "present" and event["kind"] == FLIGHT_KIND_PRESENT_SET_FRAMEBUF:
            event["arg3"] = 0
    for index, event in enumerate(kept, start=1):
        event["sequence"] = index
    projected["events"] = kept
    projected["recorder"]["recorded"] = len(kept)
    projected["recorder"]["dropped"] = 0
    if projected["terminal"]["sequence"]:
        projected["terminal"]["sequence"] = len(kept)
    return projected


def _require_vblank_progress(bundle: dict, label: str) -> None:
    """Refuse a run whose SetFrameBuf VBLANK counts never advance.

    The repeatability comparison zeroes this host-paced count, so this keeps a
    stalled VBLANK clock visible: the counts must be non-decreasing and the last
    must exceed the first.
    """
    counts = [event["arg3"] for event in bundle["events"]
              if event["class"] == "present" and event["kind"] == FLIGHT_KIND_PRESENT_SET_FRAMEBUF]
    if not counts:
        raise RuntimeError(f"{label} flight bundle has no SetFrameBuf events")
    if any(later < earlier for earlier, later in zip(counts, counts[1:], strict=False)):
        raise RuntimeError(f"{label} flight bundle: SetFrameBuf VBLANK counts went backwards: {counts}")
    # A single SetFrameBuf cannot show progress; flight_smoke's kind-count check owns that case.
    if len(counts) > 1 and counts[-1] <= counts[0]:
        raise RuntimeError(f"{label} flight bundle: VBLANK count never advanced ({counts[0]} throughout)")


def flight_smoke(build_dir: Path) -> int:
    """Record and compare repeatable GE/present evidence from the public guest."""
    build_dir = build_dir.resolve()
    manifest = _read_manifest(build_dir / "fixture")
    frames = int(manifest["frames"])
    with tempfile.TemporaryDirectory(prefix="display-smoke-flight-") as temp_dir:
        temp = Path(temp_dir)
        first_path = temp / "first.json"
        second_path = temp / "second.json"
        mutated_path = temp / "mutated.json"
        run(build_dir, gui=True, offscreen=True, flight_output=first_path)
        run(build_dir, gui=True, offscreen=True, flight_output=second_path)

        first = json.loads(first_path.read_text(encoding="utf-8"))
        second = json.loads(second_path.read_text(encoding="utf-8"))
        for label, bundle in (("first", first), ("second", second)):
            if bundle["schema_version"] != 5:
                raise RuntimeError(f"{label} flight bundle is not schema v5")
            if bundle["recorder"]["enabled_classes"] != ["ge", "present"]:
                raise RuntimeError(f"{label} flight bundle has unexpected class coverage")
            if bundle["recorder"]["dropped"] != 0:
                raise RuntimeError(f"{label} flight bundle dropped events")
            _require_vblank_progress(bundle, label)

        kind_counts = Counter((event["class"], event["kind"]) for event in first["events"])
        expected = {
            ("ge", 20): 1,   # enqueue
            ("ge", 25): 1,   # bounded PRIM event
            ("ge", 26): 1,   # list completed
            ("ge", 27): 1,   # FINISH command
            ("present", 28): frames,
            ("present", 29): frames // 2,
        }
        if kind_counts != expected:
            raise RuntimeError(f"unexpected GE/present event counts: {dict(kind_counts)}")

        # Successful host presents are recorded when a VBLANK is serviced, so where
        # they fall among the guest's SetFrameBuf calls follows wall-clock pacing and
        # reordered between two identical runs on a loaded runner. They are counted
        # above; the repeatability comparison covers the guest-determined events (the
        # GE stream and every SetFrameBuf) with the present events projected out.
        for source, target in ((first, first_path), (second, second_path)):
            target.write_text(
                json.dumps(_without_host_present_events(source), sort_keys=True),
                encoding="utf-8")
        second = _without_host_present_events(second)

        tool = ROOT / "tools" / "flight_diff.py"
        match = subprocess.run(
            [sys.executable, str(tool), str(first_path), str(second_path)],
            capture_output=True, text=True, check=False,
        )
        if match.returncode != 0 or "MATCH:" not in match.stdout:
            raise RuntimeError(f"identical flight runs did not match: {match.stdout}{match.stderr}")

        draw = next(event for event in second["events"]
                    if event["class"] == "ge" and event["kind"] == 25)
        draw["arg3"] += 1
        mutated_path.write_text(json.dumps(second, sort_keys=True), encoding="utf-8")
        divergence = subprocess.run(
            [sys.executable, str(tool), str(first_path), str(mutated_path)],
            capture_output=True, text=True, check=False,
        )
        if (divergence.returncode != 1 or f"DIVERGENCE: sequence {draw['sequence']}" not in divergence.stdout
                or "class=ge kind=ge-draw (25)" not in divergence.stdout
                or "arg3:" not in divergence.stdout):
            raise RuntimeError(
                f"mutated draw count was not localized: {divergence.stdout}{divergence.stderr}"
            )

    print(
        f"DISPLAY_FLIGHT_SMOKE status=PASS ge=4 set_framebuf={frames} "
        f"present={frames // 2} identical=MATCH mutation=FIRST_GE_DRAW_COUNT"
    )
    return 0


def run_player(build_dir: Path) -> int:
    """Drive PLAY NOW through the native player without human input.

    The player one-shot mode still populates the ordinary demo library, selects
    its launchable display-smoke entry, and calls ``player_app_launch_game``.
    ``SR_BOOT_EVENT_FILE`` is an opt-in child-runtime evidence channel: it lets
    this test assert the real window and first-frame milestones even when the
    platform process backend does not inherit the parent's stderr pipe.
    """
    suffix = ".exe" if os.name == "nt" else ""
    player = ROOT / "build" / f"nakagawa_player{suffix}"
    if not player.is_file():
        raise RuntimeError(f"native player not found: {player}")

    with tempfile.TemporaryDirectory(prefix="display-smoke-player-", dir=build_dir) as temp:
        sandbox = Path(temp)
        boot_log = sandbox / "boot-events.log"
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(sandbox / "localappdata")
        env["SR_BOOT_EVENT_FILE"] = str(boot_log)
        command = [
            str(player),
            "--demo",
            f"--runtime-root={ROOT}",
            "--launch-index=1",
            f"--screenshot={sandbox / 'player.bmp'}",
        ]
        completed = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=120
        )
        combined = completed.stdout + completed.stderr
        if completed.returncode != 0:
            sys.stderr.write(combined)
            raise RuntimeError(f"native player launch driver exited {completed.returncode}")

        if not boot_log.is_file():
            raise RuntimeError("native player launch produced no child boot evidence")
        events = boot_log.read_text(encoding="utf-8", errors="replace")

        required_parent = (
            "[PLAYER] Launch index 1: PLAY NOW available",
            "[PLAYER] Launch argv contains --gui: yes",
        )
        if failure := evidence_failure(combined, required_parent, ()):
            raise RuntimeError(f"player evidence {failure}")

        required_events = (
            "BOOT_EVENT phase=init public_safe=1",
            "BOOT_EVENT phase=image_loaded",
            "BOOT_EVENT phase=runtime_registered",
            "BOOT_EVENT phase=index_prepare_end state=2",
            "BOOT_EVENT phase=window_ready",
            "BOOT_EVENT phase=first_frame",
        )
        if failure := evidence_failure(events, required_events, ()):
            raise RuntimeError(f"child boot evidence {failure}")

        forbidden = (
            "SR_DATAROOT is configured but is not a valid absolute path",
            "index initialization failed; refusing partial index",
            "UNKNOWN NID",
            "NONPLT_MISS",
            "INTERP_REJECT",
        )
        if failure := evidence_failure(combined + events, (), forbidden):
            raise RuntimeError(f"native player launch evidence {failure}")

        write_if_changed(build_dir / "nakagawa_player.stdout.log", completed.stdout.encode("utf-8"))
        write_if_changed(build_dir / "nakagawa_player.stderr.log", completed.stderr.encode("utf-8"))
        write_if_changed(build_dir / "nakagawa_player.boot-events.log", events.encode("utf-8"))

    print("DISPLAY_SMOKE_PLAYER status=PASS play_now=1 argv_gui=1 window_ready=1 first_frame=1")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--out-dir", type=Path, required=True)
    generate_parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--build-dir", type=Path, required=True)
    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--build-dir", type=Path, required=True)
    run_parser.add_argument("--gui", action="store_true")
    run_parser.add_argument(
        "--offscreen", action="store_true",
        help="run --sched --gui with the explicit no-window host presenter",
    )
    vramdump_parser = subparsers.add_parser("vramdump")
    vramdump_parser.add_argument("--build-dir", type=Path, required=True)
    flight_parser = subparsers.add_parser("flight")
    flight_parser.add_argument("--build-dir", type=Path, required=True)
    player_parser = subparsers.add_parser("run-player")
    player_parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "run" and args.offscreen and not args.gui:
        run_parser.error("--offscreen requires --gui")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "generate":
            return generate(args.out_dir, frames=args.frames)
        if args.command == "verify":
            return verify(args.build_dir)
        if args.command == "run":
            return run(args.build_dir, gui=args.gui, offscreen=args.offscreen)
        if args.command == "vramdump":
            return run_vramdump(args.build_dir)
        if args.command == "flight":
            return flight_smoke(args.build_dir)
        if args.command == "run-player":
            return run_player(args.build_dir)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        sys.stderr.write(f"DISPLAY_SMOKE_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
