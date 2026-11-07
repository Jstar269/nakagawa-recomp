# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

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
The flight gate checks every displayed RGB pixel against the stripe pattern
for the first frame accepted by the host presenter.
"""

from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import hashlib
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
import zlib


ROOT = Path(__file__).resolve().parents[2]

BASE = 0x08810000
ENTRY = BASE

# Build artifacts are named after the catalog title id because that is the only
# shape src/core/nk_launch.c probes for. Keep this equal to the Makefile's
# DISPLAY_SMOKE_NAME and to the "id" in assets/titles/display-smoke.json.
ARTIFACT_STEM = "display-smoke-v1"
TITLE_ID = ARTIFACT_STEM
DISPLAY_DISC_ID = "TEST00006"
DISPLAY_TITLE = "Nakagawa Display Smoke Fixture"

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
DISPLAY_WIDTH = 480
DISPLAY_HEIGHT = 272
PIXELS = STRIDE * DISPLAY_HEIGHT
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


def _expected_present_pixels(frame: int) -> bytes:
    """Build expected RGB bytes from one frame of the guest stripe pattern."""
    frame &= 0xFF
    pixels = bytearray(DISPLAY_WIDTH * DISPLAY_HEIGHT * 3)
    cursor = 0
    for y in range(DISPLAY_HEIGHT):
        for x in range(DISPLAY_WIDTH):
            word = expected_colour(((y * STRIDE + x) >> 4) + frame)
            pixels[cursor] = word & 0xFF
            pixels[cursor + 1] = (word >> 8) & 0xFF
            pixels[cursor + 2] = (word >> 16) & 0xFF
            cursor += 3
    return bytes(pixels)


def _require_exact_vram_frame(data: bytes, label: str) -> int:
    """Require a VRAM dump's framebuffer (offset 0, ABGR8888, stride STRIDE) to be exactly one
    guest frame of the stripe pattern, the one its first pixel names; return that frame."""
    frame = data[0]
    row_bytes = STRIDE * 4
    if len(data) < row_bytes * DISPLAY_HEIGHT:
        raise RuntimeError(f"{label} is {len(data)} bytes, smaller than the framebuffer")
    for y in range(DISPLAY_HEIGHT):
        expected = b"".join(
            expected_colour(((y * STRIDE + x) >> 4) + frame).to_bytes(4, "little")
            for x in range(DISPLAY_WIDTH)
        )
        if data[y * row_bytes:y * row_bytes + DISPLAY_WIDTH * 4] != expected:
            raise RuntimeError(f"{label}: framebuffer row {y} is not guest frame {frame}")
    return frame


def _require_exact_present_pixels(path: Path, frames: int, label: str) -> int:
    """Require every captured pixel to match one complete guest frame."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"{label} has no readable first-frame PPM at {path}") from exc

    header = f"P6\n{DISPLAY_WIDTH} {DISPLAY_HEIGHT}\n255\n".encode("ascii")
    if not data.startswith(header):
        raise RuntimeError(f"{label} first-frame PPM has an unexpected header")
    actual = data[len(header):]
    expected_length = DISPLAY_WIDTH * DISPLAY_HEIGHT * 3
    if len(actual) != expected_length:
        raise RuntimeError(
            f"{label} first-frame PPM has {len(actual)} pixel bytes; expected {expected_length}"
        )

    frame = actual[0]
    if frames <= 256 and frame >= frames:
        raise RuntimeError(
            f"{label} first-frame PPM encodes frame {frame}, outside the {frames}-frame guest run"
        )
    expected = _expected_present_pixels(frame)
    for byte_index, (got, want) in enumerate(zip(actual, expected, strict=True)):
        if got != want:
            pixel_index = byte_index // 3
            x = pixel_index % DISPLAY_WIDTH
            y = pixel_index // DISPLAY_WIDTH
            component = "RGB"[byte_index % 3]
            raise RuntimeError(
                f"{label} presented pixel ({x},{y}) {component} mismatch in frame {frame}: "
                f"got=0x{got:02x} expected=0x{want:02x}"
            )
    return frame


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


def _iso_dir_record(identifier: bytes, lba: int, size: int,
                    is_directory: bool) -> bytes:
    """Build one bounded ISO-9660 record for the synthetic launch disc."""
    record_size = 33 + len(identifier)
    if record_size & 1:
        record_size += 1
    record = bytearray(record_size)
    record[0] = record_size
    record[2:10] = struct.pack("<I", lba) + struct.pack(">I", lba)
    record[10:18] = struct.pack("<I", size) + struct.pack(">I", size)
    record[25] = 2 if is_directory else 0
    record[28] = 1
    record[31] = 1
    record[32] = len(identifier)
    record[33 : 33 + len(identifier)] = identifier
    return bytes(record)


def _display_smoke_param_sfo() -> bytes:
    entries = [
        ("DISC_ID", DISPLAY_DISC_ID.encode("ascii") + b"\0"),
        ("DISC_VERSION", b"1.00\0"),
        ("TITLE", DISPLAY_TITLE.encode("ascii") + b"\0"),
    ]
    entries.sort(key=lambda item: item[0])
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()
    for key, value in entries:
        key_offset = len(key_table)
        key_table.extend(key.encode("ascii") + b"\0")
        data_offset = len(data_table)
        data_table.extend(value)
        while len(data_table) % 4:
            data_table.append(0)
        entry_table.extend(
            struct.pack("<HHIII", key_offset, 0x0204,
                        len(value), len(value), data_offset)
        )

    key_table_start = 20 + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4:
        key_table.append(0)
        data_table_start += 1
    header = struct.pack(
        "<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00",
        key_table_start, data_table_start, len(entries)
    )
    return header + bytes(entry_table) + bytes(key_table) + bytes(data_table)


def _write_display_smoke_iso(path: Path) -> None:
    """Write a synthetic PARAM.SFO disc so PLAY NOW resolves TEST00006 by ID."""
    sector_size = 2048
    root_lba, game_lba, sfo_lba = 17, 18, 19
    sfo = _display_smoke_param_sfo()
    image = bytearray(1024 * sector_size)
    pvd_offset = 16 * sector_size
    image[pvd_offset] = 1
    image[pvd_offset + 1 : pvd_offset + 6] = b"CD001"
    image[pvd_offset + 6] = 1
    image[pvd_offset + 40 : pvd_offset + 40 + len(DISPLAY_DISC_ID)] = \
        DISPLAY_DISC_ID.encode("ascii")

    root_entries = (
        _iso_dir_record(b"\x00", root_lba, sector_size, True)
        + _iso_dir_record(b"\x01", root_lba, sector_size, True)
        + _iso_dir_record(b"PSP_GAME", game_lba, sector_size, True)
    )
    game_entries = (
        _iso_dir_record(b"\x00", game_lba, sector_size, True)
        + _iso_dir_record(b"\x01", root_lba, sector_size, True)
        + _iso_dir_record(b"PARAM.SFO;1", sfo_lba, len(sfo), False)
    )
    image[root_lba * sector_size : root_lba * sector_size + len(root_entries)] = \
        root_entries
    image[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = \
        game_entries
    image[sfo_lba * sector_size : sfo_lba * sector_size + len(sfo)] = sfo
    image[pvd_offset + 156 : pvd_offset + 190] = \
        _iso_dir_record(b"\x00", root_lba, sector_size, True)
    path.write_bytes(image)


def _request_player_window_close(process: subprocess.Popen, what: str = "native player") -> None:
    """Close a process's window the way a person does: WM_CLOSE to its first window."""
    if process.poll() is not None:
        return
    if os.name != "nt":
        process.terminate()
        return

    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    windows = []
    callback_type = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM
    )
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD)
    ]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD

    @callback_type
    def collect_process_window(hwnd, _lparam):
        window_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == process.pid:
            # The window a person closes is a visible one: a graphics driver can own a hidden
            # helper window in the same process, which ignores WM_CLOSE.
            if user32.IsWindowVisible(hwnd):
                windows.insert(0, hwnd)
            else:
                windows.append(hwnd)
        return True

    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    if not user32.EnumWindows(collect_process_window, 0):
        raise OSError(ctypes.get_last_error(), "EnumWindows failed")
    if not windows:
        raise RuntimeError(f"{what} window was not found for cleanup")
    user32.PostMessageW.argtypes = [
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
    ]
    user32.PostMessageW.restype = wintypes.BOOL
    if not user32.PostMessageW(windows[0], 0x0010, 0, 0):  # WM_CLOSE
        raise OSError(ctypes.get_last_error(), "PostMessage(WM_CLOSE) failed")


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
    flight_trigger: str | None = None,
    extra_env: dict[str, str | None] | None = None,
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
    run_cwd = ROOT
    if flight_output is not None:
        run_cwd = flight_output.resolve().parent
        run_cwd.mkdir(parents=True, exist_ok=True)
        # Keep the 59.94 Hz guest VBLANK clock paced even if the caller inherited turbo mode.
        env.update({
            "SR_FLIGHT": "ge,present;4096",
            "SR_NOVBPACE": "0",
            "SR_FLIGHT_OUTPUT": str(flight_output.resolve()),
            "SR_FIRST_FRAME_DUMP": "1",
            # The flight smoke uses a 30 Hz host output cap. Its accepted count is paced by
            # the host clock, so the gate bounds that count while keeping guest evidence exact.
            "SR_FPS_CAP": "30",
        })
        # An inherited trigger or watch range would change what the bundle retains.
        for key in ("SR_FLIGHT_TRIGGER", "SR_WATCH"):
            env.pop(key, None)
        if flight_trigger is not None:
            env["SR_FLIGHT_TRIGGER"] = flight_trigger
    # A recording is opted into per route: an inherited one would change what this run writes.
    for key in ("SR_VIDEOREC_DIR", "SR_VIDEOREC_CODEC", "SR_VIDEOREC_SIZE", "SR_VIDEOREC_START",
                "SR_VIDEOREC_FFMPEG", "NK_VIDEOREC_FAKE_FFMPEG"):
        env.pop(key, None)
    # extra_env names a value to set, or None to remove the variable for this route.
    for key, value in (extra_env or {}).items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    completed = subprocess.run(
        command, cwd=run_cwd, env=env, capture_output=True, text=True
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


def run_quit(build_dir: Path) -> int:
    """Close the runtime's window while the fixture guest is still running (#1377).

    A quit stops the scheduler at its next boundary and the run ends through the driver's
    teardown: the close is reported (``phase=quit_requested``), the driver's code after the
    scheduler runs (its ``done (hit_hle=`` line), and the process exits 0. Before #1377 the
    close called exit() from inside the presenter, so the driver never got there. The fixture
    registers no exit callback, so the stop is due at once (hle-thread-selftest covers the
    callback window). Windows only: the close is a WM_CLOSE to the window, which the default
    SDL3/Vulkan presenter and the GDI fallback both receive; ``SR_VIDEO`` picks between them.
    """
    if os.name != "nt":
        print("DISPLAY_SMOKE_QUIT status=SKIP reason=no_win32_window")
        return 0
    build_dir = build_dir.resolve()
    frames = int(_read_manifest(build_dir / "fixture")["frames"])
    if frames < 600:
        raise RuntimeError(
            f"the quit route needs a guest that outlives the close: build it with "
            f"DISPLAY_SMOKE_BUILD_FRAMES of 600 or more (this one has {frames})"
        )
    executable = (build_dir / f"{ARTIFACT_STEM}.exe").resolve()
    image_path = (build_dir / f"{ARTIFACT_STEM}_image.bin").resolve()
    command = [
        str(executable), "--image", str(image_path), f"0x{BASE:08x}", f"0x{ENTRY:08x}",
        "none", "none", "--gui",
    ]
    with tempfile.TemporaryDirectory(prefix="display-smoke-quit-", dir=build_dir) as temp:
        sandbox = Path(temp)
        events_path = sandbox / "boot.events"
        env = os.environ.copy()
        env["SR_BOOT_EVENT_FILE"] = str(events_path)
        # Scripted routes end or pause the run themselves; none may decide this one.
        for key in ("SR_HEADLESS_ACCEPT_PRESENTED_FRAMES", "SR_EXIT_AT_VBLANK", "SR_PADSCRIPT",
                    "SR_PAUSE_SCRIPT", "SR_GE_CAPTURE_FRAME"):
            env.pop(key, None)
        stdout_path = sandbox / "runtime.stdout.capture"
        stderr_path = sandbox / "runtime.stderr.capture"
        events = ""
        with stdout_path.open("w", encoding="utf-8") as stdout_file, \
                stderr_path.open("w", encoding="utf-8") as stderr_file:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stdout_file,
                                       stderr=stderr_file, text=True)
            try:
                deadline = time.monotonic() + 60
                while process.poll() is None and time.monotonic() < deadline:
                    if events_path.is_file():
                        events = events_path.read_text(encoding="utf-8", errors="replace")
                        if "BOOT_EVENT phase=first_frame" in events:
                            break
                    time.sleep(0.02)
                if process.poll() is not None:
                    raise RuntimeError(f"the runtime exited {process.returncode} before the close")
                if "BOOT_EVENT phase=first_frame" not in events:
                    raise RuntimeError("the runtime presented no first frame within 60 s")
                closed_at = time.monotonic()
                _request_player_window_close(process, "runtime")
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError("the runtime did not stop within 30 s of the close") from error
                stop_seconds = time.monotonic() - closed_at
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
        if events_path.is_file():
            events = events_path.read_text(encoding="utf-8", errors="replace")
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
    write_if_changed(build_dir / f"{ARTIFACT_STEM}.quit.stdout.log", stdout.encode("utf-8"))
    write_if_changed(build_dir / f"{ARTIFACT_STEM}.quit.stderr.log", stderr.encode("utf-8"))
    combined = stdout + stderr
    if process.returncode != 0:
        sys.stderr.write(combined)
        raise RuntimeError(f"the runtime exited {process.returncode} after the close")
    markers = (
        "BOOT_EVENT phase=window_ready",
        "BOOT_EVENT phase=quit_requested",
        "SR_QUIT: the run stops at the scheduler's next boundary",
        "done (hit_hle=",
    )
    forbidden = ("Guest requested exit", "UNKNOWN NID", "NONPLT_MISS", "INTERP_REJECT")
    if failure := evidence_failure(combined + events, markers, forbidden):
        sys.stderr.write(combined)
        raise RuntimeError(f"quit evidence {failure}")
    print(f"DISPLAY_SMOKE_QUIT status=PASS exit=0 stop_after_close_s={stop_seconds:.2f}")
    return 0


#: How many consecutive vblanks run_vramdump asks the capture run to dump (SR_VRAMDUMP_MAX_VBLANKS),
#: from the first vblank the runtime counts. The first presented frame is among them whatever the
#: load: it has been seen at vblanks 1 to 3.
VRAMDUMP_WINDOW = 8
VRAMDUMP_FIRST_VBLANK = 1
#: The latest vblank the capture run may present its first frame at. Measured on this fixture:
#: vblank 1 on an idle machine, 2 or 3 with 16 busy processes competing (80 runs) and in both
#: hosted failures of #2200. 4 leaves a margin; a presenter that slips to 7 is a regression to
#: report, not a race to absorb, even though the 1..8 window would still hold it.
VRAMDUMP_FIRST_PRESENT_MAX = 4


def first_presented_vblank(output: str) -> int | None:
    """The vblank of the first ``HOST_PRESENT_SUBMITTED`` line of a ``SR_PRESENT_TRACE`` run."""
    match = re.search(r"HOST_PRESENT_SUBMITTED f=(\d+)", output)
    return int(match.group(1)) if match else None


def vramdump_capture_failure(output: str, window: range,
                             latest_first_present: int = VRAMDUMP_FIRST_PRESENT_MAX) -> str | None:
    """Why a capture run's output does not prove a present-aligned VRAM capture, or None.

    Which vblank a run presents first is not fixed: it moves with how busy the machine is, so
    the capture run cannot be required to present the vblank the baseline run happened to
    present first (#2200). What stays strict is what the capture proves, judged on the capture
    run's own trace (``SR_PRESENT_TRACE``): the window holds the vblank it presented first (the
    frame ``frame_first.ppm`` is of), every ``VRAMDUMP vblank=N PASS`` is a vblank it presented
    and one that was requested, that first frame was captured, and none reported FAIL.
    """
    presented = {int(value) for value in re.findall(r"HOST_PRESENT_SUBMITTED f=(\d+)", output)}
    passed = [int(value) for value in re.findall(r"VRAMDUMP vblank=(\d+) PASS", output)]
    failed = re.findall(r"VRAMDUMP vblank=(\d+) FAIL", output)
    first, last = window.start, window[-1]
    if failed:
        return f"the capture reported FAIL at vblank {failed[0]}"
    first_presented = first_presented_vblank(output)
    if first_presented is None or first_presented not in window:
        return (f"the capture run's first presented vblank ({first_presented}) is not in the "
                f"requested window {first}..{last}, so its first-frame PPM has no VRAM capture")
    if first_presented > latest_first_present:
        return (f"the capture run presented its first frame at vblank {first_presented}, later than "
                f"the {latest_first_present} this fixture has been seen to need under load")
    if not passed:
        return (f"the requested window {first}..{last} presented vblanks "
                f"{sorted(presented & set(window))} but produced no VRAM capture")
    for vblank in passed:
        if vblank not in window:
            return f"VRAM capture reported PASS for vblank {vblank}, which was not requested"
        if vblank not in presented:
            return f"VRAM capture reported PASS for vblank {vblank}, which the run did not present"
    if len(set(passed)) != len(passed):
        return f"VRAM capture reported PASS twice for one vblank: {passed}"
    if first_presented not in passed:
        return (f"the first presented vblank {first_presented} produced no VRAM capture "
                f"(captured {sorted(passed)})")
    return None


def first_frame_vram_sidecar(capture_dir: Path, first_presented: int, passes: int,
                             expected_rgb: bytes) -> Path:
    """The sidecar of the capture run's first presented vblank, once it proves that frame.

    ``expected_rgb`` is the 480x272 RGB the host sink wrote for that same first present. The
    sidecar of exactly that vblank must carry the display framebuffer's address, stride and format
    and hold those pixels, and the run must have written one sidecar per reported PASS. Another
    vblank's sidecar that happens to match does not stand in for it (#2200).
    """
    sidecars = sorted(capture_dir.glob("vram_*.json"))
    if not sidecars:
        raise RuntimeError("no selected vblank produced a VRAM sidecar")
    if len(sidecars) != passes:
        raise RuntimeError(f"{passes} vblanks reported PASS but {len(sidecars)} VRAM sidecars exist")
    candidate = capture_dir / f"vram_{first_presented}.json"
    if not candidate.is_file():
        raise RuntimeError(f"the first presented vblank {first_presented} has no VRAM sidecar")
    sidecar = json.loads(candidate.read_text(encoding="utf-8"))
    display = sidecar.get("display_framebuffer", {})
    if (display.get("addr") != f"0x{FRAMEBUFFER:08x}" or
            display.get("stride") != STRIDE or display.get("format_code") != PIXEL_FORMAT):
        raise RuntimeError(f"captured display framebuffer metadata mismatch: {display}")
    raw_image = (candidate.parent / sidecar["vram"]["image_file"]).read_bytes()
    row_bytes = STRIDE * 4
    for y in range(DISPLAY_HEIGHT):
        row = raw_image[y * row_bytes:y * row_bytes + DISPLAY_WIDTH * 4]
        rgb = bytearray(DISPLAY_WIDTH * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[0::4], row[1::4], row[2::4]
        if bytes(rgb) != expected_rgb[y * DISPLAY_WIDTH * 3:(y + 1) * DISPLAY_WIDTH * 3]:
            samples = []
            for sample_y in (0, 128, 263, 264, 265, 271):
                raw_at = sample_y * row_bytes
                ppm_at = sample_y * DISPLAY_WIDTH * 3
                samples.append(f"y{sample_y}:vram={raw_image[raw_at:raw_at + 4].hex()}"
                               f"/ppm={expected_rgb[ppm_at:ppm_at + 3].hex()}")
            raise RuntimeError(
                f"the VRAM capture of the first presented vblank {first_presented} "
                f"({len(sidecars)} captured) does not match the offscreen presented PPM at row {y}; "
                + ",".join(samples))
    return candidate


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
        # The baseline run proves a frame is presented and gives the reference PPM. The capture
        # run is another process and presents its first frame at whatever vblank the machine's
        # load lets it, so the window starts at the first vblank and the run is judged on its
        # own trace (#2200).
        window = range(VRAMDUMP_FIRST_VBLANK, VRAMDUMP_FIRST_VBLANK + VRAMDUMP_WINDOW)
        selection = list(window)
        selected_vblanks = ",".join(str(vblank) for vblank in selection)
        captured = launch(capture_dir, capture_vblank=selected_vblanks)
        baseline_ppm = baseline_dir / "frame_first.ppm"
        capture_ppm = capture_dir / "frame_first.ppm"
        if not baseline_ppm.is_file() or not capture_ppm.is_file():
            raise RuntimeError("offscreen host sink did not publish its first-frame PPM")
        # Each run's first frame must be exactly one guest frame of the stripe pattern (the one
        # its first pixel names). They are the same frame in every run measured, 0, whatever
        # vblank was presented first; checking each against its own frame keeps that a fact the
        # check verifies, not one it assumes (#2200).
        frames = int(_read_manifest(build_dir / "fixture")["frames"])
        baseline_frame = _require_exact_present_pixels(baseline_ppm, frames, "baseline")
        capture_frame = _require_exact_present_pixels(capture_ppm, frames, "capture")
        if baseline_frame == capture_frame and baseline_ppm.read_bytes() != capture_ppm.read_bytes():
            raise RuntimeError("enabling SR_VRAMDUMP changed the presented PPM bytes")
        captured_output = captured.stdout + captured.stderr
        if failure := vramdump_capture_failure(captured_output, window):
            raise RuntimeError(failure)
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

        passes = len(re.findall(r"VRAMDUMP vblank=\d+ PASS", captured_output))
        ppm_header = b"P6\n480 272\n255\n"
        ppm = capture_ppm.read_bytes()
        if not ppm.startswith(ppm_header) or len(ppm) - len(ppm_header) != 480 * 272 * 3:
            raise RuntimeError("offscreen host-sink output is not the expected 480x272 P6 frame")
        expected_rgb = ppm[len(ppm_header):]
        first_presented = first_presented_vblank(captured_output)
        assert first_presented is not None  # vramdump_capture_failure refused a run without one
        matching_sidecar = first_frame_vram_sidecar(
            capture_dir / "capture", first_presented, passes, expected_rgb)

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


def _without_host_present_events(bundle: dict, keep_vblank_count: bool = False) -> dict:
    """Project out successful-present events and renumber the rest as a complete run.

    SetFrameBuf events keep their buffer, format and stride, but their arg3 is the
    VBLANK count at the call. Paced runs service VBLANKs on the host clock, so that
    count differs between two identical runs on a loaded runner; it is zeroed here
    and the guest-determined arguments are still compared. On the deterministic clock
    the count is guest time, so ``keep_vblank_count`` keeps it in the comparison.
    """
    projected = json.loads(json.dumps(bundle))
    kept = [
        event for event in projected["events"]
        if not (event["class"] == "present" and event["kind"] == FLIGHT_KIND_PRESENT_FRAME)
    ]
    for event in kept:
        if (not keep_vblank_count and event["class"] == "present"
                and event["kind"] == FLIGHT_KIND_PRESENT_SET_FRAMEBUF):
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


def _require_flight_event_counts(bundle: dict, frames: int, label: str) -> int:
    """Enforce exact guest counts and bound wall-clock host output."""
    counts = Counter((event["class"], event["kind"]) for event in bundle["events"])
    expected_guest = {
        ("ge", 20): 1,   # enqueue
        ("ge", 25): 1,   # bounded PRIM event
        ("ge", 26): 1,   # list completed
        ("ge", 27): 1,   # FINISH command
        ("present", FLIGHT_KIND_PRESENT_SET_FRAMEBUF): frames,
    }
    actual_guest = {
        key: count for key, count in counts.items()
        if key != ("present", FLIGHT_KIND_PRESENT_FRAME)
    }
    if actual_guest != expected_guest:
        raise RuntimeError(
            f"{label} has unexpected deterministic GE/SetFrameBuf counts: {actual_guest}"
        )

    for event in bundle["events"]:
        if event["class"] != "present" or event["kind"] != FLIGHT_KIND_PRESENT_SET_FRAMEBUF:
            continue
        actual = (event["arg0"], event["arg1"], event["arg2"])
        expected = (FRAMEBUFFER, PIXEL_FORMAT, STRIDE)
        if actual != expected:
            raise RuntimeError(
                f"{label} SetFrameBuf arguments {actual} differ from {expected}"
            )

    # The 30 Hz host output cap does not wait for a missed slot. Hosted Windows CI
    # observed 31 presents on #1058 and 60 then 31 across #1076's two launches;
    # local runs observed 30. Keep the guest event stream and pixels exact while
    # accepting the measured wall-clock range of 30 through one per guest frame.
    host_presents = counts.get(("present", FLIGHT_KIND_PRESENT_FRAME), 0)
    minimum = frames // 2
    if not minimum <= host_presents <= frames:
        raise RuntimeError(
            f"{label} host present count {host_presents} outside measured range {minimum}..{frames}"
        )
    return host_presents


def flight_smoke(build_dir: Path) -> int:
    """Record exact guest/pixel evidence and bounded host-paced presents."""
    build_dir = build_dir.resolve()
    manifest = _read_manifest(build_dir / "fixture")
    frames = int(manifest["frames"])
    with tempfile.TemporaryDirectory(prefix="display-smoke-flight-") as temp_dir:
        temp = Path(temp_dir)
        first_dir = temp / "first"
        second_dir = temp / "second"
        first_dir.mkdir()
        second_dir.mkdir()
        first_path = first_dir / "flight.json"
        second_path = second_dir / "flight.json"
        mutated_path = temp / "mutated.json"
        run(build_dir, gui=True, offscreen=True, flight_output=first_path)
        run(build_dir, gui=True, offscreen=True, flight_output=second_path)

        first = json.loads(first_path.read_text(encoding="utf-8"))
        second = json.loads(second_path.read_text(encoding="utf-8"))
        host_present_counts = []
        pixel_frames = []
        for label, bundle, run_dir in (
            ("first", first, first_dir), ("second", second, second_dir)
        ):
            if bundle["schema_version"] != 7:
                raise RuntimeError(f"{label} flight bundle is not schema v7")
            if bundle["recorder"]["enabled_classes"] != ["ge", "present"]:
                raise RuntimeError(f"{label} flight bundle has unexpected class coverage")
            if bundle["recorder"]["dropped"] != 0:
                raise RuntimeError(f"{label} flight bundle dropped events")
            _require_vblank_progress(bundle, label)
            host_present_counts.append(_require_flight_event_counts(bundle, frames, label))
            pixel_frames.append(
                _require_exact_present_pixels(run_dir / "frame_first.ppm", frames, label)
            )

        # Successful host presents follow wall-clock pacing, so their positions vary
        # between runs. Compare every deterministic GE event and SetFrameBuf argument
        # after projecting out host presents and the host-paced VBLANK field.
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

        # The ge-draw trigger mode (#320) on the production GE path: the record freezes at
        # the first PRIM, while the run itself still finishes with its exact pixel.
        trigger_dir = temp / "trigger"
        trigger_dir.mkdir()
        trigger_path = trigger_dir / "flight.json"
        run(build_dir, gui=True, offscreen=True, flight_output=trigger_path,
            flight_trigger="ge-draw:1")
        triggered = json.loads(trigger_path.read_text(encoding="utf-8"))
        validated = subprocess.run(
            [sys.executable, "-c",
             "import json, sys; sys.path.insert(0, sys.argv[1]); import flight_diff; "
             "flight_diff.validate_bundle(json.load(open(sys.argv[2], encoding='utf-8')))",
             str(ROOT / "tools"), str(trigger_path)],
            capture_output=True, text=True, check=False,
        )
        if validated.returncode != 0:
            raise RuntimeError(f"ge-draw trigger bundle is malformed: {validated.stderr}")
        terminal = triggered["terminal"]
        last = triggered["events"][-1] if triggered["events"] else {}
        draws = [event for event in triggered["events"]
                 if event["class"] == "ge" and event["kind"] == 25]
        if (terminal["reason"] != "trigger" or terminal["kind"] != 25 or terminal["arg0"] != 1
                or triggered["recorder"]["triggers"]["mode"] != "ge-draw"
                or triggered["recorder"]["triggers"]["ge_draw"] != 1
                or len(draws) != 1 or last is not draws[-1]
                or terminal["sequence"] != last.get("sequence")
                or triggered["recorder"]["recorded"] != terminal["sequence"]):
            raise RuntimeError(f"ge-draw trigger did not freeze at the first draw: {terminal}")

    print(
        f"DISPLAY_FLIGHT_SMOKE status=PASS ge=4 set_framebuf={frames} "
        f"host_present={host_present_counts[0]},{host_present_counts[1]} "
        f"bounds={frames // 2}..{frames} pixel_frames={pixel_frames[0]},{pixel_frames[1]} "
        "pixels=EXACT identical=MATCH mutation=FIRST_GE_DRAW_COUNT "
        f"trigger=GE_DRAW_1@{terminal['sequence']}"
    )
    return 0


def _videorec_frames(video: Path, frames: int) -> list[int]:
    """Decode raw BGRX frames into the guest frame index each picture shows.

    The fake encoder stores what the recorder piped to it unchanged, so its video.mkv
    holds the same raw frames a raw recording's video.bgr0 does.

    Every recorded picture must be one complete guest frame of the stripe pattern, exactly,
    and the guest frame index may only stay or grow from one recorded frame to the next.
    """
    size = DISPLAY_WIDTH * DISPLAY_HEIGHT * 4
    data = video.read_bytes()
    if len(data) % size:
        raise RuntimeError(f"{video.name} is {len(data)} bytes, not a whole number of frames")
    expected_cache: dict[int, bytes] = {}
    shown: list[int] = []
    for index in range(len(data) // size):
        bgrx = data[index * size:(index + 1) * size]
        rgb = bytearray(DISPLAY_WIDTH * DISPLAY_HEIGHT * 3)
        rgb[0::3] = bgrx[2::4]
        rgb[1::3] = bgrx[1::4]
        rgb[2::3] = bgrx[0::4]
        frame = rgb[0]
        if frames <= 256 and frame >= frames:
            raise RuntimeError(f"recorded frame {index} shows guest frame {frame}, outside the run")
        if frame not in expected_cache:
            expected_cache[frame] = _expected_present_pixels(frame)
        if bytes(rgb) != expected_cache[frame]:
            raise RuntimeError(f"recorded frame {index} is not exactly guest frame {frame}")
        if shown and frame < shown[-1]:
            raise RuntimeError(f"recorded frame {index} goes back to guest frame {frame}")
        shown.append(frame)
    return shown


# The fake encoder sleeps this long after each frame it reads, so the recorder's pipe writes
# block: a stall at least as long as a 4K-frame FFV1 encode on a slow host.
VIDEOREC_STALL_MS = 40
# Deterministic clock, not paced: recording refuses to start on the paced clock (#2080 review).
_DETERMINISTIC = {"SR_DETERMINISTIC_CLOCK": "1", "SR_NOVBPACE": None}


def videorec_smoke(build_dir: Path, fake_ffmpeg: Path) -> int:
    """Record the fixture's run through a stalled encoder and prove the guest is unchanged (#1273).

    Two offscreen runs on the deterministic clock, one recording through ``fake_ffmpeg`` (the
    videorec selftest acting as FFmpeg) that stalls every frame write. The guest's GE and
    SetFrameBuf flight events, including the VBLANK count at each SetFrameBuf, match after host
    presents are projected out, so the stall moved no guest event in guest time. Both runs finish
    with the exact framebuffer word; record.json is PASS and muxed, shows the stall in
    ``encoder_write_ms``, and the recording holds one exact guest picture per VBLANK with a
    silent WAV exactly as long as the video.
    """
    build_dir = build_dir.resolve()
    fake_ffmpeg = fake_ffmpeg.resolve()
    if not fake_ffmpeg.is_file():
        raise RuntimeError(f"the fake encoder {fake_ffmpeg.name} was not built")
    manifest = _read_manifest(build_dir / "fixture")
    frames = int(manifest["frames"])
    with tempfile.TemporaryDirectory(prefix="display-smoke-videorec-") as temp_dir:
        temp = Path(temp_dir)
        plain_dir = temp / "plain"
        rec_dir = temp / "recorded"
        out_dir = temp / "video"
        plain_dir.mkdir()
        rec_dir.mkdir()
        plain_path = plain_dir / "flight.json"
        rec_path = rec_dir / "flight.json"
        run(build_dir, gui=True, offscreen=True, flight_output=plain_path, extra_env=_DETERMINISTIC)
        run(build_dir, gui=True, offscreen=True, flight_output=rec_path,
            extra_env={**_DETERMINISTIC, "SR_VIDEOREC_DIR": str(out_dir),
                       "SR_VIDEOREC_CODEC": "ffv1", "SR_VIDEOREC_FFMPEG": str(fake_ffmpeg),
                       "NK_VIDEOREC_FAKE_FFMPEG": f"enc=slow:{VIDEOREC_STALL_MS};mux=ok"})
        log = (build_dir / f"{ARTIFACT_STEM}.gui-offscreen.stderr.log").read_text(
            encoding="utf-8", errors="replace")
        if "VIDEOREC: PASS" not in log:
            raise RuntimeError("the recording run did not report VIDEOREC: PASS")

        plain = json.loads(plain_path.read_text(encoding="utf-8"))
        recorded = json.loads(rec_path.read_text(encoding="utf-8"))
        for label, bundle, run_dir in (
            ("plain", plain, plain_dir), ("recorded", recorded, rec_dir)
        ):
            if bundle["recorder"]["dropped"] != 0:
                raise RuntimeError(f"{label} flight bundle dropped events")
            _require_vblank_progress(bundle, label)
            _require_exact_present_pixels(run_dir / "frame_first.ppm", frames, label)
        plain_path.write_text(
            json.dumps(_without_host_present_events(plain, keep_vblank_count=True), sort_keys=True),
            encoding="utf-8")
        rec_path.write_text(
            json.dumps(_without_host_present_events(recorded, keep_vblank_count=True),
                       sort_keys=True),
            encoding="utf-8")
        match = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "flight_diff.py"), str(plain_path),
             str(rec_path)],
            capture_output=True, text=True, check=False,
        )
        if match.returncode != 0 or "MATCH:" not in match.stdout:
            raise RuntimeError(
                "the stalled recording changed the guest's flight events or their VBLANK "
                f"counts: {match.stdout}{match.stderr}")

        record = json.loads((out_dir / "record.json").read_text(encoding="utf-8"))
        if record["status"] != "PASS":
            raise RuntimeError(f"record.json reports {record['status']}: {record['reason']}")
        if (record["encoder"], record["muxed"], record["clock"]) != ("ffv1", True, "deterministic"):
            raise RuntimeError("record.json is not an encoded, muxed, deterministic-clock recording")
        # The fixture's guest returns to the driver, which names that end and its exit status.
        if (record["run_end"], record["exit_status"]) != ("the run returned to the driver", 0):
            raise RuntimeError(
                f"the recording ended at {record['run_end']!r} with exit status "
                f"{record['exit_status']!r}, not the driver's clean end")
        if (record["width"], record["height"]) != (DISPLAY_WIDTH, DISPLAY_HEIGHT):
            raise RuntimeError("the offscreen recording is not at the presenter's 480x272")
        count = int(record["frames"])
        stall_ms = int(record["encoder_write_ms"])
        # The encoder pipe holds about two frames on Windows (videorec.c sizes it) and the
        # 64 KiB default on POSIX, so once it fills each write waits out most of a stall.
        if stall_ms < count * VIDEOREC_STALL_MS // 2:
            raise RuntimeError(
                f"encoder writes took {stall_ms} ms for {count} frames: the stall did not happen")
        rows = (out_dir / "frames.csv").read_text(encoding="utf-8").splitlines()[1:]
        if count < 2 or len(rows) != count:
            raise RuntimeError(f"record.json counts {count} frames, frames.csv {len(rows)}")
        vcounts = [int(row.split(",")[1]) for row in rows]
        if vcounts != list(range(vcounts[0], vcounts[0] + count)):
            raise RuntimeError("the recording does not hold one frame per VBLANK")
        video = out_dir / "video.mkv"
        shown = _videorec_frames(video, frames)
        if len(shown) != count:
            raise RuntimeError(f"{video.name} holds {len(shown)} frames, record.json {count}")
        if (out_dir / "recording.mkv").read_bytes() != video.read_bytes():
            raise RuntimeError("the fake mux did not copy the encoded video into recording.mkv")
        cells = [row.split(",") for row in rows]
        new = sum(1 for cell in cells if cell[3] == "new")
        if new != int(record["new_frames"]) or len(set(shown)) > new:
            raise RuntimeError("new frames in frames.csv and the pictures disagree")
        flips = sum(int(cell[4]) for cell in cells)
        if flips < 1 or flips != int(record["guest_flips"]):
            raise RuntimeError(f"frames.csv counts {flips} guest flips, record.json {record['guest_flips']}")
        wav = (out_dir / "audio.wav").read_bytes()
        samples = (len(wav) - 44) // 4
        if samples != count * 44144100 // 60000 or any(wav[44:]):
            raise RuntimeError(f"audio.wav holds {samples} samples; the fixture is silent")
    print(
        f"DISPLAY_SMOKE_VIDEOREC status=PASS frames={count} new={new} guest_flips={flips} "
        f"guest_frames={shown[0]}..{shown[-1]} audio_samples={samples} "
        f"encoder_write_ms={stall_ms} stall_ms_per_frame={VIDEOREC_STALL_MS} "
        "clock=deterministic guest=MATCH vblank_counts=MATCH pixels=EXACT"
    )
    return 0


def fsr1_smoke(build_dir: Path) -> int:
    """SR_PRESENT_UPSCALE=fsr1 is presentation only: the guest's frame, guest VRAM and every guest
    flight event are byte-identical with it on and off.

    Two runs on the Vulkan presenter, in a hidden window larger than the guest frame so the upscaler
    runs on every present: off, then fsr1. Each records the flight bundle, the first presented guest
    frame and VRAM at a set of VBLANKs. The fsr1 run must say it upscaled presents; the off run must
    not. The flight bundles must match after the host-paced present events are projected out, the
    first frames must be the exact stripe pixels and equal, every VRAM dump's framebuffer must be
    exactly the guest frame it names, and the dumps of a guest frame both runs captured must hold
    the same VRAM bytes.
    """
    build_dir = build_dir.resolve()
    manifest = _read_manifest(build_dir / "fixture")
    frames = int(manifest["frames"])
    expected = final_frame_first_pixel(frames)
    executable = (build_dir / f"{ARTIFACT_STEM}.exe").resolve()
    if not executable.exists():
        executable = (build_dir / ARTIFACT_STEM).resolve()
    image_path = (build_dir / f"{ARTIFACT_STEM}_image.bin").resolve()
    if not executable.is_file() or not image_path.is_file():
        raise RuntimeError("display-smoke runtime and generated image must be built first")
    command = [
        str(executable), "--image", str(image_path), f"0x{BASE:08x}", f"0x{ENTRY:08x}",
        "none", "none", "--gui", f"--expect-u32=0x{FRAMEBUFFER:08x}:0x{expected:08x}",
    ]
    # SR_VRAMDUMP takes at most 8 VBLANKs (SR_VRAMDUMP_MAX_VBLANKS): eight in a row early in the run,
    # where two runs' host pacing has drifted least, so they share guest frames.
    vblanks = ",".join(str(v) for v in range(6, 14))

    def launch(work: Path, upscale: str | None) -> tuple[dict, str]:
        (work / "capture").mkdir(parents=True)
        env = os.environ.copy()
        for name in ("SR_VIDEO", "SR_FBDUMP", "SR_EXIT_AT_VBLANK", "SR_FLIGHT_TRIGGER", "SR_WATCH",
                     "SR_PRESENT_UPSCALE", "SR_PRESENT_SCALING"):
            env.pop(name, None)
        env.update({
            "SR_WINDOW_HIDDEN": "1",
            "SR_FLIGHT": "ge,present;4096",
            "SR_NOVBPACE": "0",
            "SR_FLIGHT_OUTPUT": str(work / "flight.json"),
            "SR_FIRST_FRAME_DUMP": "1",
            "SR_FPS_CAP": "30",
            "SR_VRAMDUMP": vblanks,
            "SR_VRAMDUMP_DIR": str(work / "capture"),
        })
        if upscale:
            env["SR_PRESENT_UPSCALE"] = upscale
        done = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True,
                              timeout=120, check=False)
        combined = done.stdout + done.stderr
        (work / "run.log").write_text(combined, encoding="utf-8")
        if done.returncode:
            raise RuntimeError(f"fsr1 smoke ({upscale or 'off'}) exited {done.returncode}: {combined[-2000:]}")
        if "status=PASS" not in combined or "status=FAIL" in combined:
            raise RuntimeError(f"fsr1 smoke ({upscale or 'off'}) missed its guest check: {combined[-2000:]}")
        if "VRAMDUMP enabled selections=8" not in combined:
            raise RuntimeError(f"the {upscale or 'off'} run refused its VRAM dump selection: {combined[-2000:]}")
        bundle = json.loads((work / "flight.json").read_text(encoding="utf-8"))
        _require_vblank_progress(bundle, upscale or "off")
        return bundle, combined

    with tempfile.TemporaryDirectory(prefix="display-smoke-fsr1-") as temporary:
        root = Path(temporary)
        runs = {}
        for label in ("off", "fsr1"):
            work = root / label
            bundle, combined = launch(work, None if label == "off" else "fsr1")
            runs[label] = (work, bundle, combined)
        off_log, fsr_log = runs["off"][2], runs["fsr1"][2]
        upscaled = re.search(r"sdl3vk: FSR 1 upscaled (\d+) presents", fsr_log)
        if "FSR 1 upscaling on" not in fsr_log or not upscaled or int(upscaled.group(1)) < 1:
            raise RuntimeError("the fsr1 run did not upscale any present: " + fsr_log[-2000:])
        if "FSR 1 upscal" in off_log:
            raise RuntimeError("the off run reported FSR 1 upscaling")

        first = [runs[label][0] / "frame_first.ppm" for label in ("off", "fsr1")]
        for path, label in zip(first, ("off", "fsr1"), strict=True):
            _require_exact_present_pixels(path, frames, label)
        if first[0].read_bytes() != first[1].read_bytes():
            raise RuntimeError("FSR 1 changed the first presented guest frame")

        # The upscaled run is stamped as enhanced, in its log and its bundle; the off run is not.
        if "ACE_MODE: present upscale fsr1" not in fsr_log or "ACE_MODE:" in off_log:
            raise RuntimeError("the fsr1 run's ACE_MODE stamp is missing, or the off run has one")
        # tools/boot_gate.py also reads the boot event, for a run whose stderr lines were silenced.
        if "BOOT_EVENT phase=present_upscale mode=fsr1" not in fsr_log or "phase=present_upscale" in off_log:
            raise RuntimeError("the fsr1 run's present_upscale boot event is missing, or the off run has one")
        stamps = [runs[label][1]["runtime"].get("enhancements") for label in ("off", "fsr1")]
        if stamps != [None, {"present_upscale": "fsr1"}]:
            raise RuntimeError(f"flight bundle enhancements are {stamps}, not none and fsr1")
        paths = []
        for label in ("off", "fsr1"):
            target = root / f"{label}.projected.json"
            projected = _without_host_present_events(runs[label][1])
            # Compared for the guest's events only: the stamp marks the window, not the guest.
            projected["runtime"].pop("enhancements", None)
            target.write_text(json.dumps(projected, sort_keys=True), encoding="utf-8")
            paths.append(target)
        match = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "flight_diff.py"), str(paths[0]), str(paths[1])],
            capture_output=True, text=True, check=False,
        )
        if match.returncode != 0 or "MATCH:" not in match.stdout:
            raise RuntimeError(f"FSR 1 changed the guest's flight events: {match.stdout}{match.stderr}")

        # Which guest frame a VBLANK shows depends on host pacing, which differs from run to run. So
        # every dump of both runs must hold, in its framebuffer, exactly the guest frame its first
        # pixel names (VRAM offset 0 is the framebuffer), and every dump of a frame both runs
        # captured must match the other run's byte for byte, the whole of VRAM.
        held = []
        for label in ("off", "fsr1"):
            by_frame: dict[bytes, list[tuple[str, bytes]]] = {}
            for dump in sorted((runs[label][0] / "capture").glob("vram_*.bin")):
                data = dump.read_bytes()
                _require_exact_vram_frame(data, f"{label} {dump.name}")
                by_frame.setdefault(data[:4], []).append((dump.name, data))
            held.append(by_frame)
        common = sorted(set(held[0]) & set(held[1]))
        if not common:
            raise RuntimeError("the two runs captured no common guest frame in VRAM")
        changed = [f"{a} (off) vs {b} (on)" for key in common for a, x in held[0][key]
                   for b, y in held[1][key] if x != y]
        if changed:
            raise RuntimeError(f"FSR 1 changed guest VRAM: {changed}")
    print(
        f"DISPLAY_SMOKE_FSR1 status=PASS upscaled_presents={upscaled.group(1)} guest=MATCH "
        f"first_frame=EXACT vram_frames={len(common)} vram=IDENTICAL framebuffers=EXACT"
    )
    return 0


def run_player(build_dir: Path) -> int:
    """Drive PLAY NOW through the native player without human input.

    A small source-owned ISO exposes the display fixture's stable disc ID to
    the same ``--iso --launch-now`` route used by the UI. The player's child
    boot log is read from its sandboxed Local AppData cache while the guest runs.
    """
    suffix = ".exe" if os.name == "nt" else ""
    player_build = ROOT / "build" / f"nakagawa_player{suffix}"
    if not player_build.is_file():
        raise RuntimeError(f"native player not found: {player_build}")

    with tempfile.TemporaryDirectory(prefix="display-smoke-player-", dir=build_dir) as temp:
        sandbox = Path(temp).resolve()
        player_dir = sandbox / "player"
        player_dir.mkdir()
        player = player_dir / player_build.name
        shutil.copy2(player_build, player)
        for runtime_dll in player_build.parent.glob("*.dll"):
            shutil.copy2(runtime_dll, player_dir / runtime_dll.name)

        local_app_data = sandbox / "localappdata"
        iso_path = sandbox / "display-smoke.iso"
        _write_display_smoke_iso(iso_path)
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(local_app_data)
        if os.name == "nt":
            cache_dir = local_app_data / "Nakagawa" / "cache"
        else:
            xdg_cache = sandbox / "xdg-cache"
            env["XDG_CACHE_HOME"] = str(xdg_cache)
            cache_dir = xdg_cache / "nakagawa-recomp"

        command = [
            str(player),
            f"--iso={iso_path}",
            "--launch-now",
            f"--runtime-root={ROOT}",
        ]
        stdout_path = sandbox / "nakagawa_player.stdout.capture"
        stderr_path = sandbox / "nakagawa_player.stderr.capture"
        with stdout_path.open("w", encoding="utf-8") as stdout_file, \
                stderr_path.open("w", encoding="utf-8") as stderr_file:
            process = subprocess.Popen(
                command, cwd=ROOT, env=env, stdout=stdout_file, stderr=stderr_file,
                text=True,
            )
            boot_log = cache_dir / f"player-boot-{process.pid}-1.events"
            deadline = time.monotonic() + 120
            saw_boot_file = False
            events = ""
            try:
                while process.poll() is None and time.monotonic() < deadline:
                    if boot_log.is_file():
                        saw_boot_file = True
                        try:
                            events = boot_log.read_text(
                                encoding="utf-8", errors="replace"
                            )
                        except OSError:
                            pass
                    elif saw_boot_file:
                        break
                    time.sleep(0.02)

                timed_out = process.poll() is None and time.monotonic() >= deadline
                if process.poll() is None:
                    _request_player_window_close(process)
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired as error:
                        raise RuntimeError(
                            "native player did not close after the fixture guest exited"
                        ) from error

                if boot_log.is_file():
                    try:
                        events = boot_log.read_text(
                            encoding="utf-8", errors="replace"
                        )
                    except OSError:
                        pass
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)

        player_stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        player_stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        combined = player_stdout + player_stderr
        if timed_out:
            sys.stderr.write(combined)
            raise RuntimeError("native player launch timed out before the child boot log was removed")
        if process.returncode != 0:
            sys.stderr.write(combined)
            raise RuntimeError(f"native player exited {process.returncode}")
        if not saw_boot_file:
            sys.stderr.write(combined)
            raise RuntimeError("native player launch produced no child boot evidence in its cache")

        required_parent = (
            f"-> Disc ID: {DISPLAY_DISC_ID},",
            f"[PLAYER] Launching {DISPLAY_DISC_ID} now...",
        )
        if failure := evidence_failure(combined, required_parent, ()):
            sys.stderr.write(combined)
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
            sys.stderr.write(combined)
            sys.stderr.write(events)
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

        write_if_changed(build_dir / "nakagawa_player.stdout.log", player_stdout.encode("utf-8"))
        write_if_changed(build_dir / "nakagawa_player.stderr.log", player_stderr.encode("utf-8"))
        write_if_changed(build_dir / "nakagawa_player.boot-events.log", events.encode("utf-8"))

    print(
        f"DISPLAY_SMOKE_RUN-PLAYER status=PASS play_now=1 disc_id={DISPLAY_DISC_ID} "
        "window_ready=1 first_frame=1"
    )
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
    videorec_parser = subparsers.add_parser("videorec")
    videorec_parser.add_argument("--build-dir", type=Path, required=True)
    videorec_parser.add_argument("--fake-ffmpeg", type=Path, required=True,
                                 help="the videorec selftest binary, run as a stalling FFmpeg")
    fsr1_parser = subparsers.add_parser("fsr1")
    fsr1_parser.add_argument("--build-dir", type=Path, required=True)
    player_parser = subparsers.add_parser("run-player")
    player_parser.add_argument("--build-dir", type=Path, required=True)
    quit_parser = subparsers.add_parser("quit")
    quit_parser.add_argument("--build-dir", type=Path, required=True)
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
        if args.command == "videorec":
            return videorec_smoke(args.build_dir, args.fake_ffmpeg)
        if args.command == "fsr1":
            return fsr1_smoke(args.build_dir)
        if args.command == "run-player":
            return run_player(args.build_dir)
        if args.command == "quit":
            return run_quit(args.build_dir)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        sys.stderr.write(f"DISPLAY_SMOKE_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
