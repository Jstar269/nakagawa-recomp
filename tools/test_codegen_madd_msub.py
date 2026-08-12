# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for PSP Allegrex madd/maddu/msub/msubu decoding and emission.

The PSP Allegrex places these multiply-accumulate instructions in the normal SPECIAL
table (opcode 0), NOT in generic MIPS32 SPECIAL2 (opcode 0x1C). Verified against:
  - PPSSPP Core/MIPS/MIPSTables.cpp (tableSpecial[64])
  - PPSSPP Core/MIPS/MIPSInt.cpp Int_MulDivType
  - Private HST ELF: 0x00026BAC word 0x0062001C (madd), 13 total occurrences
  - Binutils Allegrex patch: madd encoding 0x0000001c

Generic MIPS32 SPECIAL2 (opcode 0x1C) contains madd/maddu/mul/msub/msubu/clz/clo at
funct 0x00/0x01/0x02/0x04/0x05/0x20/0x21. These are NOT valid PSP encodings.
PSP SPECIAL2 (opcode 0x1C) contains halt/mfic/mtic at funct 0x00/0x24/0x26.

Tests verify:
  (a) codegen emits the correct instruction under opcode 0 / correct funct
  (b) the emitted accumulator construction is UB-safe (uint64_t hi<<32|lo)
  (c) numeric results match across -O0/-O1/-O2
  (d) opcode 0x1C / funct 0x00 is NOT decoded as Allegrex madd
  (e) all four instructions: madd, maddu, msub, msubu

Allegrex integer extension encoding matrix (public ISA knowledge):
  Instruction   | opcode | funct | rs | rt | rd | sa | Notes
  ---------------|--------|-------|----|----|----|----|------------------
  clz            |   0    | 0x16  | S  | -  | D  | -  | count leading zeros
  clo            |   0    | 0x17  | S  | -  | D  | -  | count leading ones
  madd           |   0    | 0x1C  | S  | T  | -  | -  | signed multiply-accumulate
  maddu          |   0    | 0x1D  | S  | T  | -  | -  | unsigned multiply-accumulate
  max            |   0    | 0x2C  | S  | T  | D  | -  | signed maximum
  min            |   0    | 0x2D  | S  | T  | D  | -  | signed minimum
  msub           |   0    | 0x2E  | S  | T  | -  | -  | signed multiply-subtract
  msubu          |   0    | 0x2F  | S  | T  | -  | -  | unsigned multiply-subtract
  ext            |  0x1F  | 0x00  | S  | T  | D  | pos| extract bit field
  ins            |  0x1F  | 0x04  | S  | T  | D  | pos| insert bit field
  wsbh           |  0x1F  | 0x20  | -  | T  | D  | 0x02| swap bytes within halfword
  wsbw           |  0x1F  | 0x20  | -  | T  | D  | 0x03| swap bytes within word
  seb            |  0x1F  | 0x20  | -  | T  | D  | 0x10| sign-extend byte
  seh            |  0x1F  | 0x20  | -  | T  | D  | 0x18| sign-extend halfword
  bitrev         |  0x1F  | 0x20  | -  | T  | D  | 0x14| reverse bit order
  halt           |  0x1C  | 0x00  | -  | -  | -  | -  | wait for interrupt (kernel)
  mfic           |  0x1C  | 0x24  | -  | -  | D  | -  | read interrupt controller
  mtic           |  0x1C  | 0x26  | -  | -  | -  | -  | write interrupt controller

Note: generic MIPS32 SPECIAL2 (opcode 0x1C) also has madd(0x00)/maddu(0x01)/mul(0x02)/
msub(0x04)/msubu(0x05)/clz(0x20)/clo(0x21) at DIFFERENT funct values than PSP SPECIAL2.
These generic encodings must NOT be accepted as PSP Allegrex instructions.
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import codegen

GCC = shutil.which("gcc") or shutil.which("cc")


def encode_special(rs, rt, rd, funct):
    """PSP Allegrex SPECIAL encoding: opcode=0, funct in low 6 bits."""
    return (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | (funct & 0x3F)


def encode_special2(rs, rt, rd, funct):
    """Generic MIPS32 SPECIAL2 encoding: opcode=0x1C, funct in low 6 bits.
    This is NOT a valid PSP Allegrex madd encoding."""
    return (0x1C << 26) | (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | (funct & 0x3F)


def expected_hi_lo(hi, lo, ra, rb, sign):
    """Independent oracle: correct MIPS madd/msub semantics (mod 2**64)."""
    def s32(v):
        return v - 0x100000000 if v & 0x80000000 else v

    acc = (hi << 32) | lo
    prod = s32(ra) * s32(rb)
    acc = (acc + sign * prod) & 0xFFFFFFFFFFFFFFFF
    return (acc >> 32) & 0xFFFFFFFF, acc & 0xFFFFFFFF


def expected_hi_lo_u(hi, lo, ra, rb, sign):
    """Independent oracle: correct MIPS maddu/msubu semantics (mod 2**64)."""
    acc = (hi << 32) | lo
    prod = (ra * rb) & 0xFFFFFFFFFFFFFFFF
    acc = (acc + sign * prod) & 0xFFFFFFFFFFFFFFFF
    return (acc >> 32) & 0xFFFFFFFF, acc & 0xFFFFFFFF


class TestAllegrexMaddDecoding(unittest.TestCase):
    """Decode-placement tests: PSP madd is SPECIAL (opcode 0), not SPECIAL2."""

    def test_madd_is_opcode0_not_opcode1C(self):
        # PSP Allegrex madd: opcode 0, funct 0x1C
        word_psp = encode_special(4, 5, 0, 0x1C)
        stmt, _, _ = codegen.effect(0x00026BAC, word_psp)
        # Must NOT raise Unsupported
        self.assertNotIn("Unsupported", stmt)
        self.assertIn("_acc", stmt)
        self.assertIn("s->hi", stmt)

    def test_generic_special2_madd_is_not_psp_madd(self):
        # Generic MIPS32 SPECIAL2 madd: opcode 0x1C, funct 0x00
        # This is NOT the PSP encoding and must NOT be decoded as Allegrex madd.
        word_generic = encode_special2(4, 5, 0, 0x00)
        with self.assertRaises(codegen.Unsupported):
            codegen.effect(0, word_generic)

    def test_generic_special2_clz_is_not_psp_clz(self):
        # Generic MIPS32 SPECIAL2 clz: opcode 0x1C, funct 0x20
        word_generic = encode_special2(0, 0, 2, 0x20)
        with self.assertRaises(codegen.Unsupported):
            codegen.effect(0, word_generic)

    def test_maddu_is_opcode0_funct1D(self):
        word = encode_special(4, 5, 0, 0x1D)
        stmt, _, _ = codegen.effect(0, word)
        self.assertNotIn("Unsupported", stmt)
        self.assertIn("_acc", stmt)

    def test_msub_is_opcode0_funct2E(self):
        word = encode_special(4, 5, 0, 0x2E)
        stmt, _, _ = codegen.effect(0, word)
        self.assertNotIn("Unsupported", stmt)
        self.assertIn("_acc", stmt)
        self.assertIn("_prod", stmt)

    def test_msubu_is_opcode0_funct2F(self):
        word = encode_special(4, 5, 0, 0x2F)
        stmt, _, _ = codegen.effect(0, word)
        self.assertNotIn("Unsupported", stmt)
        self.assertIn("_acc", stmt)


class TestMaddMsubEmission(unittest.TestCase):
    """UB-safe accumulator construction and numeric correctness."""

    def _run_snippet(self, stmt, hi, lo, ra, rb, opt):
        src = f"""
#include <stdint.h>
#include <stdio.h>
struct CpuState {{ uint32_t r[32]; uint32_t hi, lo; }};
int main(void) {{
    struct CpuState state;
    struct CpuState *s = &state;
    s->hi = 0x{hi:08x}u; s->lo = 0x{lo:08x}u;
    s->r[4] = 0x{ra:08x}u; s->r[5] = 0x{rb:08x}u;
    {stmt}
    printf("%08x %08x\\n", s->hi, s->lo);
    return 0;
}}
"""
        with tempfile.TemporaryDirectory() as td:
            assert GCC is not None
            c_path = Path(td) / "snippet.c"
            exe_path = Path(td) / ("snippet.exe" if sys.platform == "win32" else "snippet")
            c_path.write_text(src)
            subprocess.run([GCC, opt, "-std=c11", "-o", str(exe_path), str(c_path)],
                            check=True, capture_output=True, text=True)
            out = subprocess.run([str(exe_path)], check=True, capture_output=True, text=True)
        hi_out, lo_out = out.stdout.split()
        return int(hi_out, 16), int(lo_out, 16)

    def _assert_no_ub_shift(self, stmt):
        """The emitted code must NOT contain ((int64_t)(int32_t)s->hi << 32)."""
        compact = stmt.replace(" ", "").replace("\n", "")
        self.assertNotIn("(int64_t)(int32_t)s->hi<<32", compact)
        self.assertIn("(uint64_t)s->hi<<32", compact)

    # --- madd (signed multiply-accumulate) ---

    def test_madd_snippet_has_no_ub_shift(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        self._assert_no_ub_shift(stmt)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_positive_operands_O0(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        hi, lo, ra, rb = 0, 0, 0x00000006, 0x00000007
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O0")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_positive_operands_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        hi, lo, ra, rb = 0, 0, 0x00000006, 0x00000007
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_negative_signed_and_hi_sign_bit_O2(self):
        # The case that triggers UB in the old expression: hi=0x80000000 (sign bit set)
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        hi, lo, ra, rb = 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000002
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_wrap_mod_2_64_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        hi, lo, ra, rb = 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_zero_accumulator_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        hi, lo, ra, rb = 0, 0, 0, 0
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_madd_int32_min_overflow_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1C))
        # INT32_MIN * -1 = 0x80000000 * 0xFFFFFFFF = 0x8000000000000000 (signed overflow wraps)
        hi, lo, ra, rb = 0, 0, 0x80000000, 0xFFFFFFFF
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    # --- maddu (unsigned multiply-accumulate) ---

    def test_maddu_snippet_has_no_ub_shift(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1D))
        compact = stmt.replace(" ", "").replace("\n", "")
        self.assertNotIn("(int64_t)(int32_t)s->hi<<32", compact)
        self.assertIn("(uint64_t)s->hi<<32", compact)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_maddu_numeric_result_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x1D))
        hi, lo, ra, rb = 0, 0, 0xFFFFFFFF, 0xFFFFFFFF
        want_hi, want_lo = expected_hi_lo_u(hi, lo, ra, rb, sign=+1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    # --- msub (signed multiply-subtract) ---

    def test_msub_snippet_has_no_ub_shift(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x2E))
        self._assert_no_ub_shift(stmt)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_msub_negative_signed_and_hi_sign_bit_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x2E))
        hi, lo, ra, rb = 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000002
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=-1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_msub_positive_operands_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x2E))
        hi, lo, ra, rb = 0x100, 0, 0x00000010, 0x00000020
        want_hi, want_lo = expected_hi_lo(hi, lo, ra, rb, sign=-1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    # --- msubu (unsigned multiply-subtract) ---

    def test_msubu_snippet_has_no_ub_shift(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x2F))
        compact = stmt.replace(" ", "").replace("\n", "")
        self.assertNotIn("(int64_t)(int32_t)s->hi<<32", compact)
        self.assertIn("(uint64_t)s->hi<<32", compact)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_msubu_numeric_result_O2(self):
        stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, 0x2F))
        hi, lo, ra, rb = 0, 0, 0xFFFFFFFF, 0x00000002
        want_hi, want_lo = expected_hi_lo_u(hi, lo, ra, rb, sign=-1)
        got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
        self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    # --- All four at -O0 and -O2 with edge operands ---

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_all_four_instructions_edge_operands_O0(self):
        for fn, sign, oracle in [
            (0x1C, +1, expected_hi_lo),
            (0x1D, +1, expected_hi_lo_u),
            (0x2E, -1, expected_hi_lo),
            (0x2F, -1, expected_hi_lo_u),
        ]:
            with self.subTest(fn=fn):
                stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, fn))
                hi, lo, ra, rb = 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000002
                want_hi, want_lo = oracle(hi, lo, ra, rb, sign)
                got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O0")
                self.assertEqual((got_hi, got_lo), (want_hi, want_lo))

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_all_four_instructions_edge_operands_O2(self):
        for fn, sign, oracle in [
            (0x1C, +1, expected_hi_lo),
            (0x1D, +1, expected_hi_lo_u),
            (0x2E, -1, expected_hi_lo),
            (0x2F, -1, expected_hi_lo_u),
        ]:
            with self.subTest(fn=fn):
                stmt, _, _ = codegen.effect(0, encode_special(4, 5, 0, fn))
                hi, lo, ra, rb = 0x80000000, 0x00000001, 0xFFFFFFFF, 0x00000002
                want_hi, want_lo = oracle(hi, lo, ra, rb, sign)
                got_hi, got_lo = self._run_snippet(stmt, hi, lo, ra, rb, "-O2")
                self.assertEqual((got_hi, got_lo), (want_hi, want_lo))


class TestAllegrexEncodingsDocumented(unittest.TestCase):
    """Verify the documented PSP Allegrex encoding matrix matches codegen behavior."""

    def _check(self, funct_val, expected_name):
        word = encode_special(4, 5, 0, funct_val)
        stmt, _, _ = codegen.effect(0, word)
        self.assertNotIn("Unsupported", stmt, msg=f"{expected_name} (funct 0x{funct_val:02x}) should be supported")

    def test_clz_funct16(self):
        self._check(0x16, "clz")

    def test_clo_funct17(self):
        self._check(0x17, "clo")

    def test_madd_funct1C(self):
        self._check(0x1C, "madd")

    def test_maddu_funct1D(self):
        self._check(0x1D, "maddu")

    def test_max_funct2C(self):
        self._check(0x2C, "max")

    def test_min_funct2D(self):
        self._check(0x2D, "min")

    def test_msub_funct2E(self):
        self._check(0x2E, "msub")

    def test_msubu_funct2F(self):
        self._check(0x2F, "msubu")

    def test_private_elf_madd_words_are_translated(self):
        """The private HST ELF contains madd at these addresses.
        This test encodes the exact word values (opcode 0 / funct 0x1C)
        and verifies codegen translates them without fallback."""
        # Words extracted from private ELF .text: opcode 0 / funct 0x1C / various rs/rt
        known_madd_words = [
            0x0062001C,  # 0x00026BAC: madd $t3, $t2 (rs=3, rt=2 from word decode)
            0x00C3001C,  # 0x00026BC8: madd $t6, $t3
            0x0068001C,  # 0x00026BE4: madd $t3, $t8
            0x00E9001C,  # 0x0003328C: madd $t7, $t9
            0x0064001C,  # 0x00036684: madd $t3, $t4
            0x0062001C,  # 0x00040E4C: madd $t3, $t2
            0x0062001C,  # 0x000410FC: madd $t3, $t2
        ]
        for word in known_madd_words:
            with self.subTest(word=f"0x{word:08x}"):
                stmt, _, _ = codegen.effect(0, word)
                self.assertNotIn("Unsupported", stmt,
                    msg=f"PSP madd word 0x{word:08x} should be translated, not fallback")
                self.assertIn("_acc", stmt)
                self.assertIn("s->hi", stmt)


if __name__ == "__main__":
    unittest.main()
