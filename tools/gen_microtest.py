# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Generate a CRT-free Allegrex test module that exercises the integer, single-precision
# FPU, and Allegrex-extension ISAs with edge-case and seeded-random operands. Each test
# sets up its operands inline (no memory, no HLE) and executes one instruction; the result
# lands in a register and is captured by the reference-interpreter trace. The reference
# interpreter must reproduce every instruction exactly (tools/microtest_gate.py).
# Deterministic: a fixed seed.
#
# ISA split (see ci.yml): the generic MIPS III / single-precision FPU instructions belong
# to the `integer` and `fpu` groups and compile under `-march=r4000`.  The Allegrex-only
# extensions (max/min, madd-family, clz/clo, ext/ins, wsbh/wsbw/seb/seh/bitrev) belong to the
# `allegrex` group.  Every Allegrex-specific instruction-under-test is emitted as an explicit
# raw PSP Allegrex .word so that the ELF cannot silently contain a generic MIPS32 encoding.
# The surrounding setup (lui/ori/mult/mfhi/mflo/calls) may assemble under -march=r4000 or
# any conservative base MIPS mode; the Allegrex-specific words do not depend on the assembler.
#
# Usage: gen_microtest.py <out.c> [random_per_op] [--groups G] [--opcodes O]

import random
import sys

# Edge-case 32-bit operands that tend to expose sign/overflow/boundary bugs.
EDGE = [0x00000000, 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x00000002,
        0x55555555, 0xAAAAAAAA, 0x0000FFFF, 0xFFFF0000, 0x00000100, 0x000000FF]

# Edge-case single-precision bit patterns: 0, -0, 1, -1, 2, 0.5, big, small-normal,
# denormal, +inf, -inf, qNaN, max-finite.
FEDGE = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x3F000000,
         0x49742400, 0x00800000, 0x00000001, 0x7F800000, 0xFF800000, 0x7FC00000,
         0x7F7FFFFF, 0xC0490FDB, 0x40490FDB]


def li(reg, val):
    # Load a 32-bit immediate with lui+ori (both traced, both compared).
    return [f"lui ${reg}, 0x{(val >> 16) & 0xFFFF:04x}",
            f"ori ${reg}, ${reg}, 0x{val & 0xFFFF:04x}"]


def operands32(extra):
    vals = list(EDGE)
    for _ in range(extra):
        vals.append(random.randint(0, 0xFFFFFFFF))
    return vals


def fpairs(extra):
    vals = list(FEDGE)
    for _ in range(extra):
        vals.append(random.randint(0, 0xFFFFFFFF))
    return vals


def gen(extra, groups=None, opcodes=None):
    lines = []

    def emit(*asm):
        lines.extend(asm)

    # Encoding helpers shared between integer and allegrex groups.
    def _special(rs, rt, rd, sa, funct):
        return (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | (sa & 0x1F) << 6 | (funct & 0x3F)

    if opcodes:
        for op in opcodes:
            op = op.strip()
            if not op:
                continue
            if op.lower().startswith("0x"):
                val = int(op, 16)
            else:
                val = int(op, 10)
            emit(f".word 0x{val:08x}")
        return lines

    if not groups:
        groups = ["integer", "fpu", "vfpu"]

    groups = [g.lower() for g in groups]

    if "integer" in groups:
        # Generic MIPS III R-type: dest = f(t0, t1).  These assemble under -march=r4000.
        # Allegrex-only extensions (max/min, madd-family, clz/clo, ext/ins, wsbh/wsbw/seb/seh/bitrev)
        # live in the separate `allegrex` group because no generic MIPS toolchain emits
        # them for r4000 (see the ISA-note at the top of this file).
        rtype = ["addu", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                 "sllv", "srlv", "srav"]
        ops = operands32(extra)
        for op in rtype:
            for a in ops:
                for b in EDGE:  # second operand from the edge set keeps the module bounded
                    emit(*li("t0", a), *li("t1", b), f"{op} $t2, $t0, $t1")
 
        # Shift-immediate: dest = f(t0, sa) for several shift amounts.
        for op in ["sll", "srl", "sra"]:
            for a in ops:
                for sa in (0, 1, 7, 15, 16, 31):
                    emit(*li("t0", a), f"{op} $t2, $t0, {sa}")
 
        # Multiply/divide: results in hi/lo (captured directly). Includes divide-by-zero and the
        # INT_MIN / -1 overflow case so the reference interpreter's edge handling is checked.
        # DIV/DIVU are emitted as raw words to avoid assembler-inserted divide-by-zero guards.
        for op in ["mult", "multu"]:
            for a in EDGE:
                for b in EDGE:
                    emit(*li("t0", a), *li("t1", b), f"{op} $t0, $t1",
                         "mfhi $t2", "mflo $t3")
        for a in EDGE:
            for b in EDGE:
                emit(*li("t0", a), *li("t1", b),
                     f".word 0x{_special(8, 9, 0, 0, 0x1A):08X}",  # div $t0, $t1
                     "mfhi $t2", "mflo $t3")
        for a in EDGE:
            for b in EDGE:
                emit(*li("t0", a), *li("t1", b),
                     f".word 0x{_special(8, 9, 0, 0, 0x1B):08X}",  # divu $t0, $t1
                     "mfhi $t2", "mflo $t3")
 
        # Immediate ALU.
        for a in ops:
            for imm in (0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x1234):
                emit(*li("t0", a),
                     f"addiu $t2, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                     f"slti  $t3, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                     f"sltiu $t4, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                     f"andi  $t5, $t0, 0x{imm:04x}",
                     f"ori   $t6, $t0, 0x{imm:04x}",
                     f"xori  $t7, $t0, 0x{imm:04x}")
 
    if "allegrex" in groups:
        # Allegrex-only integer extensions.  Every instruction-under-test is emitted
        # as an explicit raw PSP Allegrex .word so that the ELF cannot silently
        # contain a generic MIPS32 encoding.  The surrounding setup (lui/ori/mult/mfhi/
        # mflo/calls) may assemble under -march=r4000 or any conservative base MIPS
        # mode; the Allegrex-specific words do not depend on the assembler.
        #
        # Encoding helpers (R-type format: opcode[31:26] rs[25:21] rt[20:16] rd[15:11]
        # sa[10:6] funct[5:0]):
        #   SPECIAL     (opcode 0x00): clz, clo, madd-family, max, min
        #   SPECIAL3    (opcode 0x1F): ext, ins, wsbh, wsbw, seb, seh, bitrev
        def _special3(rs, rt, rd, sa, funct):
            return 0x1F << 26 | (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | (sa & 0x1F) << 6 | (funct & 0x3F)

        def _ext_word(rs, rt, pos, size):
            return _special3(rs, rt, size - 1, pos, 0x00)

        def _ins_word(rs, rt, pos, size):
            return _special3(rs, rt, pos + size - 1, pos, 0x04)

        def _wsbh(rt, rd):
            return _special3(0, rt, rd, 0x02, 0x20)

        def _wsbw(rt, rd):
            return _special3(0, rt, rd, 0x03, 0x20)

        def _seb(rt, rd):
            return _special3(0, rt, rd, 0x10, 0x20)

        def _seh(rt, rd):
            return _special3(0, rt, rd, 0x18, 0x20)

        def _bitrev(rt, rd):
            return _special3(0, rt, rd, 0x14, 0x20)

        ops = operands32(extra)

        # Accumulator seed pairs (HI, LO)
        accum_seeds = [
            (0, 0),
            (0, 0xFFFFFFFF),          # Carry/wrap boundary for addition
            (0x00000001, 0x00000000), # Borrow boundary for subtraction
            (0xFFFFFFFF, 0xFFFFFFFF), # Modulo 2^64 wrap boundary
            (0x12345678, 0x9ABCDEF0), # Nonzero starting accumulator
            (0x80000000, 0x00000000), # High bit set in HI
        ]

        # Operands designed to test signed/unsigned and edge-case behaviors
        madd_ops = [
            (0, 0),
            (1, 1),
            (0xFFFFFFFF, 1),           # -1 * 1
            (0xFFFFFFFF, 0xFFFFFFFF), # -1 * -1 signed, or large unsigned
            (0x80000000, 1),           # INT32_MIN * 1
            (0x7FFFFFFF, 2),           # INT32_MAX * 2
            (0x80000000, 0x80000000), # large unsigned/signed products
            (0x1234, 0x5678),          # typical positive product
        ]

        # Multiply-accumulate family: results in hi/lo.
        # Seed HI and LO independently using mthi and mtlo.
        for op in ["madd", "maddu", "msub", "msubu"]:
            _funct = {"madd": 0x1C, "maddu": 0x1D, "msub": 0x2E, "msubu": 0x2F}[op]
            for val_hi, val_lo in accum_seeds:
                for a, b in madd_ops:
                    emit(*li("t0", val_hi), "mthi $t0",
                         *li("t1", val_lo), "mtlo $t1",
                         *li("t2", a), *li("t3", b),
                         f".word 0x{_special(10, 11, 0, 0, _funct):08X}",  # op $t2, $t3
                         "mfhi $t4", "mflo $t5")

        # clz/clo.
        for op in ["clz", "clo"]:
            _funct = {"clz": 0x16, "clo": 0x17}[op]
            for a in ops:
                emit(*li("t0", a), f".word 0x{_special(8, 0, 10, 0, _funct):08X}")  # op $t2, $t0

        # ext/ins with varied position and size.
        for a in ops:
            for pos, size in ((0, 8), (4, 8), (8, 16), (0, 32), (16, 16), (3, 5), (12, 8), (20, 4)):
                emit(*li("t0", a), f".word 0x{_ext_word(8, 9, pos, size):08X}")  # ext $t2, $t0, pos, size
        for a in EDGE:
            for b in EDGE:
                for pos, size in ((0, 8), (8, 8), (4, 12), (0, 1), (8, 4), (16, 8), (24, 4)):
                    emit(*li("t2", a), *li("t1", b), f".word 0x{_ins_word(9, 10, pos, size):08X}")  # ins $t2, $t1, pos, size

        # bit/byte ops.
        for a in ops:
            emit(*li("t0", a),
                 f".word 0x{_wsbh(8, 10):08X}",  # wsbh $t2, $t0
                 f".word 0x{_wsbw(8, 11):08X}",  # wsbw $t3, $t0
                 f".word 0x{_seb(8, 12):08X}",   # seb $t4, $t0
                 f".word 0x{_seh(8, 13):08X}",   # seh $t5, $t0
                 f".word 0x{_bitrev(8, 14):08X}") # bitrev $t6, $t0

        # max/min: raw Allegrex SPECIAL encodings (opcode 0).
        # Test a variety of signed pairs:
        max_min_operands = [
            0, 1, 0xFFFFFFFF,       # 0, 1, -1
            0x80000000, 0x7FFFFFFF, # INT32_MIN, INT32_MAX
            0xFFFFFF9C, 100,        # -100, 100
        ]
        # Add some random values from ops
        for op_val in ops[:6]:
            if op_val not in max_min_operands:
                max_min_operands.append(op_val)

        for a in max_min_operands:
            for b in max_min_operands:
                emit(*li("t0", a), *li("t1", b),
                     f".word 0x{_special(8, 9, 10, 0, 0x2C):08X}",  # max $t2, $t0, $t1
                     f".word 0x{_special(8, 9, 10, 0, 0x2D):08X}")  # min $t2, $t0, $t1
 
    if "fpu" in groups:
        # FPU two-operand arithmetic and compares, plus one-operand transforms/conversions.
        fvals = fpairs(extra)
        for a in fvals:
            for b in FEDGE:
                emit(*li("t0", a), "mtc1 $t0, $f0", *li("t1", b), "mtc1 $t1, $f1",
                     "add.s $f2, $f0, $f1", "mfc1 $t2, $f2",
                     "sub.s $f3, $f0, $f1", "mfc1 $t3, $f3",
                     "mul.s $f4, $f0, $f1", "mfc1 $t4, $f4",
                     "div.s $f5, $f0, $f1", "mfc1 $t5, $f5",
                     "c.eq.s $f0, $f1", "c.lt.s $f0, $f1", "c.le.s $f0, $f1",
                     "c.ult.s $f0, $f1", "c.un.s $f0, $f1")
        for a in fvals:
            emit(*li("t0", a), "mtc1 $t0, $f0",
                 "abs.s $f1, $f0", "mfc1 $t1, $f1",
                 "neg.s $f2, $f0", "mfc1 $t2, $f2",
                 "sqrt.s $f3, $f0", "mfc1 $t3, $f3",
                 "cvt.w.s $f4, $f0", "mfc1 $t4, $f4",
                 "trunc.w.s $f5, $f0", "mfc1 $t5, $f5",
                 "round.w.s $f6, $f0", "mfc1 $t6, $f6",
                 "ceil.w.s $f7, $f0", "mfc1 $t7, $f7",
                 "floor.w.s $f8, $f0", "mfc1 $t8, $f8",
                 "cvt.s.w $f9, $f4", "mfc1 $t9, $f9")

    if "vfpu" in groups:
        # Emit some standard VFPU instructions for microtesting
        # These will exercise the basic VFPU arithmetic pathways
        emit("vadd.q $v000, $v100, $v200", "vsub.q $v000, $v100, $v200",
             "vmul.q $v000, $v100, $v200", "vdiv.q $v000, $v100, $v200",
             "vdot.q $s000, $v100, $v200")

    # Draw a simple 64x64 RGBA gradient to guest address 0x09000000
    # representing the virtual framebuffer.
    emit("lui $t0, 0x0900")
    emit("ori $t1, $zero, 4096")
    emit("1:")
    emit("sw $t1, 0($t0)")
    emit("addiu $t0, $t0, 4")
    emit("addiu $t1, $t1, -1")
    emit("bne $t1, $zero, 1b")
    emit("nop")

    return lines



def write_c(path, lines):
    body = "\n".join('\t\t"%s\\n"' % ln for ln in lines)
    clobbers = ", ".join('"%s"' % r for r in
                         ["t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9",
                          "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9",
                          "hi", "lo", "memory"])
    with open(path, "w", encoding="ascii", newline="\n") as out:
        out.write(
            "// Generated by tools/gen_microtest.py. Do not edit by hand.\n"
            "// CRT-free Allegrex integer + FPU differential test module.\n\n"
            "__attribute__((section(\".rodata.sceModuleInfo\"), aligned(4), used))\n"
            "const struct {\n"
            "    unsigned short attributes;\n"
            "    unsigned short version;\n"
            "    char name[28];\n"
            "    unsigned int gp;\n"
            "    unsigned int libent;\n"
            "    unsigned int libentsz;\n"
            "    unsigned int libstub;\n"
            "    unsigned int libstubend;\n"
            "} module_info = {\n"
            "    0,\n"
            "    0x0100,\n"
            "    \"microtest_gen\",\n"
            "    0, 0, 0, 0, 0\n"
            "};\n\n"
            "void exit_stub(void);\n\n"
            "void _start(void) {\n"
            "\t__asm__ volatile(\n"
            + body + "\n"
            "\t\t::: " + clobbers + "\n"
            "\t);\n"
            "\texit_stub();\n"
            "}\n\n"
            "__attribute__((used))\n"
            "void exit_stub(void) {\n"
            "    __asm__ volatile(\".word 0x0008430C\");\n"
            "}\n"
        )


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description="MIPS Allegrex microtest generator")
    parser.add_argument("out_c", help="Output C file path")
    parser.add_argument("extra", type=int, nargs="?", default=6, help="Extra random operand count")
    parser.add_argument("--groups", help="Comma-separated instruction groups (integer,fpu,allegrex,vfpu)")
    parser.add_argument("--opcodes", help="Comma-separated hex opcode words")

    # Support positional arguments alongside flags
    if any(a.startswith("-") for a in argv[1:]):
        args = parser.parse_args(argv[1:])
        out_c = args.out_c
        extra = args.extra
        groups = args.groups.split(",") if args.groups else None
        opcodes = args.opcodes.split(",") if args.opcodes else None
    else:
        if len(argv) < 2:
            sys.stderr.write("usage: gen_microtest.py <out.c> [random_per_op] [--groups G] [--opcodes O]\n")
            return 2
        out_c = argv[1]
        extra = int(argv[2]) if len(argv) > 2 else 6
        groups = None
        opcodes = None

    random.seed(0xA11E)
    lines = gen(extra, groups=groups, opcodes=opcodes)
    write_c(out_c, lines)
    print(f"wrote {out_c}: {len(lines)} instructions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
