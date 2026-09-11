# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate and qualify the source-owned PSP display smoke guest.

The committed fixture is this recipe, not a binary.  It deterministically emits
a small ELF32 PSP PRX/``~PSP`` pair into the ignored build tree.  Unlike
``fixtures/production_smoke``, whose guest proves the AOT/interpreter seam with
a sentinel word, this guest proves the *presentation* path: it fills the PSP
framebuffer with a moving pattern, hands the buffer to ``sceDisplaySetFrameBuf``
and waits for vblank, once per frame, for a fixed number of frames.

That exercises the production chain the compute fixtures never touch --
``sceDisplaySetFrameBuf`` -> display latch -> vblank -> ``gui_present`` -- with
no external toolchain, no retail disc, and no private input.  The guest is
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

    frame = 0
    do {
        for (i = 0; i < 512 * 272; i++)
            vram[i] = colour((i >> 4) + frame);
        sceDisplaySetFrameBuf(0x04000000, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
        sceDisplayWaitVblankStart();
    } while (++frame < FRAMES);

``colour(v)`` packs ``v`` into an ABGR8888 word so consecutive 16-pixel bands
differ in all three channels and the whole pattern scrolls by one band per
frame.
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

BASE = 0x08810000
ENTRY = BASE

# Build artifacts are named after the catalog title id because that is the only
# shape src/core/nk_launch.c probes for. Keep this equal to the Makefile's
# DISPLAY_SMOKE_NAME and to the "id" in assets/titles/display-smoke.json.
ARTIFACT_STEM = "display-smoke-v1"
TITLE_ID = ARTIFACT_STEM

# Guest text layout. The entry body occupies [0, TEXT_SECTION_SIZE); the two
# import stubs form the .sceStub.text section at STUB_OFFSET.
TEXT_SECTION_SIZE = 0xA4
STUB_OFFSET = 0xB0
SETFB_STUB = STUB_OFFSET
VBLANK_STUB = STUB_OFFSET + 8
TEXT_FILE_SIZE = 0xC0

# Guest data layout, mirroring the PSP module-info/import-table shape.
DATA_VADDR = 0x1000
MODULE_INFO_SIZE = 52
LIBRARY_NAME_OFFSET = 0x34
LIBSTUB_OFFSET = 0x50
LIBSTUB_SIZE = 20
NID_TABLE_OFFSET = 0x64
DATA_FILE_SIZE = 0x70
DATA_MEMORY_SIZE = 0x80
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE

TEXT_FILE_OFFSET = 0x100
DATA_FILE_OFFSET = 0x200
RELOCATION_FILE_OFFSET = 0x280

LIBRARY = "sceDisplay"
NID_SET_FRAME_BUF = 0x289D82FE
NID_WAIT_VBLANK_START = 0x984C27E7
NIDS = (NID_SET_FRAME_BUF, NID_WAIT_VBLANK_START)

# Presentation parameters. Stride must be a multiple of 64 and the format must
# be 0..3; src/rt/hle.c rejects anything else, which is part of what the gate
# proves is being honoured.
FRAMEBUFFER = 0x04000000
STRIDE = 512
PIXEL_FORMAT = 3  # PSP_DISPLAY_PIXEL_FORMAT_8888
PIXELS = STRIDE * 272
DEFAULT_FRAMES = 240

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
    """The whole guest: a frame loop around a framebuffer fill and a flip."""
    if not 1 <= frames <= 0x7FFF:
        raise ValueError(f"frame count {frames} does not fit a signed 16-bit immediate")
    return [
        # prologue
        _i(0x09, SP, SP, -32),           # 0x00 addiu sp, sp, -32
        _i(0x2B, SP, RA, 28),            # 0x04 sw ra, 28(sp)
        _i(0x2B, SP, S0, 24),            # 0x08 sw s0, 24(sp)
        _r(ZERO, ZERO, S0, 0, 0x21),     # 0x0C addu s0, zero, zero   (frame = 0)
        # frame_loop (0x10)
        _i(0x0F, ZERO, T0, 0x0400),      # 0x10 lui t0, 0x0400        (vram cursor)
        _r(ZERO, ZERO, T1, 0, 0x21),     # 0x14 addu t1, zero, zero   (i = 0)
        _i(0x0F, ZERO, T2, PIXELS >> 16),  # 0x18 lui t2, hi(PIXELS)
        _i(0x0D, T2, T2, PIXELS & 0xFFFF),  # 0x1C ori t2, t2, lo(PIXELS)
        # pixel_loop (0x20)
        _r(ZERO, T1, T3, 4, 0x02),       # 0x20 srl t3, t1, 4
        _r(T3, S0, T3, 0, 0x21),         # 0x24 addu t3, t3, s0
        _i(0x0C, T3, T3, 0xFF),          # 0x28 andi t3, t3, 0xFF     (red)
        _i(0x0E, T3, T4, 0xFF),          # 0x2C xori t4, t3, 0xFF     (green)
        _r(ZERO, T3, T5, 1, 0x00),       # 0x30 sll t5, t3, 1
        _i(0x0C, T5, T5, 0xFF),          # 0x34 andi t5, t5, 0xFF     (blue)
        _r(ZERO, T5, T6, 16, 0x00),      # 0x38 sll t6, t5, 16
        _r(ZERO, T4, T7, 8, 0x00),       # 0x3C sll t7, t4, 8
        _r(T6, T7, T6, 0, 0x25),         # 0x40 or t6, t6, t7
        _r(T6, T3, T6, 0, 0x25),         # 0x44 or t6, t6, t3
        _i(0x0F, ZERO, T8, 0xFF00),      # 0x48 lui t8, 0xFF00        (alpha)
        _r(T6, T8, T6, 0, 0x25),         # 0x4C or t6, t6, t8
        _i(0x2B, T0, T6, 0),             # 0x50 sw t6, 0(t0)
        _i(0x09, T0, T0, 4),             # 0x54 addiu t0, t0, 4
        _i(0x09, T1, T1, 1),             # 0x58 addiu t1, t1, 1
        _i(0x05, T1, T2, -16),           # 0x5C bne t1, t2, pixel_loop
        0,                               # 0x60 nop (delay slot)
        # present
        _i(0x0F, ZERO, A0, FRAMEBUFFER >> 16),  # 0x64 lui a0, 0x0400
        _i(0x09, ZERO, A1, STRIDE),      # 0x68 addiu a1, zero, 512
        _i(0x09, ZERO, A2, PIXEL_FORMAT),  # 0x6C addiu a2, zero, 3
        _i(0x09, ZERO, A3, 1),           # 0x70 addiu a3, zero, 1     (sync = 1)
        _j(0x03, SETFB_STUB),            # 0x74 jal sceDisplaySetFrameBuf
        0,                               # 0x78 nop (delay slot)
        _j(0x03, VBLANK_STUB),           # 0x7C jal sceDisplayWaitVblankStart
        0,                               # 0x80 nop (delay slot)
        _i(0x09, S0, S0, 1),             # 0x84 addiu s0, s0, 1
        _i(0x0A, S0, T9, frames),        # 0x88 slti t9, s0, FRAMES
        _i(0x05, T9, ZERO, -32),         # 0x8C bne t9, zero, frame_loop
        0,                               # 0x90 nop (delay slot)
        # epilogue
        _i(0x23, SP, RA, 28),            # 0x94 lw ra, 28(sp)
        _i(0x23, SP, S0, 24),            # 0x98 lw s0, 24(sp)
        _r(RA, 0, 0, 0, 0x08),           # 0x9C jr ra
        _i(0x09, SP, SP, 32),            # 0xA0 addiu sp, sp, 32 (delay slot)
    ]


def relocation_records() -> list[tuple[int, int]]:
    """The ordered PSP type-A relocation table.

    Segment 0 is .text, segment 1 is .data. The two R_MIPS_26 records rebase the
    calls to the import stubs; the R_MIPS_32 records rebase the module-info and
    import-table pointers. The stub-table pointer is the only data word that
    targets the text segment.
    """
    return [
        (0x74, relocation_info(R_MIPS_26, 0, 0)),  # jal sceDisplaySetFrameBuf
        (0x7C, relocation_info(R_MIPS_26, 0, 0)),  # jal sceDisplayWaitVblankStart
        (0x2C, relocation_info(R_MIPS_32, 1, 1)),  # module libstub
        (0x30, relocation_info(R_MIPS_32, 1, 1)),  # module libstubend
        (0x50, relocation_info(R_MIPS_32, 1, 1)),  # library name
        (0x5C, relocation_info(R_MIPS_32, 1, 1)),  # NID table
        (0x60, relocation_info(R_MIPS_32, 1, 0)),  # first import stub (into .text)
    ]


def build_text_segment(frames: int) -> bytes:
    entry = _entry_words(frames)
    if len(entry) * 4 != TEXT_SECTION_SIZE:
        raise AssertionError(
            f"entry body is {len(entry) * 4:#x} bytes but .text is declared {TEXT_SECTION_SIZE:#x}"
        )
    text = bytearray(TEXT_FILE_SIZE)
    text[0 : len(entry) * 4] = _words(entry)
    # Two 8-byte import slots: the recompiler pairs each with its NID and routes
    # the call to the HLE handler, so the syscall word is never executed.
    text[STUB_OFFSET : STUB_OFFSET + 16] = _words(
        [0x03E00008, 0x0000000C, 0x03E00008, 0x0000000C]
    )
    return bytes(text)


def build_data_segment() -> bytes:
    data = bytearray(DATA_FILE_SIZE)
    module_name = b"display-smoke-v1"
    library_name = LIBRARY.encode("ascii") + b"\0"
    data[0:MODULE_INFO_SIZE] = struct.pack(
        "<HH28s5I",
        0,
        0x0100,
        module_name + b"\0" * (28 - len(module_name)),
        0,
        0,
        0,
        LIBSTUB_OFFSET,
        LIBSTUB_OFFSET + LIBSTUB_SIZE,
    )
    if len(library_name) > LIBSTUB_OFFSET - LIBRARY_NAME_OFFSET:
        raise AssertionError("library name no longer fits the fixed layout")
    data[LIBRARY_NAME_OFFSET : LIBRARY_NAME_OFFSET + len(library_name)] = library_name
    data[LIBSTUB_OFFSET : LIBSTUB_OFFSET + LIBSTUB_SIZE] = struct.pack(
        "<IHHBBHII",
        LIBRARY_NAME_OFFSET,
        0x0101,
        0x0009,
        5,
        0,
        len(NIDS),
        NID_TABLE_OFFSET,
        STUB_OFFSET,
    )
    struct.pack_into(f"<{len(NIDS)}I", data, NID_TABLE_OFFSET, *NIDS)
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
                DATA_FILE_OFFSET + LIBSTUB_OFFSET, LIBSTUB_SIZE, 4,
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


def manifest_bytes(prx: bytes, psp_header: bytes, frames: int) -> bytes:
    manifest = {
        "schema": 1,
        "kind": "source-owned-psp-display-smoke",
        "title_id": TITLE_ID,
        "base": f"0x{BASE:08x}",
        "entry": f"0x{ENTRY:08x}",
        "library": LIBRARY,
        "nids": [f"0x{nid:08x}" for nid in NIDS],
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
        (0x74, SETFB_STUB, "sceDisplaySetFrameBuf"),
        (0x7C, VBLANK_STUB, "sceDisplayWaitVblankStart"),
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


def run(build_dir: Path, gui: bool = False) -> int:
    fixture = build_dir / "fixture"
    manifest = _read_manifest(fixture)
    frames = int(manifest["frames"])
    expected = final_frame_first_pixel(frames)
    executable = build_dir / f"{ARTIFACT_STEM}.exe"
    if not executable.exists():
        executable = build_dir / ARTIFACT_STEM
    image_path = build_dir / f"{ARTIFACT_STEM}_image.bin"
    command = [
        str(executable),
        "--image",
        str(image_path),
        f"0x{BASE:08x}",
        f"0x{ENTRY:08x}",
        "none",
        "none",
        "--gui" if gui else "--sched",
        f"--expect-u32=0x{FRAMEBUFFER:08x}:0x{expected:08x}",
    ]
    completed = subprocess.run(
        command, cwd=ROOT, env=os.environ.copy(), capture_output=True, text=True
    )
    write_if_changed(build_dir / f"{ARTIFACT_STEM}.stdout.log", completed.stdout.encode("utf-8"))
    write_if_changed(build_dir / f"{ARTIFACT_STEM}.stderr.log", completed.stderr.encode("utf-8"))
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
    missing = [marker for marker in markers if marker not in combined]
    if missing:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence omits: " + ", ".join(missing))
    forbidden = ("UNKNOWN NID", "NONPLT_MISS", "INTERP_REJECT", "status=FAIL")
    present = [marker for marker in forbidden if marker in combined]
    if present:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence contains: " + ", ".join(present))
    print(
        f"DISPLAY_SMOKE_RUN status=PASS mode={'gui' if gui else 'headless'} "
        f"frames={frames} framebuffer=0x{FRAMEBUFFER:08x} value=0x{expected:08x}"
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
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "generate":
            return generate(args.out_dir, frames=args.frames)
        if args.command == "verify":
            return verify(args.build_dir)
        if args.command == "run":
            return run(args.build_dir, gui=args.gui)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        sys.stderr.write(f"DISPLAY_SMOKE_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
