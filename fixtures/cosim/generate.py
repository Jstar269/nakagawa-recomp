# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the source-owned guest used by the AOT/interpreter cosimulation gate.

The committed fixture is this recipe, not a binary.  It deterministically emits a
small ELF32 PSP ``~PSP``/PRX pair into the ignored build tree together with a
generated C manifest naming every cell's guest address.  Names, addresses,
instructions and data are project-authored test values; nothing here contains or
derives from a retail title.

WHAT THE GUEST IS FOR
---------------------
Each *cell* is a self-contained guest function that relinquishes control through a
register transfer -- ``jr $ra`` for most, a computed tail call for ``jrtail``.  The
production pipeline translates every cell to native C (``tools/codegen.py``), so
one build produces both execution lanes over *the same guest bytes*:

  lane A  the generated ``f_<addr>`` body, entered through production dispatch()
  lane B  the production interpreter floor (``src/rt/guest_interp.c``) over the
          image bytes, entered through the same dispatch() with the cell simply
          not registered

``fixtures/cosim/cosim_selftest.c`` runs both and compares architectural state.
The cells therefore exist to make one semantic question answerable per cell, not
to be a broad opcode census.

RELOCATION MODEL
----------------
The guest is laid out at segment-relative addresses and rebased by the ordinary
production loader, exactly like ``fixtures/production_smoke``.  Every ``j``/``jal``
word therefore needs an R_MIPS_26 record so the production relocation pass owns
its final encoding; :func:`relocation_records` derives them from the instruction
stream rather than a hand-maintained list.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


ROOT = Path(__file__).resolve().parents[2]

# Deliberately disjoint from fixtures/production_smoke (0x08804000) so a stale
# artifact from one fixture can never satisfy the other's assertions.
BASE = 0x08900000
DATA_SEGMENT_OFFSET = 0x1000
# Guest scratch the cells load/store through. It lives in the .bss extent so the
# fixture never ships initialized data that a cell then merely reads back.
SCRATCH = BASE + DATA_SEGMENT_OFFSET + 0x40
SCRATCH_SIZE = 0x40
# Guest stack for the cells that must preserve $ra across a call. It sits above
# the loaded image inside the flat arena; the harness seeds and compares the
# whole observed window below, so both lanes start from identical bytes.
STACK = BASE + DATA_SEGMENT_OFFSET + 0x100
WINDOW_LO = BASE + DATA_SEGMENT_OFFSET
WINDOW_HI = BASE + DATA_SEGMENT_OFFSET + 0x200

TEXT_FILE_OFFSET = 0x100
DATA_FILE_OFFSET = 0x1200
DATA_FILE_SIZE = 0x40
DATA_MEMORY_SIZE = 0x40 + SCRATCH_SIZE
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE

R_MIPS_26 = 4
SHT_PRX_RELOC = 0x700000A0


# ---------------------------------------------------------------------------
# instruction encoding (project-authored, MIPS32/Allegrex public encoding)
# ---------------------------------------------------------------------------

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


def _fp(fmt: int, ft: int, fs: int, fd: int, function: int) -> int:
    """COP1 (opcode 0x11) word.  ``fmt`` occupies the rs field."""
    return (
        (0x11 << 26)
        | ((fmt & 31) << 21)
        | ((ft & 31) << 16)
        | ((fs & 31) << 11)
        | ((fd & 31) << 6)
        | (function & 63)
    )


# Register numbers used by the cells.
ZERO, AT, V0, V1, A0, A1 = 0, 1, 2, 3, 4, 5
T0, T1, T2, T3, T4, T5, T6, T7 = 8, 9, 10, 11, 12, 13, 14, 15
S0, S1, S2, S3 = 16, 17, 18, 19
T8, T9 = 24, 25
SP, RA = 29, 31

NOP = 0
JR_RA = _r(RA, 0, 0, 0, 0x08)


# ---------------------------------------------------------------------------
# cells
# ---------------------------------------------------------------------------
#
# Every cell is a list of words ending in a `jr` and its delay slot.  Cell bodies
# never name an absolute guest address of their own: the harness supplies the
# scratch pointer in $a0, so the same bytes are position independent apart from
# the relocated j/jal targets and the patched intra-fixture leaf pointers.

def _cell_alu() -> list[int]:
    """Ordinary three-operand ALU, immediate ALU, and all three shift kinds.

    Chosen so signed and unsigned interpretations disagree: $t1 is 0xfffffffd, so
    `slt` yields 1 while `sltu` yields 0, and `sra` differs from `srl`.
    """
    return [
        _i(0x0F, 0, T0, 0x1234),            # lui   t0, 0x1234
        _i(0x0D, T0, T0, 0x5678),           # ori   t0, t0, 0x5678   -> 0x12345678
        _i(0x09, ZERO, T1, -3),             # addiu t1, zero, -3     -> 0xfffffffd
        _r(T0, T1, T2, 0, 0x21),            # addu  t2, t0, t1
        _r(T0, T1, T3, 0, 0x23),            # subu  t3, t0, t1
        _r(T0, T1, T4, 0, 0x24),            # and   t4, t0, t1
        _r(T0, T1, T5, 0, 0x25),            # or    t5, t0, t1
        _r(T0, T1, T6, 0, 0x26),            # xor   t6, t0, t1
        _r(T1, T0, T7, 0, 0x2A),            # slt   t7, t1, t0       -> 1 (signed)
        _r(T1, T0, T8, 0, 0x2B),            # sltu  t8, t1, t0       -> 0 (unsigned)
        _r(0, T0, S0, 5, 0x00),             # sll   s0, t0, 5
        _r(0, T0, S1, 5, 0x02),             # srl   s1, t0, 5
        _r(0, T1, S2, 5, 0x03),             # sra   s2, t1, 5        -> 0xffffffff
        _r(0, T1, S3, 5, 0x02),             # srl   s3, t1, 5        -> 0x07ffffff
        _r(T2, ZERO, V0, 0, 0x21),          # addu  v0, t2, zero
        JR_RA,
        NOP,
    ]


def _cell_r0() -> list[int]:
    """$r0 write suppression across the immediate, register and shift forms.

    Every instruction below names $zero as its destination.  A lane that lets any
    of them land makes the final `addu v0, zero, zero` non-zero.

    The return delay slot is deliberately NOT `nop`.  `nop` encodes as
    `sll $zero, $zero, 0`, which writes $r0 with zero -- in a lane that has lost
    $r0 suppression that final instruction REPAIRS the register file and hides the
    defect.  The mutation campaign found exactly that: the `allow-r0-write` mutant
    survived until this slot stopped ending the cell with an $r0 write.  No guest
    read can expose the damage either (a read of $zero is a constant in both
    lanes), so the architectural state vector is the only observer and the cell
    must leave the damage in place for it to see.
    """
    return [
        _i(0x0F, 0, T0, 0x7FFF),            # lui   t0, 0x7fff
        _i(0x09, ZERO, ZERO, 0x7FFF),       # addiu zero, zero, 0x7fff
        _r(T0, T0, ZERO, 0, 0x21),          # addu  zero, t0, t0
        _r(0, T0, ZERO, 3, 0x00),           # sll   zero, t0, 3
        _i(0x0D, T0, ZERO, 0xFFFF),         # ori   zero, t0, 0xffff
        _r(ZERO, ZERO, V0, 0, 0x21),        # addu  v0, zero, zero   -> must be 0
        _i(0x09, ZERO, V1, 1),              # addiu v1, zero, 1
        _r(ZERO, ZERO, V1, 0, 0x25),        # or    v1, zero, zero   -> must be 0
        JR_RA,
        _i(0x09, ZERO, A1, 0x77),           # addiu a1, zero, 0x77 (slot, not a nop)
    ]


def _cell_ldst() -> list[int]:
    """Word, half and byte memory traffic through the $a0 scratch pointer.

    Sign- vs zero-extending loads are paired over the same stored bytes, and the
    final `lw` observes the half-word lane inside its containing word, so a lane
    that writes the wrong lane or the wrong width diverges on both the register
    file and the ordered write log.
    """
    return [
        _i(0x09, ZERO, T1, 0x1234),         # addiu t1, zero, 0x1234
        _i(0x2B, A0, T1, 0),                # sw    t1, 0(a0)
        _i(0x23, A0, T2, 0),                # lw    t2, 0(a0)
        _i(0x09, ZERO, T3, -1),             # addiu t3, zero, -1
        _i(0x28, A0, T3, 4),                # sb    t3, 4(a0)
        _i(0x20, A0, T4, 4),                # lb    t4, 4(a0)       -> 0xffffffff
        _i(0x24, A0, T5, 4),                # lbu   t5, 4(a0)       -> 0x000000ff
        _i(0x09, ZERO, T6, -2),             # addiu t6, zero, -2
        _i(0x29, A0, T6, 8),                # sh    t6, 8(a0)
        _i(0x21, A0, T7, 8),                # lh    t7, 8(a0)       -> 0xfffffffe
        _i(0x25, A0, T8, 8),                # lhu   t8, 8(a0)       -> 0x0000fffe
        _i(0x23, A0, V0, 8),                # lw    v0, 8(a0)       -> half lane in word
        _i(0x23, A0, V1, 4),                # lw    v1, 4(a0)       -> byte lane in word
        JR_RA,
        NOP,
    ]


def _cell_branch() -> list[int]:
    """Conditional branch ownership of its delay slot.

    Three properties, none of which survives a lane that reorders the slot:
      * a taken `beq` still executes its slot;
      * a not-taken `bne` still executes its slot;
      * the branch condition is read BEFORE the slot runs -- the third branch
        compares two equal registers and its slot then makes them unequal, so a
        lane that evaluates the condition after the slot falls through instead.
    """
    words = [
        _i(0x09, ZERO, T0, 5),              # 0x00 addiu t0, zero, 5
        _i(0x09, ZERO, T1, 5),              # 0x04 addiu t1, zero, 5
        _i(0x09, ZERO, V0, 0),              # 0x08 addiu v0, zero, 0
        _i(0x04, T0, T1, 2),                # 0x0c beq   t0, t1, +2  (-> 0x18)
        _i(0x09, V0, V0, 1),                # 0x10 addiu v0, v0, 1   (slot: always)
        _i(0x09, V0, V0, 0x100),            # 0x14 addiu v0, v0, 256 (skipped)
        _i(0x05, T0, T1, 2),                # 0x18 bne   t0, t1, +2  (not taken)
        _i(0x09, V0, V0, 2),                # 0x1c addiu v0, v0, 2   (slot: always)
        _i(0x09, V0, V0, 4),                # 0x20 addiu v0, v0, 4
        _i(0x04, T0, T1, 2),                # 0x24 beq   t0, t1, +2  (cond read here)
        _i(0x09, ZERO, T1, 9),              # 0x28 addiu t1, zero, 9 (slot mutates t1)
        _i(0x09, V0, V0, 0x200),            # 0x2c addiu v0, v0, 512 (skipped if taken)
        _r(T1, ZERO, V1, 0, 0x21),          # 0x30 addu  v1, t1, zero -> 9
        JR_RA,                              # 0x34
        _i(0x09, V0, V0, 8),                # 0x38 addiu v0, v0, 8   (return slot)
    ]
    return words


def _cell_jump(return_slot_value: int = 16) -> list[int]:
    """Direct `j` with its delay slot, and an architecturally unreachable word.

    The word after the `j` slot can only execute in a lane that mis-computes the
    jump target or fails to give the branch ownership of its slot.
    """
    return [
        _i(0x09, ZERO, V0, 0),              # 0x00 addiu v0, zero, 0
        _j(0x02, 0x10),                     # 0x04 j     +0x10 (relocated)
        _i(0x09, V0, V0, 1),                # 0x08 addiu v0, v0, 1   (slot)
        _i(0x09, V0, V0, 0x400),            # 0x0c addiu v0, v0, 1024 (unreachable)
        _i(0x09, V0, V0, 2),                # 0x10 addiu v0, v0, 2
        JR_RA,                              # 0x14
        _i(0x09, V0, V0, return_slot_value),  # 0x18 return slot
    ]


def _leaf(marker: int, delta: int) -> list[int]:
    """A callee that identifies itself and returns through $ra.

    Every computed transfer in this fixture targets a leaf like this one, because
    generated code models `jal`/`jalr` as a host CALL: its target must be a
    registered function entry that ends in `jr $ra`, or control returns into the
    middle of an already-executing native body.  See fixtures/cosim/README.md.
    """
    return [
        _i(0x09, ZERO, V1, marker),         # addiu v1, zero, <marker>
        _i(0x09, V0, V0, delta),            # addiu v0, v0, <delta>
        JR_RA,
        _r(RA, ZERO, A1, 0, 0x21),          # addu a1, ra, zero (return slot)
    ]


def _cell_link_leaf() -> list[int]:
    """Leaf A -- the architecturally correct destination of every call below."""
    return _leaf(0x00A1, 0x10)


def _cell_link_leaf_b() -> list[int]:
    """Leaf B -- reached only by a lane that resolves a call target too late."""
    return _leaf(0x00B2, 0x1000)


def _cell_link() -> list[int]:
    """`jal` link semantics with a conventional $ra-preserving frame.

    `jal` must write $ra = <call> + 8 BEFORE its delay slot runs, and the callee
    must return exactly there.  The frame also puts a real `sw`/`lw` of $ra on
    the ordered write log, so a lane that loses the save or the restore diverges
    on memory as well as on registers.
    """
    return [
        _i(0x09, SP, SP, -16),              # 0x00 addiu sp, sp, -16
        _i(0x2B, SP, RA, 12),               # 0x04 sw    ra, 12(sp)
        _i(0x09, ZERO, V0, 0),              # 0x08 addiu v0, zero, 0
        _j(0x03, 0x00),                     # 0x0c jal   leafA (patched + relocated)
        _i(0x09, V0, V0, 1),                # 0x10 addiu v0, v0, 1  (slot)
        _r(RA, ZERO, S0, 0, 0x21),          # 0x14 addu  s0, ra, zero (link value)
        _r(V1, ZERO, S1, 0, 0x21),          # 0x18 addu  s1, v1, zero (leaf marker)
        _i(0x23, SP, RA, 12),               # 0x1c lw    ra, 12(sp)
        JR_RA,                              # 0x20
        _i(0x09, SP, SP, 16),               # 0x24 addiu sp, sp, 16 (return slot)
    ]


def _cell_linkr() -> list[int]:
    """`jalr` form: the transfer target comes from a register, not the encoding."""
    return [
        _i(0x09, SP, SP, -16),              # 0x00 addiu sp, sp, -16
        _i(0x2B, SP, RA, 12),               # 0x04 sw    ra, 12(sp)
        _i(0x09, ZERO, V0, 0),              # 0x08 addiu v0, zero, 0
        _i(0x0F, 0, T0, 0),                 # 0x0c lui   t0, %hi(leafA)  (patched)
        _i(0x0D, T0, T0, 0),                # 0x10 ori   t0, t0, %lo(leafA)
        _r(T0, 0, RA, 0, 0x09),             # 0x14 jalr  ra, t0
        _i(0x09, V0, V0, 1),                # 0x18 addiu v0, v0, 1 (slot)
        _r(RA, ZERO, S0, 0, 0x21),          # 0x1c addu  s0, ra, zero (link value)
        _r(V1, ZERO, S1, 0, 0x21),          # 0x20 addu  s1, v1, zero (leaf marker)
        _i(0x23, SP, RA, 12),               # 0x24 lw    ra, 12(sp)
        JR_RA,                              # 0x28
        _i(0x09, SP, SP, 16),               # 0x2c addiu sp, sp, 16 (return slot)
    ]


def _cell_jrslot() -> list[int]:
    """DELAY-SLOT ALIASING -- the `jalr` target register is rewritten by its slot.

    A computed transfer reads its target register AT THE TRANSFER; the delay slot
    runs afterwards and must not change where control goes.  Here the slot copies
    leaf B's address over $t0 after `jalr` has already read it, so:

        architecturally correct  -> leaf A runs  (v1 = 0x00a1, v0 += 0x10)
        target resolved too late -> leaf B runs  (v1 = 0x00b2, v0 += 0x1000)

    The two outcomes are separated on both $v0 and $v1, so the cell cannot pass
    by accident.
    """
    return [
        _i(0x09, SP, SP, -16),              # 0x00 addiu sp, sp, -16
        _i(0x2B, SP, RA, 12),               # 0x04 sw    ra, 12(sp)
        _i(0x09, ZERO, V0, 0),              # 0x08 addiu v0, zero, 0
        _i(0x0F, 0, T0, 0),                 # 0x0c lui   t0, %hi(leafA)  (patched)
        _i(0x0D, T0, T0, 0),                # 0x10 ori   t0, t0, %lo(leafA)
        _i(0x0F, 0, T1, 0),                 # 0x14 lui   t1, %hi(leafB)  (patched)
        _i(0x0D, T1, T1, 0),                # 0x18 ori   t1, t1, %lo(leafB)
        _r(T0, 0, RA, 0, 0x09),             # 0x1c jalr  ra, t0   (target read HERE)
        _r(T1, ZERO, T0, 0, 0x21),          # 0x20 addu  t0, t1, zero (slot rewrites t0)
        _r(V1, ZERO, S0, 0, 0x21),          # 0x24 addu  s0, v1, zero (which leaf ran)
        _r(T0, ZERO, S1, 0, 0x21),          # 0x28 addu  s1, t0, zero (slot did land)
        _i(0x23, SP, RA, 12),               # 0x2c lw    ra, 12(sp)
        JR_RA,                              # 0x30
        _i(0x09, SP, SP, 16),               # 0x34 addiu sp, sp, 16 (return slot)
    ]


def _cell_jrtail() -> list[int]:
    """Computed TAIL CALL: `jr $rs` to a registered entry, aliased by its slot.

    `jr $rs` with rs != $ra is a distinct emission path from `jalr` -- generated
    code turns it into a dispatch followed by the owning entry's host return, so
    the callee's own `jr $ra` lands on the CALLER's $ra and both lanes leave
    through the same synchronization point.

    The delay slot rewrites $t0 after the transfer has already read it, exactly as
    in `jrslot`, so this cell separates the two emission paths rather than
    duplicating one of them.  This cell has no `jr $ra` of its own: the tail call
    IS its exit.
    """
    return [
        _i(0x09, ZERO, V0, 0),              # 0x00 addiu v0, zero, 0
        _i(0x0F, 0, T0, 0),                 # 0x04 lui   t0, %hi(leafA)  (patched)
        _i(0x0D, T0, T0, 0),                # 0x08 ori   t0, t0, %lo(leafA)
        _i(0x0F, 0, T1, 0),                 # 0x0c lui   t1, %hi(leafB)  (patched)
        _i(0x0D, T1, T1, 0),                # 0x10 ori   t1, t1, %lo(leafB)
        _r(T0, 0, 0, 0, 0x08),              # 0x14 jr    t0   (target read HERE)
        _r(T1, ZERO, T0, 0, 0x21),          # 0x18 addu  t0, t1, zero (slot rewrites t0)
    ]


def _cell_hilo() -> list[int]:
    """HI/LO through signed and unsigned multiply.

    The operands make the two products differ in the HIGH word only, so a lane
    that confuses `mult` with `multu` keeps LO identical and is caught only by HI.
    """
    return [
        _i(0x0F, 0, T0, 0x0001),            # lui   t0, 1        -> 0x00010000
        _i(0x09, ZERO, T1, 0x1234),         # addiu t1, zero, 0x1234
        _r(T0, T1, 0, 0, 0x19),             # multu t0, t1
        _r(0, 0, T2, 0, 0x10),              # mfhi  t2
        _r(0, 0, T3, 0, 0x12),              # mflo  t3
        _i(0x09, ZERO, T4, -3),             # addiu t4, zero, -3
        _r(T4, T1, 0, 0, 0x19),             # multu t4, t1       (unsigned)
        _r(0, 0, T5, 0, 0x10),              # mfhi  t5
        _r(0, 0, T6, 0, 0x12),              # mflo  t6
        _r(T4, T1, 0, 0, 0x18),             # mult  t4, t1       (signed, same bits)
        _r(0, 0, V0, 0, 0x10),              # mfhi  v0           -> differs from t5
        _r(0, 0, V1, 0, 0x12),              # mflo  v1           -> equals t6
        JR_RA,
        NOP,
    ]


def _cell_fpu() -> list[int]:
    """Scalar FPU cell over the #120 helper path.

    Both lanes call the same ``sr_fpu_*`` helpers, so this cell compares operand
    selection, register-file indexing and FCR31 threading -- NOT the arithmetic
    kernel, which ``src/rt/fp_convert_selftest.c`` owns.  2.5 and -2.5 are chosen
    because `cvt.w.s` resolves them differently in all four FCR31 rounding modes.
    """
    return [
        _i(0x0F, 0, T0, 0x4020),            # 0x00 lui t0, 0x4020   ->  2.5f
        _fp(0x04, T0, 0, 0, 0x00),          # 0x04 mtc1 t0, f0
        _i(0x0F, 0, T1, 0xC020),            # 0x08 lui t1, 0xc020   -> -2.5f
        _fp(0x04, T1, 1, 0, 0x00),          # 0x0c mtc1 t1, f1
        _fp(0x10, 1, 0, 2, 0x00),           # 0x10 add.s f2, f0, f1 ->  0.0
        _fp(0x10, 0, 0, 3, 0x02),           # 0x14 mul.s f3, f0, f0 ->  6.25
        _fp(0x10, 0, 0, 4, 0x24),           # 0x18 cvt.w.s f4, f0   (FCR31 RM)
        _fp(0x10, 0, 1, 5, 0x24),           # 0x1c cvt.w.s f5, f1   (FCR31 RM)
        _fp(0x00, V0, 4, 0, 0x00),          # 0x20 mfc1 v0, f4
        _fp(0x00, V1, 5, 0, 0x00),          # 0x24 mfc1 v1, f5
        _i(0x39, A0, 3, 0x10),              # 0x28 swc1 f3, 0x10(a0)
        _i(0x31, A0, 6, 0x10),              # 0x2c lwc1 f6, 0x10(a0)
        _fp(0x00, T2, 6, 0, 0x00),          # 0x30 mfc1 t2, f6
        JR_RA,                              # 0x34
        NOP,
    ]


def _cell_spleak() -> list[int]:
    """POSITIVE CONTROL -- deliberately leaves $sp unbalanced at `jr $ra`.

    This is NOT a defect fixture: it pins the one architectural asymmetry the
    lanes genuinely have.  Generated code closes every callable entry with
    ``s->r[29] = _sp_entry`` on an o32 callee-saved-SP assumption, while the
    interpreter executes only the instructions present.  The comparator must
    REPORT this divergence on $r29 and on nothing else; a comparator that hides
    it (or that reports extra fields) fails its own expectations.
    """
    return [
        _i(0x09, SP, SP, -32),              # addiu sp, sp, -32
        _i(0x09, ZERO, V0, 0x55),           # addiu v0, zero, 0x55
        JR_RA,                              # jr ra  (epilogue deliberately absent)
        NOP,
    ]


def _cell_ret() -> list[int]:
    """The cosim return trampoline.

    Guest-visible content is a bare `jr $ra`.  The harness registers an inert
    observer here in both lanes so the interpreter has a REGISTERED AOT
    destination to hand off to; the observer touches no guest state.
    """
    return [JR_RA, NOP]


# Ordered cell table.  ``kind`` drives the harness's seeding and expectations and
# is emitted into the generated manifest so C and Python cannot drift.
CELLS: tuple[tuple[str, str, object], ...] = (
    ("alu", "ordinary ALU", _cell_alu),
    ("r0", "r0 write suppression", _cell_r0),
    ("ldst", "load/store width and extension", _cell_ldst),
    ("branch", "conditional branch + delay slot", _cell_branch),
    ("jump", "direct jump + delay slot", _cell_jump),
    ("link_leaf", "leaf A: the correct call destination", _cell_link_leaf),
    ("link_leaf_b", "leaf B: a late-resolved call destination", _cell_link_leaf_b),
    ("link", "jal link semantics", _cell_link),
    ("linkr", "jalr link semantics", _cell_linkr),
    ("jrslot", "computed-transfer target vs its delay slot", _cell_jrslot),
    ("jrtail", "computed tail call vs its delay slot", _cell_jrtail),
    ("hilo", "HI/LO multiply", _cell_hilo),
    ("fpu", "scalar FPU over the #120 helper path", _cell_fpu),
    ("spleak", "positive control: unbalanced $sp epilogue", _cell_spleak),
    ("ret", "cosim return trampoline", _cell_ret),
)

# Cells the harness never enters directly, so no comparison case is built for
# them.  The leaves still run -- reached through the calls under test.
NON_ENTRY_CELLS = frozenset({"link_leaf", "link_leaf_b", "ret"})

# The trampoline is deliberately NOT called from the fixture entry: nothing in
# the guest transfers to it, so the analyzer never claims it and the harness
# owns that address outright in both lanes.
UNDISCOVERED_CELLS = frozenset({"ret"})


def cell_layout() -> dict[str, tuple[int, list[int]]]:
    """Assign each cell a segment-relative offset, entry function first."""
    layout: dict[str, tuple[int, list[int]]] = {}
    offset = 0
    # The entry function is sized from the call count, so reserve its extent
    # before placing cells: two words per call plus the closing jr/nop pair.
    entry_calls = [name for name, _desc, _fn in CELLS if name not in UNDISCOVERED_CELLS]
    offset += (len(entry_calls) * 2 + 2) * 4
    for name, _desc, builder in CELLS:
        words = builder()
        layout[name] = (offset, words)
        offset += len(words) * 4
    return layout


def entry_words(layout: dict[str, tuple[int, list[int]]]) -> list[int]:
    """Region A: a `jal` per discoverable cell so the analyzer claims each as a
    high-confidence function start rather than relying on a decode heuristic."""
    words: list[int] = []
    for name, _desc, _fn in CELLS:
        if name in UNDISCOVERED_CELLS:
            continue
        words.append(_j(0x03, layout[name][0]))
        words.append(NOP)
    words.append(JR_RA)
    words.append(NOP)
    return words


def build_text_segment() -> bytes:
    layout = cell_layout()
    end = max(offset + len(words) * 4 for offset, words in layout.values())
    text = bytearray((end + 15) & ~15)
    head = entry_words(layout)
    text[0 : len(head) * 4] = struct.pack(f"<{len(head)}I", *head)
    for _name, (offset, words) in layout.items():
        text[offset : offset + len(words) * 4] = struct.pack(f"<{len(words)}I", *words)

    # Patch every intra-fixture reference that names another cell's address.
    leaf_a = BASE + layout["link_leaf"][0]
    leaf_b = BASE + layout["link_leaf_b"][0]
    for cell, slot, address in (
        ("linkr", 0x0C, leaf_a),
        ("jrslot", 0x0C, leaf_a),
        ("jrslot", 0x14, leaf_b),
        ("jrtail", 0x04, leaf_a),
        ("jrtail", 0x0C, leaf_b),
    ):
        base = layout[cell][0] + slot
        register = T0 if address == leaf_a else T1
        struct.pack_into("<I", text, base, _i(0x0F, 0, register, _hi16_for_ori(address)))
        struct.pack_into(
            "<I", text, base + 4, _i(0x0D, register, register, address & 0xFFFF)
        )
    # The `jal` inside the link cell and the `j` inside the jump cell carry
    # segment-relative targets that the production relocation pass rebases.
    struct.pack_into(
        "<I", text, layout["link"][0] + 0x0C, _j(0x03, layout["link_leaf"][0])
    )
    struct.pack_into(
        "<I", text, layout["jump"][0] + 0x04, _j(0x02, layout["jump"][0] + 0x10)
    )
    return bytes(text)


def _hi16_for_ori(value: int) -> int:
    """Upper half for a `lui`/`ori` pair.

    `ori` zero-extends its immediate, so this is a plain shift -- the %hi carry
    adjustment exists only for the sign-extended `addiu`/`lw` pairing and would
    be wrong here.
    """
    return (value >> 16) & 0xFFFF


def relocation_records() -> list[tuple[int, int]]:
    """Derive the R_MIPS_26 table from the instruction stream.

    Every `j`/`jal` word in .text gets a record; nothing is hand-maintained, so a
    new cell cannot silently ship an unrelocated transfer.  The lui/ori pointer
    pair inside the `jalr` cell is materialized by this generator at absolute
    values and needs no record: it names a guest address, not a jump encoding.
    """
    text = build_text_segment()
    records: list[tuple[int, int]] = []
    for offset in range(0, len(text), 4):
        word = struct.unpack_from("<I", text, offset)[0]
        if (word >> 26) in (2, 3):
            records.append((offset, R_MIPS_26 | (0 << 8) | (0 << 16)))
    return records


def build_data_segment() -> bytes:
    """Module metadata ahead of the scratch BSS extent.

    The cells never read this; it exists so the fixture is a real PSP module --
    a second PT_LOAD with a distinct BSS tail and a module-info record the
    ordinary loader and import extractor accept. The export and import tables are
    empty by design: this guest calls no imports, and an empty table is the honest
    way to say so rather than carrying an unused stub.
    """
    data = bytearray(DATA_FILE_SIZE)
    module_name = b"cosim"
    data[0:52] = struct.pack(
        "<HH28s5I",
        0,                                                   # attributes
        0x0100,                                              # version
        module_name + b"\0" * (28 - len(module_name)),
        0,                                                   # gp: unused
        0, 0,                                                # exports: empty
        0, 0,                                                # imports: empty
    )
    struct.pack_into("<I", data, 0x34, 0x5A5A5A5A)           # padding sentinel
    return bytes(data)


def build_prx() -> bytes:
    text = build_text_segment()
    data = build_data_segment()
    relocation_bytes = b"".join(
        struct.pack("<II", *record) for record in relocation_records()
    )
    relocation_offset = DATA_FILE_OFFSET + DATA_FILE_SIZE

    section_names = (
        b"\0.text\0.rodata.sceModuleInfo\0.data\0.reloc.sceModuleInfo\0.bss\0.shstrtab\0"
    )
    names = {
        name: section_names.index(name.encode("ascii"))
        for name in (
            ".text", ".rodata.sceModuleInfo", ".data",
            ".reloc.sceModuleInfo", ".bss", ".shstrtab",
        )
    }
    shstr_offset = relocation_offset + len(relocation_bytes)
    section_table_offset = (shstr_offset + len(section_names) + 3) & ~3
    section_count = 7

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
                len(text), len(text), 5, 0x1000,
            ),
            struct.pack(
                "<8I", 1, DATA_FILE_OFFSET, DATA_SEGMENT_OFFSET, DATA_SEGMENT_OFFSET,
                DATA_FILE_SIZE, DATA_MEMORY_SIZE, 6, 0x1000,
            ),
        ]
    )

    def section(name, section_type, flags, address, offset, size, alignment, entry_size=0):
        return struct.pack(
            "<10I", names[name], section_type, flags, address, offset, size,
            0, 0, alignment, entry_size,
        )

    sections = [struct.pack("<10I", *([0] * 10))]
    sections.extend(
        [
            section(".text", 1, 6, 0, TEXT_FILE_OFFSET, len(text), 4),
            section(
                ".rodata.sceModuleInfo", 1, 2,
                DATA_SEGMENT_OFFSET, DATA_FILE_OFFSET, 52, 4,
            ),
            section(
                ".data", 1, 3, DATA_SEGMENT_OFFSET + 0x34,
                DATA_FILE_OFFSET + 0x34, DATA_FILE_SIZE - 0x34, 4,
            ),
            section(
                ".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0,
                relocation_offset, len(relocation_bytes), 4, 8,
            ),
            section(
                ".bss", 8, 3, DATA_SEGMENT_OFFSET + DATA_FILE_SIZE,
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
    blob[relocation_offset : relocation_offset + len(relocation_bytes)] = relocation_bytes
    blob[shstr_offset : shstr_offset + len(section_names)] = section_names
    joined = b"".join(sections)
    blob[section_table_offset : section_table_offset + len(joined)] = joined
    return bytes(blob)


def build_psp_header() -> bytes:
    header = bytearray(0x80)
    header[:4] = b"~PSP"
    header[0x27] = 2
    struct.pack_into("<I", header, 0x38, BSS_SIZE)
    struct.pack_into(
        "<4I", header, 0x54, len(build_text_segment()), DATA_MEMORY_SIZE, 0, 0
    )
    return bytes(header)


# ---------------------------------------------------------------------------
# generated manifest
# ---------------------------------------------------------------------------

def manifest_header() -> bytes:
    """Emit the C manifest the harness includes.

    Guest addresses live in exactly one place -- this generator -- so a cell that
    moves cannot leave a stale literal behind in the harness.
    """
    layout = cell_layout()
    text = build_text_segment()
    lines = [
        "/* Generated by fixtures/cosim/generate.py. Do not edit by hand. */",
        "#ifndef NAKAGAWA_COSIM_CELLS_H",
        "#define NAKAGAWA_COSIM_CELLS_H",
        "",
        f"#define COSIM_BASE          0x{BASE:08x}u",
        f"#define COSIM_TEXT_LO       0x{BASE:08x}u",
        f"#define COSIM_TEXT_HI       0x{BASE + len(text):08x}u",
        f"#define COSIM_SCRATCH       0x{SCRATCH:08x}u",
        f"#define COSIM_SCRATCH_SIZE  0x{SCRATCH_SIZE:x}u",
        f"#define COSIM_STACK         0x{STACK:08x}u",
        f"#define COSIM_WINDOW_LO     0x{WINDOW_LO:08x}u",
        f"#define COSIM_WINDOW_HI     0x{WINDOW_HI:08x}u",
        f"#define COSIM_ENTRY         0x{BASE:08x}u",
        f"#define COSIM_RETURN        0x{BASE + layout['ret'][0]:08x}u",
        f"#define COSIM_LEAF_A        0x{BASE + layout['link_leaf'][0]:08x}u",
        f"#define COSIM_LEAF_B        0x{BASE + layout['link_leaf_b'][0]:08x}u",
        "",
        "/* X(name, guest_address, word_count, description) -- entry cells only. */",
        "#define COSIM_CELL_LIST(X) \\",
    ]
    entries = [
        (name, desc, layout[name]) for name, desc, _fn in CELLS
        if name not in NON_ENTRY_CELLS
    ]
    for index, (name, desc, (offset, words)) in enumerate(entries):
        terminator = "" if index == len(entries) - 1 else " \\"
        lines.append(
            f'    X({name}, 0x{BASE + offset:08x}u, {len(words)}u, "{desc}"){terminator}'
        )
    lines.extend(["", "#endif", ""])
    return ("\n".join(lines)).encode("ascii")


def manifest_bytes() -> bytes:
    layout = cell_layout()
    prx = build_prx()
    manifest = {
        "schema": 1,
        "kind": "source-owned-aot-interpreter-cosim",
        "base": BASE,
        "scratch": SCRATCH,
        "scratch_size": SCRATCH_SIZE,
        "stack": STACK,
        "window": [WINDOW_LO, WINDOW_HI],
        "prx_sha256": sha256(prx),
        "psp_sha256": sha256(build_psp_header()),
        "relocations": [
            {"offset": offset, "info": info} for offset, info in relocation_records()
        ],
        "cells": [
            {
                "name": name,
                "address": BASE + layout[name][0],
                "words": len(layout[name][1]),
                "description": desc,
                "entry": name not in NON_ENTRY_CELLS,
            }
            for name, desc, _fn in CELLS
        ],
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("ascii")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path: Path, data: bytes) -> bool:
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def generate(out_dir: Path) -> int:
    prx = build_prx()
    psp_header = build_psp_header()
    outputs = {
        out_dir / "guest.prx": prx,
        out_dir / "guest.psp": psp_header,
        out_dir / "manifest.json": manifest_bytes(),
        out_dir / "cosim_cells.h": manifest_header(),
    }
    changed = [str(path) for path, data in outputs.items() if write_if_changed(path, data)]
    # The harness writes one canonical instruction trace per lane per case; create
    # the directory here so no Make recipe needs a portable mkdir.
    (out_dir / "traces").mkdir(parents=True, exist_ok=True)
    print(
        f"COSIM_FIXTURE state={'updated' if changed else 'unchanged'} "
        f"prx_sha256={sha256(prx)} cells={len(CELLS)}"
    )
    return 0


def verify(build_dir: Path) -> int:
    """Qualify the RELOCATED guest image the production loader actually produced.

    Two independent claims, both of which a broken pipeline would fail:
      * every cell's non-transfer words survive relocation byte for byte, so the
        comparison runs on the instructions this recipe declares;
      * every `j`/`jal` word decodes to a transfer inside the fixture's own text
        extent, so no relocation left a segment-relative target behind.
    """
    image_path = build_dir / "cosim_image.bin"
    if not image_path.is_file():
        print(f"COSIM_VERIFY FAIL: missing relocated image {image_path}", file=sys.stderr)
        return 1
    image = image_path.read_bytes()
    text = build_text_segment()
    layout = cell_layout()
    end = len(text)

    problems: list[str] = []
    for offset in range(0, end, 4):
        actual = struct.unpack_from("<I", image, offset)[0]
        expected = struct.unpack_from("<I", text, offset)[0]
        opcode = actual >> 26
        if opcode in (2, 3):
            target = ((actual & 0x03FFFFFF) << 2) | (BASE & 0xF0000000)
            if not (BASE <= target < BASE + end):
                problems.append(
                    f"transfer at 0x{BASE + offset:08x} targets 0x{target:08x}, "
                    "outside the fixture text extent"
                )
            continue
        if (expected >> 26) in (2, 3):
            problems.append(
                f"word at 0x{BASE + offset:08x} was a transfer in the recipe but "
                f"decodes as opcode 0x{opcode:02x} in the relocated image"
            )
            continue
        if actual != expected:
            problems.append(
                f"word at 0x{BASE + offset:08x} is 0x{actual:08x}, recipe declares "
                f"0x{expected:08x}"
            )

    leaf = BASE + layout["link_leaf"][0]
    link_jal = struct.unpack_from("<I", image, layout["link"][0] + 0x0C)[0]
    link_target = ((link_jal & 0x03FFFFFF) << 2) | (BASE & 0xF0000000)
    if (link_jal >> 26) != 3 or link_target != leaf:
        problems.append(
            f"link cell jal decodes to 0x{link_target:08x}, expected leaf 0x{leaf:08x}"
        )

    # The aliasing cell is only meaningful while its two leaf pointers actually
    # differ; a generator change that collapsed them would make the cell pass
    # vacuously in both lanes.
    leaf_b = BASE + layout["link_leaf_b"][0]
    if leaf_b == leaf:
        problems.append("leaf A and leaf B resolve to the same address")
    for slot, expected in ((0x0C, leaf), (0x14, leaf_b)):
        base = layout["jrslot"][0] + slot
        hi = struct.unpack_from("<I", image, base)[0] & 0xFFFF
        lo = struct.unpack_from("<I", image, base + 4)[0] & 0xFFFF
        if ((hi << 16) | lo) != expected:
            problems.append(
                f"jrslot pointer pair at +0x{slot:02x} materializes "
                f"0x{(hi << 16) | lo:08x}, expected 0x{expected:08x}"
            )

    if problems:
        for problem in problems:
            print(f"COSIM_VERIFY FAIL: {problem}", file=sys.stderr)
        return 1
    print(
        f"COSIM_VERIFY OK cells={len(CELLS)} text_bytes={end} "
        f"relocations={len(relocation_records())}"
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("generate", "verify"))
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--build-dir", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    if args.command == "generate":
        if args.out_dir is None:
            print("generate requires --out-dir", file=sys.stderr)
            return 2
        return generate(args.out_dir)
    if args.build_dir is None:
        print("verify requires --build-dir", file=sys.stderr)
        return 2
    return verify(args.build_dir)


if __name__ == "__main__":
    raise SystemExit(main())
