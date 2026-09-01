# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate, qualify, and run the source-owned second-platform workload ladder.

The committed fixture is this recipe, not a binary.  The ladder is a family of
independently generated PSP-shaped ELF32 PRX/``~PSP`` guests that traverse the
ordinary loader -> relocation -> analyzer -> codegen -> production link ->
driver -> scheduler -> HLE path with identities deliberately disjoint from the
retail title and from fixtures/production_smoke:

    ladder-zero   L0  single-function CPU guest; no imports, no relocations,
                      no ~PSP header, nonzero entry offset, one result word.
    ladder-reloc  L1  multi-function guest; forward/backward direct transfers,
                      an indirect call through a relocated function-pointer
                      table, three relocated HI16/LO16 pointer pairs, an
                      unreferenced dead-anchor function, and a result written
                      into the .bss extent beyond the file bytes.
    ladder-gap    L3  build-time mode of ladder-reloc: the interior callee is
                      omitted from native emission (--omit-aot), so the call
                      reaches it through the ordinary production dispatch()
                      interpreter seam and hands back to registered AOT code.
                      Its emitted LUI/ADDIU pairs are independently decoded
                      to prove the end handoff consumes scratch_B; A is left
                      distinct so an A substitution changes the result.
    ladder-sched  L2  scheduler + real HLE kernel objects. Imports six
                      ThreadManForUser NIDs; a second guest thread writes a
                      marker and signals an event flag while the entry thread
                      blocks on it; the result depends on cross-thread memory
                      visibility through the scheduler.
    ladder-fpu    L4  scalar-FPU guest consuming the #120 production contract.
    ladder-fs     L5  VFS/file guest reading a source-owned payload through
                      SR_FSDIR, with an open-failure negative control.
    ladder-title2 T2  production-path callback/thread/event/file/interpreter
                      fixture with a nine-word result block and a separate
                      unsupported-NID fatal negative control.

Every workload differs from every other tracked workload in base address,
entry placement, segment/BSS layout, import identity, or data placement.  None
requires a title manifest, SR_DATAROOT, an exact hook, or any compatibility
override.
"""

from __future__ import annotations

import argparse
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


ROOT = Path(__file__).resolve().parents[2]

R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
SHT_PRX_RELOC = 0x700000A0

TEXT_FILE_OFFSET = 0x100
DATA_FILE_OFFSET = 0xC00
RELOC_FILE_OFFSET = 0x1400
STUB_AREA_OFFSET = 0xA80
DATA_SEG_DEFAULT_VADDR = 0x2000

NID_CREATE_THREAD = 0x446D8DE6
NID_START_THREAD = 0xF475845D
NID_EXIT_THREAD = 0xAA73C935
NID_CREATE_EVENT_FLAG = 0x55C20A00
NID_SET_EVENT_FLAG = 0x1FB15A32
NID_WAIT_EVENT_FLAG = 0x402FCF22
LIB_THREADMAN = "ThreadManForUser"

NID_IO_OPEN = 0x109F50BC
NID_IO_READ = 0x6A638D83
NID_IO_CLOSE = 0x810C4BC3
LIB_IOFILEMGR = "IoFileMgrForUser"
L5_PAYLOAD_NAME = "platform_ladder_l5.txt"
L5_PAYLOAD_BYTES = b"Nakagawa platform ladder L5 source-owned payload\n"
L5_FAIL_SENTINEL = 0xF51D0001
L5_BUFFER_SIZE = 0x80

NID_CREATE_CALLBACK = 0xE81CAF8F
NID_NOTIFY_CALLBACK = 0xC11BA8C4
NID_SET_EVENT_FLAG = 0x1FB15A32
NID_WAIT_EVENT_FLAG = 0x402FCF22
NID_CHECK_CALLBACK = 0x349D6D6C
LIB_KERNEL_CALLBACK = "ThreadManForUser"

TITLE2_BASE = 0x08A40000
TITLE2_ENTRY_OFF = 0x20
TITLE2_GAP_OFF = 0x220
TITLE2_RESULT_OFF = 0x340
TITLE2_EVENT_UID_OFF = 0x370
TITLE2_CALLBACK_UID_OFF = 0x374
TITLE2_WORKER_UID_OFF = 0x378
TITLE2_PATH_OFF = 0x500
TITLE2_EVENT_NAME_OFF = 0x400
TITLE2_CALLBACK_NAME_OFF = 0x420
TITLE2_WORKER_NAME_OFF = 0x430
TITLE2_PAYLOAD_OFF = 0x080
TITLE2_PAYLOAD_LEN = 69
TITLE2_PAYLOAD_CHECKSUM = 0x0000188B
TITLE2_GAP_WORDS = (
    0x24020007,
    0x3C0808A4,
    0x25083558,
    0x8D090000,
    0x25290007,
    0xAD090000,
    0x01224026,
    0x03E00008,
    0x24420000,
)
TITLE2_MEM_CELL = 0x08A43558
TITLE2_MEM_CELL_INIT = 7
TITLE2_MEM_CELL_EXPECTED = 14
TITLE2_CALLBACK_OFF = 0x260
TITLE2_CHECKSUM_OFF = 0x300
TITLE2_WORKER_OFF = 0x3A0
TITLE2_DATA_SIZE = 0x55C
TITLE2_MEM_CELL_OFF = 0x558
TITLE2_DATA_MARKER = 0x13579BDF
TITLE2_CALLBACK_MARKER = 0xCBACCA11
TITLE2_CALLBACK_ARG = 0x2468ACE0
TITLE2_WORKER_MARKER = 0x00C0FFEE
TITLE2_IDENTITY_MARKER = 0x54495432
TITLE2_STATUS_FAILURE = 0xDEAD0001
TITLE2_UNSUPPORTED_NID = 0xDEAD2F02
TITLE2_PATH_BYTES = b"ms0:/platform_ladder_title2.txt\0"
TITLE2_CALLBACK_NAME_BYTES = b"title2_callback\0"
TITLE2_WORKER_NAME_BYTES = b"title2_worker\0"
TITLE2_NEGATIVE_BASE = 0x08A80000
TITLE2_NEGATIVE_ENTRY_OFF = 0x20
TITLE2_NEGATIVE_DATA_SIZE = 0x340


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


def _jword(opcode: int, byte_target: int) -> int:
    return ((opcode & 63) << 26) | ((byte_target >> 2) & 0x03FFFFFF)


def relocation_info(relocation_type: int, offset_segment: int, target_segment: int) -> int:
    return relocation_type | (offset_segment << 8) | (target_segment << 16)


def f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def s32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


class Asm:
    """Label-resolving MIPS32 word emitter with relocation-site recording."""

    def __init__(self) -> None:
        self.words: list[int] = []
        self._labels: dict[str, int] = {}
        self._branch_fixups: list[tuple[int, str]] = []
        self.reloc_sites: list[tuple[int, int, int]] = []  # (word idx, type, target seg)

    def here(self) -> int:
        return len(self.words)

    def label(self, name: str) -> None:
        if name in self._labels:
            raise AssertionError(f"duplicate label {name}")
        self._labels[name] = len(self.words)

    def raw(self, word: int) -> None:
        self.words.append(word & 0xFFFFFFFF)

    def nop(self) -> None:
        self.raw(_r(0, 0, 0, 0, 0x25))

    def i(self, opcode: int, rs: int, rt: int, immediate: int) -> None:
        self.raw(_i(opcode, rs, rt, immediate))

    def rr(self, rs: int, rt: int, rd: int, shift: int, function: int) -> None:
        self.raw(_r(rs, rt, rd, shift, function))

    def beq(self, rs: int, rt: int, label: str) -> None:
        index = len(self.words)
        self.raw(_i(0x04, rs, rt, 0))
        self._branch_fixups.append((index, f"br:{rs}:{rt}:{label}"))

    def bne(self, rs: int, rt: int, label: str) -> None:
        index = len(self.words)
        self.raw(_i(0x05, rs, rt, 0))
        self._branch_fixups.append((index, f"brn:{rs}:{rt}:{label}"))

    def bltz(self, rs: int, label: str) -> None:
        index = len(self.words)
        self.raw(_i(0x01, rs, 0, 0))
        self._branch_fixups.append((index, f"bltz:{rs}:{label}"))

    def bc1t(self, label: str) -> None:
        index = len(self.words)
        self.raw((0x11 << 26) | (0x08 << 21) | (1 << 16))
        self._branch_fixups.append((index, f"bc1t:{label}"))

    def bc1f(self, label: str) -> None:
        index = len(self.words)
        self.raw((0x11 << 26) | (0x08 << 21) | (0 << 16))
        self._branch_fixups.append((index, f"bc1f:{label}"))

    def pad_to(self, byte_offset: int) -> None:
        while self.here() * 4 < byte_offset:
            self.raw(0)

    def resolve(self) -> list[int]:
        words = list(self.words)
        for index, spec in self._branch_fixups:
            parts = spec.split(":")
            if parts[-1] not in self._labels:
                raise AssertionError(f"unresolved label {parts[-1]}")
            delta = (self._labels[parts[-1]] - (index + 1)) & 0xFFFF
            if parts[0] == "br":
                words[index] = _i(0x04, int(parts[1]), int(parts[2]), delta)
            elif parts[0] == "brn":
                words[index] = _i(0x05, int(parts[1]), int(parts[2]), delta)
            elif parts[0] == "bltz":
                words[index] = _i(0x01, int(parts[1]), 0, delta)
            elif parts[0] == "bc1t":
                words[index] = ((0x11 << 26) | (0x08 << 21) | (1 << 16)) | delta
            elif parts[0] == "bc1f":
                words[index] = ((0x11 << 26) | (0x08 << 21) | (0 << 16)) | delta
            else:
                raise AssertionError(spec)
        return words

    def li(self, reg: int, value: int) -> None:
        """lui/addiu pair with correct %hi carry; addiu's sign extension is
        part of the encoding, so the low half is emitted as its signed form."""
        value &= 0xFFFFFFFF
        if value < 0x8000:
            self.i(0x09, 0, reg, value)     # addiu reg, zero, value
            return
        hi = (((value >> 16) + ((value >> 15) & 1)) & 0xFFFF)
        lo = value - (hi << 16)             # in [-0x8000, 0x7FFF]
        self.i(0x0F, 0, reg, hi)
        if lo:
            self.i(0x09, reg, reg, lo & 0xFFFF)
        elif (hi << 16) != value:
            raise AssertionError("li decomposition failed")

    def load_addr_literal(self, reg: int, address: int) -> None:
        """Absolute lui/addiu pair; valid because GAME_BASE equals plan.base."""
        hi = ((address >> 16) + ((address >> 15) & 1)) & 0xFFFF
        lo = address - (hi << 16)
        self.i(0x0F, 0, reg, hi)
        self.i(0x09, reg, reg, lo & 0xFFFF)

    def load_addr_relocated(self, reg: int, data_offset: int) -> None:
        """HI16/LO16 pair whose immediates are data-segment-relative."""
        hi = ((data_offset >> 16) + ((data_offset >> 15) & 1)) & 0xFFFF
        lo = data_offset - (hi << 16)
        hi_index = len(self.words)
        self.i(0x0F, 0, reg, hi)
        lo_index = len(self.words)
        self.i(0x09, reg, reg, lo & 0xFFFF)
        self.reloc_sites.append((hi_index, R_MIPS_HI16, 1))
        self.reloc_sites.append((lo_index, R_MIPS_LO16, 1))

    def call_literal(self, address: int) -> None:
        self.raw(_jword(0x03, address))
        self.nop()

    def call_relocated(self, text_offset: int) -> None:
        index = len(self.words)
        self.raw(_jword(0x03, text_offset))
        self.reloc_sites.append((index, R_MIPS_26, 0))
        self.nop()


class Plan:
    """One source-owned guest identity."""

    def __init__(
        self,
        *,
        name: str,
        game_name: str,
        base: int,
        entry_offset: int,
        build_asm,
        imports: list[tuple[str, list[int]]],
        data_words: dict[int, int],
        data_strings: dict[int, bytes],
        data_file_size: int,
        data_mem_size: int,
        # Data words that hold guest code pointers (R_MIPS_32 against segment 0).
    data_text_pointers: list[int] | None = None,
    relocate_calls: bool,
        relocate_data: bool,
        result_addr_fn,
        expected_value_fn,
        env: dict[str, str] | None = None,
        psp_header: bool = True,
        text_pad_end: int = 0,
        data_seg_vaddr: int = DATA_SEG_DEFAULT_VADDR,
        label_contract: dict[str, int] | None = None,
        gap_omit_offset: int | None = None,
    ):
        self.name = name
        self.game_name = game_name
        self.base = base
        self.entry_offset = entry_offset
        self.build_asm = build_asm
        self.imports = imports
        self.data_words = dict(data_words)
        self.data_strings = dict(data_strings)
        self.data_file_size = data_file_size
        self.data_mem_size = data_mem_size
        self.relocate_calls = relocate_calls
        self.relocate_data = relocate_data
        self.data_text_pointers = list(data_text_pointers or [])
        self._result_addr_fn = result_addr_fn
        self._expected_value_fn = expected_value_fn
        self.env = dict(env or {})
        self.psp_header = psp_header
        self.text_pad_end = text_pad_end
        self.data_seg_vaddr = data_seg_vaddr
        self.label_contract = dict(label_contract or {})
        self.gap_omit_offset = gap_omit_offset

    @property
    def entry(self) -> int:
        return self.base + self.entry_offset

    def stub_address(self, flat_index: int) -> int:
        return self.base + STUB_AREA_OFFSET + 8 * flat_index

    def flat_nids(self) -> list[int]:
        out: list[int] = []
        for _, nids in self.imports:
            out.extend(nids)
        return out

    def result_addr(self) -> int:
        return self._result_addr_fn(self)

    def expected_value(self) -> int:
        return self._expected_value_fn(self)

    def call(self, asm: Asm, text_offset: int) -> None:
        if self.relocate_calls:
            asm.call_relocated(text_offset)
        else:
            asm.call_literal(self.base + text_offset)

    def addr_ref(self, asm: Asm, reg: int, data_offset: int) -> None:
        if self.relocate_data:
            asm.load_addr_relocated(reg, data_offset)
        else:
            asm.load_addr_literal(reg, self.base + self.data_seg_vaddr + data_offset)


# --- geometry ---------------------------------------------------------------

DATA_MODULE_INFO_NAME_OFF = 0x40
DATA_LIB_STUB_BASE = 0x100
DATA_NID_BASE = 0x180
DATA_LIB_ENT_BASE = 0x200
DATA_LIB_ENT_FUNC_TABLE = 0x21C
DATA_GUEST_BASE = 0x300
MODULE_INFO_ENT_TOP_FIELD = 36
MODULE_INFO_ENT_END_FIELD = 40
MODULE_INFO_STUB_TOP_FIELD = 44
MODULE_INFO_STUB_END_FIELD = 48
LIB_ENT_ENTRY_SIZE = 0x1C  # one 20-byte header + one func-table word + one var slot

L0_BASE = 0x08940000
L0_ENTRY_OFF = 0x40
L0_RESULT_OFF = 0x380

L1_BASE = 0x088C0000
L1_ENTRY_OFF = 0x20
L1_B_OFF = 0x60
L1_C_OFF = 0x140
L1_D_OFF = 0x1B0
L1_FN_TABLE = 0x380
L1_PTR_A = 0x388
L1_PTR_B = 0x390
L1_RESULT = 0x398
L1_DATA_FILE_SIZE = 0x39C
L1_DATA_MEM_SIZE = 0x3C0
L1_TEXT_PAD_END = 0x1E0

L2_BASE = 0x08900000
L2_ENTRY_OFF = 0x10
L2_WORKER_OFF = 0x140
L2_WORKER_NAME = 0x380
L2_EF_NAME = 0x390
L2_OUT_BITS = 0x3C0
L2_WORKER_MARK = 0x3C4
L2_RESULT = 0x3C8
L2_EF_UID = 0x3CC
L2_WORKER_UID = 0x3D0
L2_DATA_SIZE = 0x400

L4_BASE = 0x08980000
L4_ENTRY_OFF = 0x08
L4_RESULTS = 0x380
L4_RESULT_COUNT = 10
L4_CHECKSUM_INDEX = 10
L4_DATA_SIZE = L4_RESULTS + 4 * (L4_CHECKSUM_INDEX + 1)

L5_BASE = 0x089C0000
L5_ENTRY_OFF = 0x18
L5_PATH_STR = 0x400
L5_RESULT = 0x480
L5_FD_CELL = 0x484
L5_BUFFER = 0x500
L5_DATA_SIZE = 0x600

GAP_SOURCE = "ladder-reloc"


# --- workload bodies --------------------------------------------------------


def l0_expected(plan: Plan) -> int:
    prod = (1234 * 5678) & 0xFFFFFFFF
    t4 = (prod - plan.result_addr()) & 0xFFFFFFFF
    t5 = (0xCAFE0000 ^ t4) & 0xFFFFFFFF
    t6 = t5 >> 7
    s7 = 1 if s32(t5) < s32(t6) else 0
    if s7:
        t5 |= 0x00FF
    return (t5 + s7) & 0xFFFFFFFF


def build_l0(plan: Plan) -> Asm:
    a = Asm()
    a.pad_to(plan.entry_offset)
    a.label("entry")
    a.i(0x09, 29, 29, -32)
    a.i(0x2B, 29, 31, 24)
    a.i(0x09, 0, 8, 1234)
    a.i(0x09, 0, 9, 5678)
    a.rr(8, 9, 0, 0, 0x18)
    a.rr(0, 0, 10, 0, 0x12)
    plan.addr_ref(a, 11, L0_RESULT_OFF)
    a.rr(10, 11, 12, 0, 0x23)
    a.li(13, 0xCAFE0000)
    a.rr(13, 12, 13, 0, 0x26)
    a.rr(0, 13, 14, 7, 0x02)
    a.rr(13, 14, 15, 0, 0x2A)
    a.bne(15, 0, "odd")
    a.nop()
    a.beq(0, 0, "join")
    a.nop()
    a.label("odd")
    a.i(0x0D, 13, 13, 0x00FF)
    a.label("join")
    a.rr(13, 15, 13, 0, 0x21)
    a.i(0x2B, 11, 13, 0)
    a.i(0x23, 11, 2, 0)
    a.i(0x23, 29, 31, 24)
    a.i(0x09, 29, 29, 32)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    return a


def l1_expected(plan: Plan) -> int:
    # Guest flow: B sums 100 three times into s0 (300); the indirect callee
    # returns a0+3 = 24 and stores a marker through the relocated pointer;
    # the final result XORs the seed with (24 + 300).
    c_out = (21 + 3) & 0xFFFFFFFF
    v0 = (c_out + 300) & 0xFFFFFFFF
    return (0xDEADFACE ^ v0) & 0xFFFFFFFF


def build_l1(plan: Plan) -> Asm:
    a = Asm()

    def ref(reg: int, off: int) -> None:
        plan.addr_ref(a, reg, off)

    a.i(0x09, 29, 29, -16)
    a.i(0x2B, 29, 31, 12)
    plan.call(a, L1_D_OFF)
    plan.call(a, L1_C_OFF)
    a.i(0x23, 29, 31, 12)
    a.i(0x09, 29, 29, 16)
    a.pad_to(L1_ENTRY_OFF)

    a.label("entry")
    a.i(0x09, 29, 29, -16)
    a.i(0x2B, 29, 31, 12)
    plan.call(a, L1_B_OFF)
    a.i(0x23, 29, 31, 12)
    a.i(0x09, 29, 29, 16)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    a.pad_to(L1_B_OFF)

    a.label("func_b")
    a.i(0x09, 29, 29, -48)
    a.i(0x2B, 29, 31, 44)
    a.i(0x2B, 29, 16, 40)
    a.i(0x2B, 29, 17, 36)
    a.i(0x09, 0, 8, 3)                  # t0 = 3 iterations
    a.i(0x09, 0, 16, 0)                 # s0 = 0 (sum): callee-saved across the call
    ref(10, L1_PTR_A)
    a.label("loop")
    a.i(0x09, 16, 16, 100)
    a.i(0x2B, 10, 0, 0)
    a.i(0x09, 8, 8, -1)
    a.bne(8, 0, "loop")
    a.nop()
    ref(12, L1_FN_TABLE)
    a.i(0x23, 12, 25, 0)
    a.i(0x09, 0, 4, 21)
    # Linked indirect call through the relocated pointer table. The callee is
    # a floor-safe leaf; B1 owns the epilogue and continues inline after the
    # dispatched callee host-returns (the production dispatch contract).
    a.rr(25, 0, 31, 0, 0x09)            # jalr t9
    a.nop()
    a.rr(2, 16, 2, 0, 0x21)             # v0 += s0
    ref(11, L1_PTR_B)
    a.i(0x23, 11, 13, 0)
    a.rr(13, 2, 13, 0, 0x26)
    ref(14, L1_RESULT)
    a.i(0x2B, 14, 13, 0)
    a.i(0x23, 14, 2, 0)
    a.i(0x23, 29, 17, 36)
    a.i(0x23, 29, 16, 40)
    a.i(0x23, 29, 31, 44)
    a.i(0x09, 29, 29, 48)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    a.pad_to(L1_C_OFF)

    a.label("func_c")
    # Floor-expressible body: in ladder-gap mode this function executes on the
    # #118 production interpreter floor, which supports ADDIU/LUI/SW/LW plus
    # jr/j/jal transfers. Every instruction here stays inside that set so the
    # same guest bytes run identically AOT (normal) and interpreted (gap).
    a.i(0x09, 4, 2, 3)                  # v0 = a0 + 3
    ref(8, L1_PTR_A)
    a.li(9, 0x00C01234)                 # marker word (floor-safe li decomposition)
    a.i(0x2B, 8, 9, 0)                  # store marker through the pointer
    a.rr(31, 0, 0, 0, 0x08)
    a.i(0x09, 2, 2, 0)                  # delay: addiu v0,v0,0 (floor-safe)
    a.pad_to(L1_D_OFF)

    a.label("func_d")
    ref(8, L1_RESULT)
    a.i(0x23, 8, 9, 0)
    a.i(0x0D, 9, 9, 0x5A5A)
    a.i(0x2B, 8, 9, 0)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    return a


def l2_expected(plan: Plan) -> int:
    return 0x00C00000 | 0x00000FE7


def build_l2(plan: Plan) -> Asm:
    a = Asm()

    def ref(reg: int, off: int) -> None:
        plan.addr_ref(a, reg, off)

    s_create_ef = plan.stub_address(0)
    s_set_ef = plan.stub_address(1)
    s_wait_ef = plan.stub_address(2)
    s_create_th = plan.stub_address(3)
    s_start_th = plan.stub_address(4)
    s_exit_th = plan.stub_address(5)

    plan.call(a, L2_WORKER_OFF)
    a.pad_to(L2_ENTRY_OFF)

    a.label("entry")
    a.i(0x09, 29, 29, -64)
    a.i(0x2B, 29, 31, 60)
    ref(4, L2_EF_NAME)
    a.i(0x09, 0, 5, 0)
    a.i(0x09, 0, 6, 0)
    a.i(0x09, 0, 7, 0)
    a.call_literal(s_create_ef)
    ref(8, L2_EF_UID)
    a.i(0x2B, 8, 2, 0)
    ref(4, L2_WORKER_NAME)
    a.load_addr_literal(5, plan.base + L2_WORKER_OFF)
    a.i(0x09, 0, 6, 10)
    a.li(7, 0x2000)
    a.i(0x09, 0, 8, 0)
    a.i(0x2B, 29, 8, 16)
    a.i(0x2B, 29, 0, 20)
    a.call_literal(s_create_th)
    ref(8, L2_WORKER_UID)
    a.i(0x2B, 8, 2, 0)
    a.rr(2, 0, 4, 0, 0x21)
    a.i(0x09, 0, 5, 0)
    a.i(0x09, 0, 6, 0)
    a.call_literal(s_start_th)
    ref(4, L2_EF_UID)
    a.i(0x23, 4, 4, 0)
    a.i(0x09, 0, 5, 1)
    a.i(0x09, 0, 6, 0)
    ref(7, L2_OUT_BITS)
    a.i(0x2B, 29, 0, 16)
    a.call_literal(s_wait_ef)
    ref(8, L2_WORKER_MARK)
    a.i(0x23, 8, 9, 0)
    a.i(0x0D, 9, 9, 0x0FE7)
    ref(8, L2_RESULT)
    a.i(0x2B, 8, 9, 0)
    a.i(0x23, 8, 2, 0)
    a.i(0x23, 29, 31, 60)
    a.i(0x09, 29, 29, 64)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    a.pad_to(L2_WORKER_OFF)

    a.label("worker")
    ref(8, L2_WORKER_MARK)
    a.li(9, 0x00C00000)
    a.i(0x2B, 8, 9, 0)
    ref(4, L2_EF_UID)
    a.i(0x23, 4, 4, 0)
    a.i(0x09, 0, 5, 1)
    a.call_literal(s_set_ef)
    a.call_literal(s_exit_th)
    return a


def l4_results() -> list[int]:
    results: list[int] = []
    results.append(f32_bits(1.0))
    results.append(f32_bits(1.0 + 2.0**-23))
    results.append(f32_bits(16777218.0))
    results.append(0x00000021)
    results.append(0x00000000)
    results.append(0x00000002)
    results.append(4)
    results.append(3)
    results.append((-3) & 0xFFFFFFFF)
    results.append((4 << 16) | (4 << 8) | 3)
    return results


def l4_checksum() -> int:
    total = 0
    for value in l4_results():
        total = (total + value) & 0xFFFFFFFF
    return total


def l4_expected(plan: Plan) -> int:
    return l4_checksum()


def build_l4(plan: Plan) -> Asm:
    a = Asm()
    results_abs = plan.base + plan.data_seg_vaddr + L4_RESULTS

    def mtc1(rt: int, fs: int) -> None:
        a.raw((0x11 << 26) | (0x04 << 21) | ((rt & 31) << 16) | ((fs & 31) << 11))

    def mfc1(rt: int, fs: int) -> None:
        a.raw((0x11 << 26) | (0x00 << 21) | ((rt & 31) << 16) | ((fs & 31) << 11))

    def ctc1(rt: int) -> None:
        a.raw((0x11 << 26) | (0x06 << 21) | ((rt & 31) << 16) | (31 << 11))

    def fp(fmt_field: int, ft: int, fs: int, fd: int, funct: int) -> None:
        a.raw(
            (0x11 << 26)
            | ((fmt_field & 31) << 21)
            | ((ft & 31) << 16)
            | ((fs & 31) << 11)
            | ((fd & 31) << 6)
            | funct
        )

    def set_fcr31(value: int) -> None:
        a.li(8, value)
        ctc1(8)

    def put_f(reg: int, bits: int) -> None:
        a.li(8, bits)
        mtc1(8, reg)

    def store_word(index: int, src: int) -> None:
        a.load_addr_literal(15, results_abs + 4 * index)
        a.i(0x2B, 15, src, 0)

    a.raw(0)
    a.i(0x09, 29, 29, -64)
    a.i(0x2B, 29, 31, 60)

    put_f(0, 0x3F800000)
    put_f(1, 0x33800000)
    fp(0x10, 1, 0, 2, 0x00)
    mfc1(9, 2)
    store_word(0, 9)

    put_f(1, 0x34000000)
    fp(0x10, 1, 0, 2, 0x00)
    mfc1(9, 2)
    store_word(1, 9)

    set_fcr31(1)
    put_f(3, 0x01000003)
    fp(0x14, 0, 3, 4, 0x20)
    mfc1(9, 4)
    store_word(2, 9)
    set_fcr31(0)

    put_f(5, f32_bits(1.5))
    put_f(6, f32_bits(2.0))
    fp(0x10, 6, 5, 0, 0x3C)
    a.bc1t("lt_taken")
    a.nop()
    a.i(0x09, 0, 9, 0)
    a.beq(0, 0, "fold")
    a.nop()
    a.label("lt_taken")
    a.i(0x0D, 0, 9, 0x0001)
    a.label("fold")
    fp(0x10, 5, 6, 0, 0x3C)
    a.bc1f("nt_taken")
    a.nop()
    a.i(0x09, 0, 10, 0)
    a.label("nt_taken")
    a.i(0x0D, 0, 10, 0x0020)
    a.rr(9, 10, 9, 0, 0x25)
    store_word(3, 9)

    set_fcr31(0x01000000)
    put_f(7, 0x00000003)
    put_f(8, f32_bits(0.5))
    fp(0x10, 8, 7, 9, 0x02)
    mfc1(9, 9)
    store_word(4, 9)

    set_fcr31(0x00000000)
    fp(0x10, 8, 7, 9, 0x02)
    mfc1(9, 9)
    store_word(5, 9)

    put_f(10, f32_bits(3.5))
    fp(0x10, 0, 10, 11, 0x24)
    mfc1(9, 11)
    store_word(6, 9)

    set_fcr31(1)
    fp(0x10, 0, 10, 11, 0x24)
    mfc1(9, 11)
    store_word(7, 9)
    set_fcr31(0)

    put_f(12, f32_bits(-3.75))
    fp(0x10, 0, 12, 11, 0x0D)
    mfc1(9, 11)
    store_word(8, 9)

    put_f(12, f32_bits(3.5))
    fp(0x10, 0, 12, 13, 0x0C)
    mfc1(9, 13)
    put_f(12, f32_bits(3.1))
    fp(0x10, 0, 12, 13, 0x0E)
    mfc1(10, 13)
    put_f(12, f32_bits(3.9))
    fp(0x10, 0, 12, 13, 0x0F)
    mfc1(11, 13)
    a.rr(0, 9, 9, 16, 0x00)
    a.rr(0, 10, 10, 8, 0x00)
    a.rr(9, 10, 9, 0, 0x25)
    a.rr(9, 11, 9, 0, 0x25)
    store_word(9, 9)

    a.i(0x09, 0, 9, 0)
    for index in range(L4_RESULT_COUNT):
        a.load_addr_literal(15, results_abs + 4 * index)
        a.i(0x23, 15, 10, 0)
        a.rr(9, 10, 9, 0, 0x21)
    store_word(L4_CHECKSUM_INDEX, 9)
    a.i(0x23, 15, 2, 0)
    a.i(0x23, 29, 31, 60)
    a.i(0x09, 29, 29, 64)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    return a


def l5_expected_ok(plan: Plan) -> int:
    n = len(L5_PAYLOAD_BYTES)
    total = sum(L5_PAYLOAD_BYTES) & 0x00FFFFFF
    return ((n << 24) | total) & 0xFFFFFFFF


def l5_path_data_words() -> dict[int, int]:
    """Guest-visible ms0:/ path bytes at L5_PATH_STR (NUL-terminated)."""
    encoded = ("ms0:/".encode("ascii") + L5_PAYLOAD_NAME.encode("ascii")) + b"\0"
    blob = bytearray(b"\0" * 0x40)
    blob[0 : len(encoded)] = encoded
    return {
        L5_PATH_STR + index: struct.unpack_from("<I", blob, index)[0]
        for index in range(0, len(blob), 4)
    }


def build_l5(plan: Plan) -> Asm:
    a = Asm()

    def ref(reg: int, off: int) -> None:
        plan.addr_ref(a, reg, off)

    buffer_abs = plan.base + plan.data_seg_vaddr + L5_BUFFER

    a.pad_to(L5_ENTRY_OFF)
    a.label("entry")
    a.i(0x09, 29, 29, -48)
    a.i(0x2B, 29, 31, 44)
    a.i(0x2B, 29, 16, 40)
    ref(4, L5_PATH_STR)
    a.i(0x09, 0, 5, 1)
    a.i(0x09, 0, 6, 0)
    a.call_literal(plan.stub_address(0))
    ref(8, L5_FD_CELL)
    a.i(0x2B, 8, 2, 0)
    a.bltz(2, "fail")
    a.nop()
    a.rr(2, 0, 16, 0, 0x21)
    a.rr(16, 0, 4, 0, 0x21)
    a.load_addr_literal(5, buffer_abs)
    a.i(0x09, 0, 6, L5_BUFFER_SIZE)
    a.call_literal(plan.stub_address(1))
    a.rr(2, 0, 17, 0, 0x21)             # s1 = n: callee-saved across close()
    a.rr(16, 0, 4, 0, 0x21)
    a.call_literal(plan.stub_address(2))
    a.load_addr_literal(8, buffer_abs)
    a.i(0x09, 0, 9, 0)
    a.rr(17, 0, 12, 0, 0x21)            # remaining = s1
    a.label("sum_loop")
    a.beq(12, 0, "sum_done")
    a.nop()
    a.i(0x09, 12, 12, -1)
    a.raw((0x24 << 26) | (8 << 21) | (13 << 16))  # lbu t3, 0(t0)
    a.i(0x09, 8, 8, 1)
    a.rr(9, 13, 9, 0, 0x21)
    a.beq(0, 0, "sum_loop")
    a.nop()
    a.label("sum_done")
    a.rr(0, 17, 13, 24, 0x00)           # t3 = n << 24 (sll; n rides in s1)
    a.li(14, 0x00FFFFFF)
    a.rr(9, 14, 9, 0, 0x24)
    a.rr(13, 9, 9, 0, 0x25)
    ref(8, L5_RESULT)
    a.i(0x2B, 8, 9, 0)
    a.i(0x23, 8, 2, 0)
    a.beq(0, 0, "epilogue")
    a.nop()
    a.label("fail")
    ref(8, L5_RESULT)
    a.li(9, L5_FAIL_SENTINEL)
    a.i(0x2B, 8, 9, 0)
    a.rr(0, 9, 2, 0, 0x21)
    a.label("epilogue")
    a.i(0x23, 29, 16, 40)
    a.i(0x23, 29, 31, 44)
    a.i(0x09, 29, 29, 48)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()
    return a


def title2_payload_bytes() -> bytes:
    return (
        b"Nakagawa title2 source-owned payload\n"
        b"config=0x13579bdf\n"
        b"route=generic\n"
    )


def title2_payload_words() -> dict[int, int]:
    payload = title2_payload_bytes()
    blob = bytearray(len(payload))
    blob[:] = payload
    return {
        TITLE2_PAYLOAD_OFF + index: struct.unpack_from("<I", blob, index)[0]
        for index in range(0, len(blob) - 1, 4)
    }


def title2_payload_trailing() -> dict[int, bytes]:
    payload = title2_payload_bytes()
    return {TITLE2_PAYLOAD_OFF + (len(payload) - 1): payload[len(payload) - 1:]}


def title2_path_words() -> dict[int, int]:
    blob = bytearray(len(TITLE2_PATH_BYTES))
    blob[:] = TITLE2_PATH_BYTES
    return {
        TITLE2_PATH_OFF + index: struct.unpack_from("<I", blob, index)[0]
        for index in range(0, len(blob) - 1, 4)
    }


def title2_path_strings() -> dict[int, bytes]:
    return {TITLE2_PATH_OFF + (len(TITLE2_PATH_BYTES) - 1): TITLE2_PATH_BYTES[len(TITLE2_PATH_BYTES) - 1:]}


def title2_checksum() -> int:
    return sum(title2_payload_bytes()) & 0xFFFFFFFF


def title2_result_expectations(plan: Plan) -> tuple[tuple[int, int], ...]:
    base = plan.base + plan.data_seg_vaddr + TITLE2_RESULT_OFF
    values = (
        TITLE2_DATA_MARKER,
        TITLE2_CALLBACK_MARKER,
        0x00000001,
        TITLE2_WORKER_MARKER,
        TITLE2_MEM_CELL_EXPECTED,
        TITLE2_PAYLOAD_LEN,
        TITLE2_PAYLOAD_CHECKSUM,
        TITLE2_IDENTITY_MARKER,
        0x00000000,
    )
    return tuple((base + 4 * index, value) for index, value in enumerate(values))


def title2_expected(plan: Plan) -> int:
    # The legacy single-result API remains useful to generic manifest tests;
    # the production run pins and checks all nine independent words above.
    return 0


def build_title2(plan: Plan) -> Asm:
    a = Asm()

    def ref(reg: int, off: int) -> None:
        plan.addr_ref(a, reg, off)

    s_create_ef = plan.stub_address(0)
    s_create_cb = plan.stub_address(1)
    s_create_th = plan.stub_address(2)
    s_start_th = plan.stub_address(3)
    s_notify_cb = plan.stub_address(4)
    s_set_ef = plan.stub_address(5)
    s_wait_ef = plan.stub_address(6)
    s_check_cb = plan.stub_address(7)
    s_exit_th = plan.stub_address(8)
    s_io_open = plan.stub_address(9)
    s_io_read = plan.stub_address(10)
    s_io_close = plan.stub_address(11)

    callback_entry = plan.base + TITLE2_CALLBACK_OFF
    checksum_entry = plan.base + TITLE2_CHECKSUM_OFF
    worker_entry = plan.base + TITLE2_WORKER_OFF
    path_abs = plan.base + plan.data_seg_vaddr + TITLE2_PATH_OFF
    buffer_abs = plan.base + plan.data_seg_vaddr + TITLE2_PAYLOAD_OFF
    result_abs = plan.base + plan.data_seg_vaddr + TITLE2_RESULT_OFF
    mem_cell_abs = TITLE2_MEM_CELL

    # These calls are analyzer seeds for functions reached through kernel
    # object state rather than a direct native call from the entry function.
    a.call_literal(callback_entry)
    a.call_literal(checksum_entry)
    a.call_literal(worker_entry)
    a.call_literal(plan.base + TITLE2_GAP_OFF)

    a.pad_to(TITLE2_ENTRY_OFF)
    a.label("entry")
    a.i(0x09, 29, 29, -64)
    a.i(0x2B, 29, 31, 60)
    a.i(0x2B, 29, 0, 16)                 # no timeout for WaitEventFlag

    ref(4, TITLE2_EVENT_NAME_OFF)
    a.i(0x09, 0, 5, 0)
    a.i(0x09, 0, 6, 0)
    a.i(0x09, 0, 7, 0)
    a.call_literal(s_create_ef)
    a.rr(2, 0, 16, 0, 0x21)              # s0 = real event UID
    a.bltz(2, "failure")
    a.nop()
    ref(8, TITLE2_EVENT_UID_OFF)
    a.i(0x2B, 8, 16, 0)

    ref(4, TITLE2_CALLBACK_NAME_OFF)
    a.load_addr_literal(5, callback_entry)
    a.i(0x09, 0, 6, 0)
    a.call_literal(s_create_cb)
    a.rr(2, 0, 17, 0, 0x21)
    a.bltz(2, "failure")
    a.nop()
    ref(8, TITLE2_CALLBACK_UID_OFF)
    a.i(0x2B, 8, 17, 0)

    ref(4, TITLE2_WORKER_NAME_OFF)
    a.load_addr_literal(5, worker_entry)
    a.i(0x09, 0, 6, 10)                  # higher priority than the entry thread
    a.li(7, 0x2000)
    a.call_literal(s_create_th)
    a.rr(2, 0, 18, 0, 0x21)              # s2 = real worker UID
    a.bltz(2, "failure")
    a.nop()

    a.rr(18, 0, 4, 0, 0x21)
    a.i(0x09, 0, 5, 0)
    a.i(0x09, 0, 6, 0)
    a.call_literal(s_start_th)
    a.bne(2, 0, "failure")
    a.nop()

    # Synchronous source-owned file I/O.  The buffer starts zeroed in the
    # guest image; only SR_FSDIR can make the checksum pass.
    ref(4, TITLE2_PATH_OFF)
    a.i(0x09, 0, 5, 1)
    a.i(0x09, 0, 6, 0)
    a.call_literal(s_io_open)
    a.rr(2, 0, 19, 0, 0x21)              # s3 = fd
    a.bltz(2, "failure")
    a.nop()

    a.rr(19, 0, 4, 0, 0x21)
    a.load_addr_literal(5, buffer_abs)
    a.i(0x09, 0, 6, TITLE2_PAYLOAD_LEN)
    a.call_literal(s_io_read)
    a.rr(2, 0, 20, 0, 0x21)              # s4 = read length
    a.rr(19, 0, 4, 0, 0x21)
    a.call_literal(s_io_close)
    a.li(9, TITLE2_PAYLOAD_LEN)
    a.bne(20, 9, "failure")
    a.nop()

    # The omitted function is entered through the ordinary production
    # dispatch_call/interpreter boundary.  Its oracle leaves v0=7 and writes
    # the 7 -> 14 transition into the fixed guest cell.
    a.li(4, 0x09)
    a.i(0x09, 0, 5, 0)
    a.call_literal(plan.base + TITLE2_GAP_OFF)
    # Ordinary waiting must not pump the callback.  The worker has already
    # set bit 1, or will wake this call, and only the following CheckCallback
    # dispatches the pending callback on the primary thread.
    a.rr(16, 0, 4, 0, 0x21)
    a.i(0x09, 0, 5, 1)
    a.i(0x09, 0, 6, 0)
    a.i(0x09, 0, 7, 0)
    a.call_literal(s_wait_ef)
    a.bne(2, 0, "failure")
    a.nop()

    a.i(0x09, 0, 4, 0)
    a.call_literal(s_check_cb)
    a.li(9, 1)
    a.bne(2, 9, "failure")
    a.nop()

    ref(8, TITLE2_RESULT_OFF)
    a.i(0x23, 8, 9, 8)                   # callback count
    a.li(10, 1)
    a.bne(9, 10, "failure")
    a.nop()
    a.i(0x23, 8, 9, 32)                  # callback's argument/status guard
    a.bne(9, 0, "failure")
    a.nop()

    # The checksum helper writes marker, length, and checksum only after
    # consuming the bytes read above.
    a.load_addr_literal(4, buffer_abs)
    a.i(0x09, 0, 5, TITLE2_PAYLOAD_LEN)
    a.call_literal(checksum_entry)
    a.load_addr_literal(8, result_abs)
    a.i(0x23, 8, 9, 32)
    a.bne(9, 0, "failure")
    a.nop()

    a.load_addr_literal(8, mem_cell_abs)
    a.i(0x23, 8, 9, 0)
    a.load_addr_literal(10, result_abs)
    a.i(0x2B, 10, 9, 16)                 # word4 = interpreted memory result
    a.li(9, TITLE2_IDENTITY_MARKER)
    a.i(0x2B, 10, 9, 28)                 # word7 = fixture identity
    a.beq(0, 0, "epilogue")
    a.nop()

    a.label("failure")
    a.load_addr_literal(8, result_abs)
    a.li(9, TITLE2_STATUS_FAILURE)
    a.i(0x2B, 8, 9, 32)
    a.i(0x09, 0, 2, 1)

    a.label("epilogue")
    a.i(0x23, 29, 31, 60)
    a.i(0x09, 29, 29, 64)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()

    if a.here() * 4 > TITLE2_GAP_OFF:
        raise RuntimeError(
            f"title2 primary flow crossed locked interpreter gap at 0x{a.here() * 4:x}"
        )
    a.pad_to(TITLE2_GAP_OFF)
    for word in TITLE2_GAP_WORDS:
        a.raw(word)

    a.pad_to(TITLE2_CALLBACK_OFF)
    a.label("callback")
    a.i(0x09, 29, 29, -32)
    a.i(0x2B, 29, 31, 28)
    a.li(8, TITLE2_CALLBACK_ARG)
    a.bne(5, 8, "callback_bad")
    a.nop()
    ref(8, TITLE2_RESULT_OFF)
    a.li(9, TITLE2_CALLBACK_MARKER)
    a.i(0x2B, 8, 9, 4)
    a.i(0x23, 8, 10, 8)
    a.i(0x09, 10, 10, 1)
    a.i(0x2B, 8, 10, 8)
    a.i(0x09, 0, 2, 0)
    a.beq(0, 0, "callback_done")
    a.nop()
    a.label("callback_bad")
    ref(8, TITLE2_RESULT_OFF)
    a.li(9, TITLE2_STATUS_FAILURE)
    a.i(0x2B, 8, 9, 32)
    a.i(0x09, 0, 2, 1)
    a.label("callback_done")
    a.i(0x23, 29, 31, 28)
    a.i(0x09, 29, 29, 32)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()

    a.pad_to(TITLE2_CHECKSUM_OFF)
    a.label("checksum")
    a.i(0x09, 29, 29, -32)
    a.i(0x2B, 29, 31, 28)
    a.rr(4, 0, 16, 0, 0x21)              # s0 = buffer
    a.rr(5, 0, 17, 0, 0x21)              # s1 = remaining
    a.rr(5, 0, 18, 0, 0x21)              # s2 = original length
    a.i(0x09, 0, 9, 0)                    # t1 = checksum
    a.label("checksum_loop")
    a.beq(17, 0, "checksum_done")
    a.nop()
    a.raw((0x24 << 26) | (16 << 21) | (10 << 16))  # lbu t2, 0(s0)
    a.rr(9, 10, 9, 0, 0x21)
    a.i(0x09, 16, 16, 1)
    a.i(0x09, 17, 17, -1)
    a.beq(0, 0, "checksum_loop")
    a.nop()
    a.label("checksum_done")
    ref(8, TITLE2_RESULT_OFF)
    a.i(0x2B, 8, 18, 20)                 # word5 = payload length
    a.i(0x2B, 8, 9, 24)                  # word6 = checksum
    a.li(10, TITLE2_PAYLOAD_CHECKSUM)
    a.bne(9, 10, "checksum_bad")
    a.nop()
    a.li(10, TITLE2_PAYLOAD_LEN)
    a.bne(18, 10, "checksum_bad")
    a.nop()
    a.li(10, TITLE2_DATA_MARKER)
    a.i(0x2B, 8, 10, 0)                  # word0 = expected-data marker
    a.i(0x09, 0, 2, 0)
    a.beq(0, 0, "checksum_return")
    a.nop()
    a.label("checksum_bad")
    a.li(10, TITLE2_STATUS_FAILURE)
    a.i(0x2B, 8, 10, 32)
    a.i(0x09, 0, 2, 1)
    a.label("checksum_return")
    a.i(0x23, 29, 31, 28)
    a.i(0x09, 29, 29, 32)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()

    a.pad_to(TITLE2_WORKER_OFF)
    a.label("worker")
    a.i(0x09, 29, 29, -32)
    a.i(0x2B, 29, 31, 28)
    ref(8, TITLE2_RESULT_OFF)
    a.li(9, TITLE2_WORKER_MARKER)
    a.i(0x2B, 8, 9, 12)                  # word3 = worker marker
    ref(8, TITLE2_CALLBACK_UID_OFF)
    a.i(0x23, 8, 4, 0)
    a.li(5, TITLE2_CALLBACK_ARG)
    a.call_literal(s_notify_cb)
    a.bne(2, 0, "worker_fail")
    a.nop()
    ref(8, TITLE2_EVENT_UID_OFF)
    a.i(0x23, 8, 4, 0)
    a.i(0x09, 0, 5, 1)
    a.call_literal(s_set_ef)
    a.bne(2, 0, "worker_fail")
    a.nop()
    a.i(0x09, 0, 4, 0)
    a.call_literal(s_exit_th)
    a.beq(0, 0, "worker_return")
    a.nop()
    a.label("worker_fail")
    ref(8, TITLE2_RESULT_OFF)
    a.li(9, TITLE2_STATUS_FAILURE)
    a.i(0x2B, 8, 9, 32)
    a.i(0x09, 0, 4, 1)
    a.call_literal(s_exit_th)
    a.label("worker_return")
    a.i(0x23, 29, 31, 28)
    a.i(0x09, 29, 29, 32)
    a.rr(31, 0, 0, 0, 0x08)
    a.nop()

    return a


def build_title2_negative(plan: Plan) -> Asm:
    a = Asm()
    a.pad_to(TITLE2_NEGATIVE_ENTRY_OFF)
    a.label("entry")
    a.call_literal(plan.stub_address(0))
    return a


# --- plans ------------------------------------------------------------------

PLANS: dict[str, Plan] = {}


def _register_plans() -> None:
    PLANS["ladder-zero"] = Plan(
        name="ladder-zero",
        game_name="pl_zero",
        base=L0_BASE,
        entry_offset=L0_ENTRY_OFF,
        build_asm=build_l0,
        imports=[],
        data_words={},
        data_strings={},
        data_file_size=L0_RESULT_OFF + 4,
        data_mem_size=L0_RESULT_OFF + 4,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + L0_RESULT_OFF,
        expected_value_fn=l0_expected,
        psp_header=False,
        data_seg_vaddr=0x4000,
    )

LG_BASE = 0x08A00000
LG_ENTRY_OFF = 0x10
LG_MID_OFF = 0x60
LG_END_OFF = 0xA0

LG_SEED_COPY_VALUE = 0x0000BEEF
LG_RESULT = 0x380
LG_SCRATCH_A = 0x388
LG_SCRATCH_B = 0x390
LG_DATA_SIZE = 0x3C0
LG_GUARD_A_VALUE = 0x00001234

# This is the semantic contract for the gap's address topology.  Keep the
# intended relative offsets independent from the arguments passed to
# ``Plan.addr_ref``: a candidate mutation may redirect one encoder call while
# leaving its labels and result constants looking plausible.  Each tuple is
# (LUI byte offset, ADDIU byte offset, intended data-relative offset).
LG_ADDRESS_CONTRACT: dict[str, tuple[int, int, int]] = {
    "mid_scratch_a": (0x64, 0x68, 0x388),
    "mid_scratch_b": (0x7C, 0x80, 0x390),
    "end_scratch_b": (0xA0, 0xA4, 0x390),
    "result": (0xB0, 0xB4, 0x380),
}
LG_MID_TO_END_OFFSET = 0x90
LG_END_LOAD_B_OFFSET = 0xA8


def lg_expected(plan: Plan) -> int:
    # entry: v0 = 2.  The direct jump's delay slot deliberately clears v0;
    # mid therefore starts from zero, adds seven, and its jump delay adds five.
    # The result is the copied seed XOR the value visible at the AOT handoff.
    v0 = 0 + 7 + 7
    return (LG_SEED_COPY_VALUE ^ v0) & 0xFFFFFFFF


def _decode_i(word: int) -> tuple[int, int, int, int]:
    """Independent decoder for the I-format instructions used by ladder-gap."""
    return ((word >> 26) & 0x3F, (word >> 21) & 0x1F, (word >> 16) & 0x1F, word & 0xFFFF)


def _decode_j(word: int) -> tuple[int, int]:
    return ((word >> 26) & 0x3F, (word & 0x03FFFFFF) << 2)


def _sign_extend16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _decode_lui_addiu_address(words: list[int], lui_offset: int, addiu_offset: int) -> tuple[int, int]:
    """Decode an emitted LUI/ADDIU pair and reconstruct its effective address.

    This deliberately consumes the instruction words, rather than any source
    label or encoder argument.  The gap plan uses absolute address pairs, so
    the reconstructed value is the address that the guest register receives
    before the subsequent load/store executes.
    """
    lui = _decode_i(words[lui_offset // 4])
    addiu = _decode_i(words[addiu_offset // 4])
    if lui[0] != 0x0F or lui[1] != 0 or addiu[0] != 0x09:
        raise RuntimeError(
            "ladder-gap address decode expected LUI/ADDIU at "
            f"0x{lui_offset:x}/0x{addiu_offset:x}"
        )
    if lui[2] != addiu[1] or addiu[1] != addiu[2]:
        raise RuntimeError(
            "ladder-gap address decode register mismatch at "
            f"0x{lui_offset:x}/0x{addiu_offset:x}"
        )
    effective = ((lui[3] << 16) + _sign_extend16(addiu[3])) & 0xFFFFFFFF
    return effective, lui[2]


def _verify_lg_address_topology(
    words: list[int],
    plan: Plan,
    recorded_refs: dict[str, tuple[int, int, int]] | None = None,
) -> None:
    """Prove the emitted address words and the executed mid-to-end seam.

    The fixed contract supplies only the intended semantic destination.  The
    actual result comes from independently decoding the emitted LUI/ADDIU
    words.  The end load is then tied to the decoded address register and the
    decoded mid->end jump, so a label-only comparison cannot authorize a
    scratch_A substitution.
    """
    decoded: dict[str, tuple[int, int]] = {}
    for name, (lui_offset, addiu_offset, intended_offset) in LG_ADDRESS_CONTRACT.items():
        if recorded_refs is not None:
            recorded = recorded_refs.get(name)
            if recorded is None or recorded[:2] != (lui_offset, addiu_offset):
                raise RuntimeError(f"ladder-gap address pair placement mismatch for {name}")
        effective, address_reg = _decode_lui_addiu_address(words, lui_offset, addiu_offset)
        intended = plan.base + plan.data_seg_vaddr + intended_offset
        if effective != intended:
            raise RuntimeError(
                "ladder-gap decoded effective address mismatch for "
                f"{name}: expected 0x{intended:08x}, got 0x{effective:08x}"
            )
        decoded[name] = (effective, address_reg)

    jump_opcode, jump_target = _decode_j(words[LG_MID_TO_END_OFFSET // 4])
    expected_target = plan.base + LG_END_OFF
    if jump_opcode != 0x02 or jump_target != (expected_target >> 2 << 2):
        raise RuntimeError(
            "ladder-gap mid-to-end handoff target mismatch: "
            f"expected 0x{expected_target:08x}, got 0x{jump_target:08x}"
        )
    end_load = _decode_i(words[LG_END_LOAD_B_OFFSET // 4])
    end_address_reg = decoded["end_scratch_b"][1]
    if end_load != (0x23, end_address_reg, 13, 0):
        raise RuntimeError(
            "ladder-gap end load is not the decoded scratch_B handoff: "
            f"{end_load!r}"
        )


def build_lg(plan: Plan, *, end_scratch_offset: int | None = None) -> Asm:
    a = Asm()
    critical: dict[str, tuple[int, str, tuple[int, ...]]] = {}
    address_refs: dict[str, tuple[int, int, int]] = {}
    actual_end_scratch = LG_SCRATCH_B if end_scratch_offset is None else end_scratch_offset

    def ref(label: str, reg: int, off: int) -> None:
        start = len(a.words) * 4
        plan.addr_ref(a, reg, off)
        if len(a.words) * 4 - start != 8:
            raise RuntimeError(f"ladder-gap address encoder did not emit a pair for {label}")
        address_refs[label] = (start, start + 4, off)

    def emit(label: str, mnemonic: str, word: int, *operands: int) -> None:
        critical[label] = (len(a.words) * 4, mnemonic, tuple(operands))
        a.raw(word)

    # Explicitly construct the gap body from semantic helpers.  Keep the
    # resulting words checked below by an independent decoder so comments can
    # never be the instruction oracle.
    plan.call(a, LG_MID_OFF)
    plan.call(a, LG_END_OFF)
    a.pad_to(LG_ENTRY_OFF)
    a.label("entry")
    emit("entry_seed", "addiu", _i(0x09, 0, 2, 2), 0, 2, 2)
    emit("entry_to_mid", "j", _jword(0x02, plan.base + LG_MID_OFF), plan.base + LG_MID_OFF)
    emit("entry_delay_clear", "addiu", _i(0x09, 0, 2, 0), 0, 2, 0)
    a.pad_to(LG_MID_OFF)
    a.label("mid")
    emit("mid_add", "addiu", _i(0x09, 2, 2, 7), 2, 2, 7)
    ref("mid_scratch_a", 8, LG_SCRATCH_A)
    a.li(9, LG_SEED_COPY_VALUE)
    emit("mid_store_a", "sw", _i(0x2B, 8, 9, 0), 8, 9, 0, LG_SCRATCH_A)
    emit("mid_load_a", "lw", _i(0x23, 8, 10, 0), 8, 10, 0, LG_SCRATCH_A)
    ref("mid_scratch_b", 11, LG_SCRATCH_B)
    emit("mid_store_b", "sw", _i(0x2B, 11, 10, 0), 11, 10, 0, LG_SCRATCH_B)
    # Leave a distinct value in A after the B copy.  The normal result still
    # comes from B, while an end-load-to-A mutation changes the executed
    # result instead of being masked by equal scratch contents.
    emit("mid_guard_value", "addiu", _i(0x09, 0, 10, LG_GUARD_A_VALUE), 0, 10, LG_GUARD_A_VALUE)
    emit("mid_guard_store_a", "sw", _i(0x2B, 8, 10, 0), 8, 10, 0, LG_SCRATCH_A)
    emit("mid_to_end", "j", _jword(0x02, plan.base + LG_END_OFF), plan.base + LG_END_OFF)
    emit("mid_delay_add", "addiu", _i(0x09, 2, 2, 7), 2, 2, 7)
    a.pad_to(LG_END_OFF)
    a.label("end")
    ref("end_scratch_b", 12, actual_end_scratch)
    emit("end_load_b", "lw", _i(0x23, 12, 13, 0), 12, 13, 0, LG_SCRATCH_B)
    a.rr(13, 2, 13, 0, 0x26)            # xor t5,t5,v0
    ref("result", 14, LG_RESULT)
    emit("end_store_result", "sw", _i(0x2B, 14, 13, 0), 14, 13, 0, LG_RESULT)
    emit("end_load_result", "lw", _i(0x23, 14, 2, 0), 14, 2, 0, LG_RESULT)
    a.rr(31, 0, 0, 0, 0x08)
    a.i(0x09, 2, 2, 0)

    # Independent semantic proof for every critical word and target.  The
    # address proof below consumes the emitted LUI/ADDIU words; it is not a
    # comparison of the source labels passed to the encoder.
    words = a.resolve()
    expected = {
        0x10: (0x09, 0, 2, 2),
        0x14: (0x02, plan.base + LG_MID_OFF),
        0x18: (0x09, 0, 2, 0),
        0x60: (0x09, 2, 2, 7),
        0x6C: (0x0F, 0, 9, 1),
        0x70: (0x09, 9, 9, 0xBEEF),
        0x74: (0x2B, 8, 9, 0),
        0x78: (0x23, 8, 10, 0),
        0x84: (0x2B, 11, 10, 0),
        0x88: (0x09, 0, 10, LG_GUARD_A_VALUE),
        0x8C: (0x2B, 8, 10, 0),
        0x90: (0x02, plan.base + LG_END_OFF),
        0x94: (0x09, 2, 2, 7),
        0xA8: (0x23, 12, 13, 0),
        0xB8: (0x2B, 14, 13, 0),
        0xBC: (0x23, 14, 2, 0),
    }
    for offset, semantic in expected.items():
        word = words[offset // 4]
        if len(semantic) == 4:
            if _decode_i(word) != semantic:
                raise RuntimeError(f"ladder-gap encoder/decoder mismatch at 0x{offset:x}")
        else:
            opcode, target = _decode_j(word)
            if opcode != semantic[0] or target != semantic[1] >> 2 << 2:
                raise RuntimeError(f"ladder-gap jump mismatch at 0x{offset:x}")
    expected_labels = {
        "entry_seed": (0x10, "addiu", (0, 2, 2)),
        "entry_to_mid": (0x14, "j", (plan.base + LG_MID_OFF,)),
        "entry_delay_clear": (0x18, "addiu", (0, 2, 0)),
        "mid_add": (0x60, "addiu", (2, 2, 7)),
        "mid_store_a": (0x74, "sw", (8, 9, 0, LG_SCRATCH_A)),
        "mid_load_a": (0x78, "lw", (8, 10, 0, LG_SCRATCH_A)),
        "mid_store_b": (0x84, "sw", (11, 10, 0, LG_SCRATCH_B)),
        "mid_guard_value": (0x88, "addiu", (0, 10, LG_GUARD_A_VALUE)),
        "mid_guard_store_a": (0x8C, "sw", (8, 10, 0, LG_SCRATCH_A)),
        "mid_to_end": (0x90, "j", (plan.base + LG_END_OFF,)),
        "mid_delay_add": (0x94, "addiu", (2, 2, 7)),
        "end_load_b": (0xA8, "lw", (12, 13, 0, LG_SCRATCH_B)),
        "end_store_result": (0xB8, "sw", (14, 13, 0, LG_RESULT)),
        "end_load_result": (0xBC, "lw", (14, 2, 0, LG_RESULT)),
    }
    if critical != expected_labels:
        raise RuntimeError(f"ladder-gap labeled topology mismatch: {critical!r}")
    _verify_lg_address_topology(words, plan, address_refs)
    return a


# --- plans ------------------------------------------------------------------

PLANS: dict[str, Plan] = {}


def _register_plans() -> None:
    PLANS["ladder-zero"] = Plan(
        name="ladder-zero",
        game_name="pl_zero",
        base=L0_BASE,
        entry_offset=L0_ENTRY_OFF,
        build_asm=build_l0,
        imports=[],
        data_words={},
        data_strings={},
        data_file_size=L0_RESULT_OFF + 4,
        data_mem_size=L0_RESULT_OFF + 4,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + L0_RESULT_OFF,
        expected_value_fn=l0_expected,
        psp_header=False,
        data_seg_vaddr=0x4000,
    )
    PLANS["ladder-reloc"] = Plan(
        name="ladder-reloc",
        game_name="pl_reloc",
        base=L1_BASE,
        entry_offset=L1_ENTRY_OFF,
        build_asm=build_l1,
        imports=[],
        data_words={
            L1_FN_TABLE: L1_C_OFF,
            L1_PTR_A: 0,
            L1_PTR_B: 0xDEADFACE,
        },
        data_strings={},
        data_file_size=L1_DATA_FILE_SIZE,
        data_mem_size=L1_DATA_MEM_SIZE,
        relocate_calls=True,
        relocate_data=True,
        data_text_pointers=[L1_FN_TABLE],
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + L1_RESULT,
        expected_value_fn=l1_expected,
        text_pad_end=L1_TEXT_PAD_END,
        label_contract={
            "entry": L1_ENTRY_OFF,
            "func_b": L1_B_OFF,
            "func_c": L1_C_OFF,
            "func_d": L1_D_OFF,
        },
    )
    PLANS["ladder-sched"] = Plan(
        name="ladder-sched",
        game_name="pl_sched",
        base=L2_BASE,
        entry_offset=L2_ENTRY_OFF,
        build_asm=build_l2,
        imports=[
            (
                LIB_THREADMAN,
                [
                    NID_CREATE_EVENT_FLAG,
                    NID_SET_EVENT_FLAG,
                    NID_WAIT_EVENT_FLAG,
                    NID_CREATE_THREAD,
                    NID_START_THREAD,
                    NID_EXIT_THREAD,
                ],
            )
        ],
        data_words={},
        data_strings={
            L2_WORKER_NAME: b"ladder_worker\0",
            L2_EF_NAME: b"ladder_ef\0",
        },
        data_file_size=L2_DATA_SIZE,
        data_mem_size=L2_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + L2_RESULT,
        expected_value_fn=l2_expected,
        env={"SR_DISPATCH_FATAL": "1"},
    )
    PLANS["ladder-fpu"] = Plan(
        name="ladder-fpu",
        game_name="pl_fpu",
        base=L4_BASE,
        entry_offset=L4_ENTRY_OFF,
        build_asm=build_l4,
        imports=[],
        data_words={},
        data_strings={},
        data_file_size=L4_DATA_SIZE,
        data_mem_size=L4_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=(
            lambda p: p.base + p.data_seg_vaddr + L4_RESULTS + 4 * L4_CHECKSUM_INDEX
        ),
        expected_value_fn=l4_expected,
        data_seg_vaddr=0x3000,
    )
    PLANS["ladder-gap"] = Plan(
        name="ladder-gap",
        game_name="pl_gap_chain",
        base=LG_BASE,
        entry_offset=LG_ENTRY_OFF,
        build_asm=build_lg,
        imports=[],
        data_words={
            LG_SCRATCH_A: 0,
            LG_SCRATCH_B: 0,
        },
        data_strings={},
        data_file_size=LG_DATA_SIZE,
        data_mem_size=LG_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + LG_RESULT,
        expected_value_fn=lg_expected,
        env={"SR_DISPATCH_FATAL": "1"},
        label_contract={"entry": LG_ENTRY_OFF, "mid": LG_MID_OFF, "end": LG_END_OFF},
        gap_omit_offset=LG_MID_OFF,
    )
    PLANS["ladder-fs"] = Plan(
        name="ladder-fs",
        game_name="pl_fs",
        base=L5_BASE,
        entry_offset=L5_ENTRY_OFF,
        build_asm=build_l5,
        imports=[(LIB_IOFILEMGR, [NID_IO_OPEN, NID_IO_READ, NID_IO_CLOSE])],
        data_words=l5_path_data_words(),
        data_strings={},
        data_file_size=L5_DATA_SIZE,
        data_mem_size=L5_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + L5_RESULT,
        expected_value_fn=l5_expected_ok,
        env={"SR_DISPATCH_FATAL": "1"},
    )
    PLANS["ladder-title2"] = Plan(
        name="ladder-title2",
        game_name="pl_title2",
        base=TITLE2_BASE,
        entry_offset=TITLE2_ENTRY_OFF,
        build_asm=build_title2,
        imports=[
            (LIB_KERNEL_CALLBACK, [
                NID_CREATE_EVENT_FLAG,
                NID_CREATE_CALLBACK,
                NID_CREATE_THREAD,
                NID_START_THREAD,
                NID_NOTIFY_CALLBACK,
                NID_SET_EVENT_FLAG,
                NID_WAIT_EVENT_FLAG,
                NID_CHECK_CALLBACK,
                NID_EXIT_THREAD,
            ]),
            (LIB_IOFILEMGR, [NID_IO_OPEN, NID_IO_READ, NID_IO_CLOSE]),
        ],
        data_words={
            **title2_path_words(),
            TITLE2_MEM_CELL_OFF: TITLE2_MEM_CELL_INIT,
        },
        data_strings={
            **title2_path_strings(),
            TITLE2_EVENT_NAME_OFF: b"title2_event\0",
            TITLE2_CALLBACK_NAME_OFF: TITLE2_CALLBACK_NAME_BYTES,
            TITLE2_WORKER_NAME_OFF: TITLE2_WORKER_NAME_BYTES,
        },
        data_file_size=TITLE2_DATA_SIZE,
        data_mem_size=TITLE2_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr + TITLE2_RESULT_OFF,
        expected_value_fn=title2_expected,
        env={"SR_DISPATCH_FATAL": "1"},
        label_contract={
            "entry": TITLE2_ENTRY_OFF,
            "callback": TITLE2_CALLBACK_OFF,
            "checksum": TITLE2_CHECKSUM_OFF,
            "worker": TITLE2_WORKER_OFF,
        },
        gap_omit_offset=TITLE2_GAP_OFF,
        data_text_pointers=[],
        data_seg_vaddr=0x3000,
        text_pad_end=TITLE2_WORKER_OFF + 0x100,
    )
    PLANS["ladder-title2-negative"] = Plan(
        name="ladder-title2-negative",
        game_name="pl_title2_negative",
        base=TITLE2_NEGATIVE_BASE,
        entry_offset=TITLE2_NEGATIVE_ENTRY_OFF,
        build_asm=build_title2_negative,
        imports=[(LIB_THREADMAN, [TITLE2_UNSUPPORTED_NID])],
        data_words={},
        data_strings={},
        data_file_size=TITLE2_NEGATIVE_DATA_SIZE,
        data_mem_size=TITLE2_NEGATIVE_DATA_SIZE,
        relocate_calls=False,
        relocate_data=False,
        result_addr_fn=lambda p: p.base + p.data_seg_vaddr,
        expected_value_fn=lambda p: 0,
        env={"SR_DISPATCH_FATAL": "1"},
        label_contract={"entry": TITLE2_NEGATIVE_ENTRY_OFF},
        data_seg_vaddr=0x3000,
    )


_register_plans()

WORKLOAD_CHOICES = sorted(set(PLANS) | {"ladder-gap"})


def effective_plan(workload: str) -> Plan:
    return PLANS[workload]


def mode_of(workload: str) -> str:
    return "gap" if PLANS[workload].gap_omit_offset is not None else "normal"


def game_name_of(workload: str) -> str:
    return effective_plan(workload).game_name


def gap_codegen_args(plan: Plan) -> str:
    assert plan.gap_omit_offset is not None
    return f"--omit-aot=0x{plan.base + plan.gap_omit_offset:08x}"


# ---------------------------------------------------------------------------
# ELF / ~PSP assembly
# ---------------------------------------------------------------------------


def assemble_text(plan: Plan) -> tuple[list[int], list[tuple[int, int, int]]]:
    asm = plan.build_asm(plan)
    words = asm.resolve()
    # Label-placement contract: every workload declares fixed offsets for its
    # named functions; drift would silently retarget calls and pointer tables.
    expected_labels = plan.label_contract
    for name, byte_offset in expected_labels.items():
        actual = asm._labels.get(name)
        if actual is None or actual * 4 != byte_offset:
            raise RuntimeError(
                f"{plan.name}: label {name} assembled at "
                f"{(actual or 0) * 4:#x}, contract requires {byte_offset:#x}"
            )
    return words, list(asm.reloc_sites)


def text_bytes_for(plan: Plan) -> tuple[bytes, int, list[tuple[int, int, int]]]:
    words, sites = assemble_text(plan)
    if len(words) * 4 <= plan.entry_offset:
        raise RuntimeError(
            f"{plan.name}: assembled text ends at {len(words) * 4:#x} but the "
            f"entry offset is {plan.entry_offset:#x}"
        )
    if words[plan.entry_offset // 4] == 0:
        raise RuntimeError(
            f"{plan.name}: no instruction at the declared entry offset "
            f"{plan.entry_offset:#x}; the workload would enter on padding"
        )
    code_end = max(plan.entry_offset + 4, len(words) * 4)
    declared_end = max(code_end, plan.text_pad_end)
    stub_count = len(plan.flat_nids())
    stub_end = STUB_AREA_OFFSET + 8 * stub_count if stub_count else 0
    size = max(declared_end, stub_end)
    size = (size + 0xF) & ~0xF
    blob = bytearray(size)
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, 4 * index, word)
    for index in range(stub_count):
        struct.pack_into("<I", blob, STUB_AREA_OFFSET + 8 * index, 0x03E00008)
        struct.pack_into("<I", blob, STUB_AREA_OFFSET + 8 * index + 4, 0)
    return bytes(blob), size, sites


def data_bytes_for(plan: Plan) -> bytes:
    size = max(plan.data_file_size, plan.data_mem_size, DATA_GUEST_BASE + 4)
    blob = bytearray(size)
    module_name = b"platform_ladder"
    struct.pack_into(
        "<HH28s5I",
        blob,
        0,
        0,
        0x0100,
        module_name + b"\0" * (28 - len(module_name)),
        0,
        DATA_LIB_ENT_BASE,                       # ent_top
        DATA_LIB_ENT_BASE + LIB_ENT_ENTRY_SIZE,  # ent_end
        DATA_LIB_STUB_BASE if plan.imports else 0,
        DATA_LIB_STUB_BASE + 20 * len(plan.imports) if plan.imports else 0,
    )
    # One export entry pointing at the guest entry: the generic PSP way a
    # module publishes its start address. The analyzer reconstructs exports
    # from .lib.ent and seeds the address as a high-confidence function start.
    # Fields: name, version, flags, entLen(words), varCount, funcCount,
    # funcTable, varTable.
    struct.pack_into("<I", blob, DATA_LIB_ENT_BASE + 0, DATA_MODULE_INFO_NAME_OFF)
    struct.pack_into("<H", blob, DATA_LIB_ENT_BASE + 4, 0x0001)   # version
    struct.pack_into("<H", blob, DATA_LIB_ENT_BASE + 6, 0x0000)   # flags
    struct.pack_into("<B", blob, DATA_LIB_ENT_BASE + 8, 7)        # entLen words (=28B step)
    struct.pack_into("<B", blob, DATA_LIB_ENT_BASE + 9, 0)        # varCount
    struct.pack_into("<H", blob, DATA_LIB_ENT_BASE + 10, 1)       # funcCount
    struct.pack_into("<I", blob, DATA_LIB_ENT_BASE + 12, DATA_LIB_ENT_FUNC_TABLE)
    struct.pack_into("<I", blob, DATA_LIB_ENT_BASE + 16, DATA_LIB_ENT_FUNC_TABLE + 4)
    struct.pack_into("<I", blob, DATA_LIB_ENT_FUNC_TABLE, plan.entry_offset)
    struct.pack_into("<I", blob, DATA_LIB_ENT_FUNC_TABLE + 4, 0)
    name_cursor = DATA_MODULE_INFO_NAME_OFF
    for lib_index, (library, nids) in enumerate(plan.imports):
        entry = DATA_LIB_STUB_BASE + 20 * lib_index
        first_nid = sum(len(n) for _, n in plan.imports[:lib_index])
        raw = library.encode("ascii") + b"\0"
        blob[name_cursor : name_cursor + len(raw)] = raw
        struct.pack_into(
            "<IHHBBHII",
            blob,
            entry,
            name_cursor,
            0x0101,
            0x0009,
            5,
            0,
            len(nids),
            DATA_NID_BASE + 4 * first_nid,
            STUB_AREA_OFFSET + 8 * first_nid,
        )
        name_cursor += len(raw)
    for index, nid in enumerate(plan.flat_nids()):
        struct.pack_into("<I", blob, DATA_NID_BASE + 4 * index, nid)
    string_offsets = set(plan.data_strings)
    for offset, value in sorted(plan.data_words.items()):
        if offset in string_offsets:
            continue
        struct.pack_into("<I", blob, offset, value)
    for offset, raw in sorted(plan.data_strings.items()):
        blob[offset : offset + len(raw)] = raw
    return bytes(blob)


def plan_relocations(
    plan: Plan, sites: list[tuple[int, int, int]]
) -> list[tuple[int, int]]:
    records: list[tuple[int, int]] = [
        (index * 4, relocation_info(rtype, 0, target_segment))
        for index, rtype, target_segment in sites
    ]
    # Module entry-table pointers (self-relative within the data segment).
    records.append((MODULE_INFO_ENT_TOP_FIELD, relocation_info(R_MIPS_32, 1, 1)))
    records.append((MODULE_INFO_ENT_END_FIELD, relocation_info(R_MIPS_32, 1, 1)))
    records.append((DATA_LIB_ENT_BASE + 0, relocation_info(R_MIPS_32, 1, 1)))   # export name
    records.append((DATA_LIB_ENT_BASE + 12, relocation_info(R_MIPS_32, 1, 1)))  # func table
    records.append((DATA_LIB_ENT_BASE + 16, relocation_info(R_MIPS_32, 1, 1)))  # var table
    # The exported function pointer itself targets the text segment.
    records.append((DATA_LIB_ENT_FUNC_TABLE, relocation_info(R_MIPS_32, 1, 0)))
    for offset in plan.data_text_pointers:
        records.append((offset, relocation_info(R_MIPS_32, 1, 0)))
    if plan.imports:
        records.append((MODULE_INFO_STUB_TOP_FIELD, relocation_info(R_MIPS_32, 1, 1)))
        records.append((MODULE_INFO_STUB_END_FIELD, relocation_info(R_MIPS_32, 1, 1)))
        for lib_index in range(len(plan.imports)):
            entry = DATA_LIB_STUB_BASE + 20 * lib_index
            records.append((entry + 0, relocation_info(R_MIPS_32, 1, 1)))
            records.append((entry + 12, relocation_info(R_MIPS_32, 1, 1)))
            records.append((entry + 16, relocation_info(R_MIPS_32, 1, 0)))
    return records


SECTION_TOKENS = [
    "", ".text", ".sceStub.text", ".rodata.sceModuleInfo",
    ".rodata.libstub.names", ".lib.stub", ".rodata.sceNid", ".lib.ent",
    ".data", ".bss", ".reloc.sceModuleInfo", ".shstrtab",
]


def build_prx(plan: Plan) -> tuple[bytes, bytes]:
    text, text_size, sites = text_bytes_for(plan)
    data = data_bytes_for(plan)
    relocations = plan_relocations(plan, sites)
    relocation_bytes = b"".join(struct.pack("<II", *record) for record in relocations)

    offsets: dict[str, int] = {}
    cursor = 0
    for token in SECTION_TOKENS:
        offsets[token] = cursor
        cursor += len(token.encode("ascii")) + 1
    section_names = b"".join(token.encode("ascii") + b"\0" for token in SECTION_TOKENS)

    shstr_offset = RELOC_FILE_OFFSET + len(relocation_bytes)
    section_table_offset = (shstr_offset + len(section_names) + 3) & ~3

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    program_headers = b"".join(
        [
            struct.pack(
                "<8I", 1, TEXT_FILE_OFFSET, 0, 0,
                text_size, text_size, 5, 0x1000,
            ),
            struct.pack(
                "<8I", 1, DATA_FILE_OFFSET, plan.data_seg_vaddr, plan.data_seg_vaddr,
                len(data), max(plan.data_mem_size, len(data)), 6, 0x1000,
            ),
        ]
    )

    def section(token: str, sec_type: int, flags: int, address: int, offset: int,
                size: int, alignment: int, entry_size: int = 0) -> bytes:
        return struct.pack(
            "<10I",
            offsets[token], sec_type, flags, address, offset, size, 0, 0,
            alignment, entry_size,
        )

    stub_count = len(plan.flat_nids())
    text_section_size = STUB_AREA_OFFSET if stub_count else text_size
    sections: list[bytes] = [struct.pack("<10I", *([0] * 10))]
    sections.append(section(".text", 1, 6, 0, TEXT_FILE_OFFSET, text_section_size, 4))
    if stub_count:
        sections.append(
            section(".sceStub.text", 1, 6, STUB_AREA_OFFSET,
                    TEXT_FILE_OFFSET + STUB_AREA_OFFSET, 8 * stub_count, 4)
        )
    sections.append(
        section(".rodata.sceModuleInfo", 1, 2, plan.data_seg_vaddr,
                DATA_FILE_OFFSET, 52, 4)
    )
    sections.append(
        section(".lib.ent", 1, 2, plan.data_seg_vaddr + DATA_LIB_ENT_BASE,
                DATA_FILE_OFFSET + DATA_LIB_ENT_BASE, LIB_ENT_ENTRY_SIZE, 4)
    )
    if plan.imports:
        sections.append(
            section(".rodata.libstub.names", 1, 2,
                    plan.data_seg_vaddr + DATA_MODULE_INFO_NAME_OFF,
                    DATA_FILE_OFFSET + DATA_MODULE_INFO_NAME_OFF,
                    DATA_LIB_STUB_BASE - DATA_MODULE_INFO_NAME_OFF, 1)
        )
        sections.append(
            section(".lib.stub", 1, 2, plan.data_seg_vaddr + DATA_LIB_STUB_BASE,
                    DATA_FILE_OFFSET + DATA_LIB_STUB_BASE,
                    20 * len(plan.imports), 4)
        )
        sections.append(
            section(".rodata.sceNid", 1, 2, plan.data_seg_vaddr + DATA_NID_BASE,
                    DATA_FILE_OFFSET + DATA_NID_BASE,
                    4 * len(plan.flat_nids()), 4)
        )
    data_section_size = max(len(data) - DATA_GUEST_BASE, 4)
    sections.append(
        section(".data", 1, 3, plan.data_seg_vaddr + DATA_GUEST_BASE,
                DATA_FILE_OFFSET + DATA_GUEST_BASE,
                data_section_size, 4)
    )
    if plan.data_mem_size > plan.data_file_size:
        sections.append(
            section(".bss", 8, 3, plan.data_seg_vaddr + plan.data_file_size,
                    DATA_FILE_OFFSET + plan.data_file_size,
                    plan.data_mem_size - plan.data_file_size, 16)
        )
    sections.append(
        section(".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0,
                RELOC_FILE_OFFSET, len(relocation_bytes), 4, 8)
    )
    sections.append(section(".shstrtab", 3, 0, 0, shstr_offset, len(section_names), 1))

    elf_header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        0xFFA0,
        8,
        1,
        0,
        52,
        section_table_offset,
        0x10,
        52,
        32,
        2,
        40,
        len(sections),
        len(sections) - 1,
    )

    blob = bytearray(section_table_offset + len(sections) * 40)
    blob[0 : len(elf_header)] = elf_header
    blob[52 : 52 + len(program_headers)] = program_headers
    blob[TEXT_FILE_OFFSET : TEXT_FILE_OFFSET + len(text)] = text
    blob[DATA_FILE_OFFSET : DATA_FILE_OFFSET + len(data)] = data
    blob[RELOC_FILE_OFFSET : RELOC_FILE_OFFSET + len(relocation_bytes)] = relocation_bytes
    blob[shstr_offset : shstr_offset + len(section_names)] = section_names
    packed_sections = b"".join(sections)
    blob[section_table_offset : section_table_offset + len(packed_sections)] = packed_sections

    psp_header = b""
    if plan.psp_header:
        header = bytearray(0x80)
        header[:4] = b"~PSP"
        header[0x27] = 2
        struct.pack_into("<I", header, 0x38, max(0, plan.data_mem_size - len(data)))
        struct.pack_into(
            "<4I", header, 0x54,
            text_size, max(plan.data_mem_size, len(data)), 0, 0,
        )
        psp_header = bytes(header)
    return bytes(blob), psp_header


def manifest_bytes(plan: Plan, prx: bytes, psp_header: bytes, workload: str) -> bytes:
    manifest = {
        "schema": 1,
        "kind": "source-owned-psp-platform-ladder",
        "workload": workload,
        "source_plan": plan.name,
        "game_name": game_name_of(workload),
        "mode": mode_of(workload),
        "base": f"0x{plan.base:08x}",
        "entry": f"0x{plan.entry:08x}",
        "result": f"0x{plan.result_addr():08x}",
        "expected": f"0x{plan.expected_value():08x}",
        "imports": [
            {"library": library, "nids": [f"0x{nid:08x}" for nid in nids]}
            for library, nids in plan.imports
        ],
        "psp_header": plan.psp_header,
        "prx_sha256": hashlib.sha256(prx).hexdigest(),
        "psp_header_sha256": hashlib.sha256(psp_header).hexdigest(),
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("ascii")


def write_if_changed(path: Path, data: bytes) -> bool:
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def generate(out_dir: Path, workload: str) -> int:
    plan = effective_plan(workload)
    prx, psp_header = build_prx(plan)
    outputs = {
        out_dir / "guest.prx": prx,
        out_dir / "guest.psp": psp_header,
        out_dir / "manifest.json": manifest_bytes(plan, prx, psp_header, workload),
    }
    changed = [str(path) for path, data in outputs.items() if write_if_changed(path, data)]
    state = "updated" if changed else "unchanged"
    print(
        f"PLATFORM_LADDER_FIXTURE workload={workload} state={state} "
        f"prx_sha256={hashlib.sha256(prx).hexdigest()} "
        f"psp_sha256={hashlib.sha256(psp_header).hexdigest()}"
    )
    return 0


# --- pipeline qualification --------------------------------------------------


def _read_manifest(fixture_dir: Path) -> dict[str, object]:
    return json.loads((fixture_dir / "manifest.json").read_text(encoding="ascii"))


def required_symbols(workload: str) -> tuple[int, ...]:
    plan = effective_plan(workload)
    symbols = [plan.entry]
    if plan.name == "ladder-reloc":
        symbols += [plan.base + L1_B_OFF, plan.base + L1_D_OFF, plan.base + L1_C_OFF]
    if plan.name == "ladder-gap":
        # mid is the omitted interpreter-floor function; entry and end stay AOT.
        symbols += [plan.base + LG_END_OFF]
    if plan.name == "ladder-sched":
        symbols.append(plan.base + L2_WORKER_OFF)
    if plan.name == "ladder-title2":
        symbols += [
            plan.base + TITLE2_CALLBACK_OFF,
            plan.base + TITLE2_CHECKSUM_OFF,
            plan.base + TITLE2_WORKER_OFF,
        ]
    return tuple(symbols)


def verify(build_dir: Path, workload: str) -> int:
    plan = effective_plan(workload)
    stem = game_name_of(workload)
    fixture_dir = build_dir / "fixture"
    manifest = _read_manifest(fixture_dir)
    prx = (fixture_dir / "guest.prx").read_bytes()
    psp_header = (fixture_dir / "guest.psp").read_bytes()
    if manifest != json.loads(manifest_bytes(plan, prx, psp_header, workload)):
        raise RuntimeError("fixture manifest does not match the generated bytes")
    if plan.name == "ladder-gap":
        # Re-decode the words from the final ELF bytes as a second boundary of
        # the proof.  This catches a packer/image mutation even when the
        # in-memory Asm object was correct.
        through = max(
            offset
            for pair in LG_ADDRESS_CONTRACT.values()
            for offset in pair[:2]
        )
        through = max(through, LG_END_LOAD_B_OFFSET) + 4
        image_words = [
            struct.unpack_from("<I", prx, TEXT_FILE_OFFSET + offset)[0]
            for offset in range(0, through, 4)
        ]
        _verify_lg_address_topology(image_words, plan)
    if plan.name == "ladder-title2":
        gap_words = [
            struct.unpack_from(
                "<I", prx, TEXT_FILE_OFFSET + TITLE2_GAP_OFF + 4 * index
            )[0]
            for index in range(len(TITLE2_GAP_WORDS))
        ]
        if gap_words != list(TITLE2_GAP_WORDS):
            raise RuntimeError("title2 interpreter gap bytes drifted")
        if TITLE2_BASE + plan.data_seg_vaddr + TITLE2_MEM_CELL_OFF != TITLE2_MEM_CELL:
            raise RuntimeError("title2 memory-cell address contract drifted")

    image_path = build_dir / f"{stem}_image.bin"
    executable = build_dir / f"{stem}.exe"
    main_c = build_dir / f"{stem}_recomp.c"
    funcs_h = build_dir / f"{stem}_recomp_funcs.h"
    imports_toml = build_dir / f"{stem}_imports.toml"
    missing = [
        str(path)
        for path in (image_path, executable, main_c, funcs_h, imports_toml)
        if not path.is_file()
    ]
    if missing:
        raise RuntimeError("platform-ladder build is missing: " + ", ".join(missing))

    chunk_sources = sorted(build_dir.glob(f"{stem}_recomp_[0-9]*.c"))
    if not chunk_sources:
        raise RuntimeError("no generated chunk sources found")

    generated_text = "\n".join(path.read_text(encoding="ascii") for path in chunk_sources)
    for address in required_symbols(workload):
        if f"f_{address:08x}" not in generated_text:
            raise RuntimeError(f"generated chunks omit function 0x{address:08x}")
    for library, nids in plan.imports:
        if library not in imports_toml.read_text(encoding="ascii"):
            raise RuntimeError(f"generated import manifest omits library {library}")
    for flat_index, nid in enumerate(plan.flat_nids()):
        stub_addr = plan.stub_address(flat_index)
        marker = f"sr_syscall(s, 0x{nid:08x}u)"
        if marker not in generated_text:
            raise RuntimeError(f"import stub {flat_index} does not dispatch NID 0x{nid:08x}")

    if plan.gap_omit_offset is not None:
        omitted_addr = plan.base + plan.gap_omit_offset
        if f"f_{omitted_addr:08x}" in generated_text:
            raise RuntimeError("AOT omission leaked the gap function into generated C")
        # The omitted address must be reached through the ordinary production
        # crossing, not a bespoke fixture hook. Under the current #126/#127
        # contract a linked CALL carries explicit target and resume_pc through
        # dispatch_call (resume_pc is the call's link value; live $ra is not
        # the resume descriptor). ladder-gap also exercises a TAIL crossing;
        # Title-2 intentionally needs only the linked CALL from its primary
        # production flow.
        call_seam = re.search(
            rf"dispatch_call\(s, 0x{omitted_addr:08x}u, 0x[0-9a-f]{{8}}u\)",
            generated_text,
        )
        tail_seam = f"uint32_t _t = 0x{omitted_addr:08x}u; dispatch(s, _t);" in generated_text
        required_crossings = ("CALL",) if plan.name == "ladder-title2" else ("CALL", "TAIL")
        missing = [
            name
            for name, present in (("CALL", call_seam), ("TAIL", tail_seam))
            if name in required_crossings and not present
        ]
        if missing:
            raise RuntimeError(
                "gap generated code lacks the production dispatch seam "
                f"for crossing(s): {', '.join(missing)}"
            )

    image = image_path.read_bytes()
    result_offset = plan.result_addr() - plan.base
    if len(image) <= result_offset:
        raise RuntimeError(f"flat image too small ({len(image)} bytes)")
    initial = struct.unpack_from("<I", image, result_offset)[0]
    if plan.name == "ladder-reloc" and initial != 0:
        raise RuntimeError("reloc workload result slot (.bss) is not zero before execution")

    print(
        f"PLATFORM_LADDER_VERIFY workload={workload} status=PASS "
        f"chunks={len(chunk_sources)} image_bytes={len(image)}"
    )
    return 0


# --- runtime -----------------------------------------------------------------


def validate_title2_negative_output(completed) -> None:
    """Fail closed unless the unsupported import reaches the scheduler fatal path."""
    combined = completed.stdout + completed.stderr
    if completed.returncode != 7:
        raise RuntimeError(
            f"title2 negative expected exit 7, got {completed.returncode}"
        )
    required = (
        "HLE: unimplemented nid 0xdead2f02 (unknown) (thread uid 0x",
        '-> add a handler in src/rt/hle.c: sr_hle_register(0xdead2f02u, "sceUnknown", h_...);',
    )
    missing = [marker for marker in required if marker not in combined]
    if missing:
        raise RuntimeError("title2 negative evidence omits: " + ", ".join(missing))
    forbidden = (
        "DRIVER_EXPECT_U32",
        "PLATFORM_LADDER_RUN workload=ladder-title2-negative status=PASS",
    )
    present = [marker for marker in forbidden if marker in combined]
    if present:
        raise RuntimeError("title2 negative evidence contains: " + ", ".join(present))


def run(build_dir: Path, workload: str, negative: bool = False) -> int:
    plan = effective_plan(workload)
    stem = game_name_of(workload)
    executable = build_dir / f"{stem}.exe"
    image_path = build_dir / f"{stem}_image.bin"

    if workload == "ladder-title2-negative":
        if not negative:
            raise RuntimeError("--negative is required for ladder-title2-negative")
        expected = None
    else:
        expected = plan.expected_value()
        if negative and workload != "ladder-fs":
            raise RuntimeError("--negative applies only to ladder-fs and ladder-title2-negative")
        if negative:
            expected = L5_FAIL_SENTINEL

    fs_root: tempfile.TemporaryDirectory | None = None
    payload: Path | None = None
    empty_dataroot: tempfile.TemporaryDirectory | None = None
    environment = os.environ.copy()
    for key in ("SR_DATAROOT", "SR_FSDIR"):
        environment.pop(key, None)
    # Hostile-configuration default for every ladder run: declare an explicit
    # EMPTY extracted-data root so runtime init can never fall back to an
    # executable-relative retail tree. A generic workload must not depend on,
    # or even name, any title's data layout.
    empty_dataroot = tempfile.TemporaryDirectory(prefix="platform_ladder_empty_root_")
    environment["SR_DATAROOT"] = empty_dataroot.name
    if workload == "ladder-fs":
        fs_root = tempfile.TemporaryDirectory(prefix="platform_ladder_fs_")
        if not negative:
            payload = Path(fs_root.name) / ("ms0__" + L5_PAYLOAD_NAME)
            payload.write_bytes(L5_PAYLOAD_BYTES)
        environment["SR_FSDIR"] = fs_root.name
    if workload == "ladder-title2":
        fs_root = tempfile.TemporaryDirectory(prefix="platform_ladder_title2_")
        payload = Path(fs_root.name) / TITLE2_PATH_BYTES.decode("ascii").rstrip("\0").replace("/", "_").replace(":", "_")
        payload.write_bytes(title2_payload_bytes())
        environment["SR_FSDIR"] = fs_root.name
    for key, value in plan.env.items():
        environment[key] = value

    command = [
        str(executable),
        "--image",
        str(image_path),
        f"0x{plan.base:08x}",
        f"0x{plan.entry:08x}",
        "none",
        "none",
        "--sched",
    ]
    if workload == "ladder-title2-negative":
        pass
    elif workload == "ladder-title2":
        command.extend(
            f"--expect-u32=0x{address:08x}:0x{value:08x}"
            for address, value in title2_result_expectations(plan)
        )
    else:
        command.append(f"--expect-u32=0x{plan.result_addr():08x}:0x{expected:08x}")
    try:
        completed = subprocess.run(
            command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=120
        )
    finally:
        if fs_root is not None:
            fs_root.cleanup()
        if empty_dataroot is not None:
            empty_dataroot.cleanup()
    log_suffix = "" if not negative else ".negative"
    write_if_changed(build_dir / f"{stem}.run{log_suffix}.stdout.log", completed.stdout.encode("utf-8"))
    write_if_changed(build_dir / f"{stem}.run{log_suffix}.stderr.log", completed.stderr.encode("utf-8"))
    combined = completed.stdout + completed.stderr

    if workload == "ladder-title2-negative":
        validate_title2_negative_output(completed)
        print(
            f"PLATFORM_LADDER_NEGATIVE workload={workload} status=PASS exit=7"
        )
        return 0

    markers = (
        "BOOT_EVENT phase=init public_safe=1",
        f"BOOT_EVENT phase=image_loaded entry=0x{plan.entry:08x}",
        "sr_register_all: completed",
    )
    expectation_markers = (
        tuple(
            f"DRIVER_EXPECT_U32 addr=0x{address:08x} got=0x{value:08x} "
            f"expected=0x{value:08x} status=PASS"
            for address, value in title2_result_expectations(plan)
        )
        if workload == "ladder-title2"
        else (
            f"DRIVER_EXPECT_U32 addr=0x{plan.result_addr():08x} got=0x{expected:08x} "
            f"expected=0x{expected:08x} status=PASS",
        )
    )
    missing = [marker for marker in (*markers, *expectation_markers) if marker not in combined]
    if missing:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence omits: " + ", ".join(missing))
    forbidden = (
        "UNKNOWN NID",
        "NONPLT_MISS",
        "INTERP_REJECT",
        "status=FAIL",
        "xbdata",
        "place_game_here",
        "hst_image",
    )
    present = [marker for marker in forbidden if marker in combined]
    if present:
        sys.stderr.write(combined)
        raise RuntimeError("runtime evidence contains: " + ", ".join(present))
    if workload == "ladder-title2":
        print(
            f"PLATFORM_LADDER_RUN workload={workload} status=PASS "
            f"result_words={len(title2_result_expectations(plan))}"
        )
    else:
        print(
            f"PLATFORM_LADDER_RUN workload={workload} status=PASS "
            f"result=0x{plan.result_addr():08x} value=0x{expected:08x}"
        )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--workload", choices=WORKLOAD_CHOICES, required=True)
    generate_parser.add_argument("--out-dir", type=Path, required=True)
    for command in ("verify", "run"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("--workload", choices=WORKLOAD_CHOICES, required=True)
        command_parser.add_argument("--build-dir", type=Path, required=True)
    run_parser = subparsers.choices["run"]
    run_parser.add_argument("--negative", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "generate":
            return generate(args.out_dir, args.workload)
        if args.command == "verify":
            return verify(args.build_dir, args.workload)
        if args.command == "run":
            return run(args.build_dir, args.workload, negative=args.negative)
        raise AssertionError(f"unhandled command {args.command}")
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        sys.stderr.write(f"PLATFORM_LADDER_{args.command.upper()} status=FAIL: {error}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
