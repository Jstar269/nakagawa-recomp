# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for Gate B ISA purity: every Allegrex instruction-under-test
must be emitted as a raw PSP word, and the compiled ELF must contain no accidental
generic MIPS32 SPECIAL2 arithmetic encodings.

2026-07-19. The previous Gate B design relied on `-march=24kc` to choose Allegrex
encodings. That is wrong: generic MIPS32 SPECIAL2 uses different funct values for
madd-family and clz/clo. This module verifies that the synthetic Gate B design
uses explicit raw .word encodings and that the compiled ELF matches.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import codegen

REPO = Path(__file__).resolve().parent.parent
GCC = shutil.which("gcc") or shutil.which("cc")


def gen_microtest(out_c, extra=2, groups=None):
    # A fresh checkout has no build/ yet (CI creates it only after this suite runs).
    Path(out_c).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, str(REPO / "tools" / "gen_microtest.py"),
         str(out_c), str(extra), "--groups", groups or "allegrex"],
        check=True, cwd=REPO,
    )


def compile_elf(src, out_elf, march="r4000"):
    subprocess.run(
        ["mipsel-linux-gnu-gcc", "-nostdlib", "-static", "-Ttext=0x08900000",
         f"-march={march}", "-mabi=32", "-Ibuild", "-o", str(out_elf), str(src)],
        check=True, cwd=REPO,
    )


def find_symbol_address(elf_path, sym_name):
    import struct
    from analyze import Elf
    elf = Elf(elf_path)
    symtab = elf.sec(".symtab")
    strtab = elf.sec(".strtab")
    if not symtab or not strtab:
        return None
    d = elf.data
    for i in range(symtab["size"] // symtab["entsz"]):
        o = symtab["off"] + i * symtab["entsz"]
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack("<IIIBBH", d[o:o + 16])
        e = strtab["off"] + st_name
        name = d[e:d.find(b"\x00", e)].decode("ascii", "replace")
        if name == sym_name:
            return st_value
    return None


def read_text_section(elf):
    """Return the raw .text bytes from a MIPS ELF.

    objcopy does not write to stdout for "-": it creates a literal file named
    "-" and the pipe stays empty, which made every word-scanning audit below
    iterate over zero instructions. Extract to a real temp file instead.
    """
    with tempfile.TemporaryDirectory(prefix="gateb_text_") as tmp:
        out = Path(tmp) / "text.bin"
        subprocess.run(
            ["mipsel-linux-gnu-objcopy", "-O", "binary", "-j", ".text", str(elf), str(out)],
            check=True, capture_output=True, cwd=REPO,
        )
        return out.read_bytes()


def disassemble_words(data):
    """Yield (pc, word) for each 4-byte instruction in the raw .text."""
    for i in range(0, len(data) - 3, 4):
        pc = 0x08900000 + i
        word = int.from_bytes(data[i:i+4], "little")
        yield pc, word


class TestAllegrexEncoderHelpers(unittest.TestCase):
    """Unit tests for the raw PSP word encoding relationships. These verify externally
    established field constraints rather than duplicating generator helper implementations."""

    def test_madd_opcode0_funct1C(self):
        w = 0x0109001C
        self.assertEqual((w >> 26) & 0x3F, 0x00)
        self.assertEqual(w & 0x3F, 0x1C)

    def test_clz_opcode0_funct16(self):
        w = 0x01005016
        self.assertEqual((w >> 26) & 0x3F, 0x00)
        self.assertEqual(w & 0x3F, 0x16)

    def test_max_opcode0_funct2C(self):
        w = 0x0109502C
        self.assertEqual((w >> 26) & 0x3F, 0x00)
        self.assertEqual(w & 0x3F, 0x2C)

    def test_ext_rd_field_is_size_minus_1(self):
        for size in (1, 8, 16, 32):
            w = (0x1F << 26) | (8 << 21) | (9 << 16) | ((size - 1) << 11) | (0 << 6) | 0x00
            self.assertEqual((w >> 11) & 0x1F, size - 1)

    def test_ins_msb_field_is_pos_plus_size_minus_1(self):
        for pos, size in ((0, 8), (8, 8), (4, 12), (0, 1), (8, 4), (16, 8), (24, 4)):
            w = (0x1F << 26) | (8 << 21) | (9 << 16) | ((pos + size - 1) << 11) | (pos << 6) | 0x04
            self.assertEqual((w >> 11) & 0x1F, pos + size - 1)
            self.assertEqual((w >> 6) & 0x1F, pos)

    def test_wsbh_special3_sub02(self):
        w = (0x1F << 26) | (0 << 21) | (8 << 16) | (8 << 11) | (0x02 << 6) | 0x20
        self.assertEqual((w >> 26) & 0x3F, 0x1F)
        self.assertEqual((w >> 6) & 0x1F, 0x02)
        self.assertEqual(w & 0x3F, 0x20)

    def test_wsbw_special3_sub03(self):
        w = (0x1F << 26) | (0 << 21) | (8 << 16) | (8 << 11) | (0x03 << 6) | 0x20
        self.assertEqual((w >> 26) & 0x3F, 0x1F)
        self.assertEqual((w >> 6) & 0x1F, 0x03)
        self.assertEqual(w & 0x3F, 0x20)

    def test_seb_special3_sub10(self):
        w = (0x1F << 26) | (0 << 21) | (8 << 16) | (8 << 11) | (0x10 << 6) | 0x20
        self.assertEqual((w >> 26) & 0x3F, 0x1F)
        self.assertEqual((w >> 6) & 0x1F, 0x10)
        self.assertEqual(w & 0x3F, 0x20)

    def test_seh_special3_sub18(self):
        w = (0x1F << 26) | (0 << 21) | (8 << 16) | (8 << 11) | (0x18 << 6) | 0x20
        self.assertEqual((w >> 26) & 0x3F, 0x1F)
        self.assertEqual((w >> 6) & 0x1F, 0x18)
        self.assertEqual(w & 0x3F, 0x20)

    def test_bitrev_special3_sub14(self):
        w = (0x1F << 26) | (0 << 21) | (8 << 16) | (8 << 11) | (0x14 << 6) | 0x20
        self.assertEqual((w >> 26) & 0x3F, 0x1F)
        self.assertEqual((w >> 6) & 0x1F, 0x14)
        self.assertEqual(w & 0x3F, 0x20)


class TestGateBSourceUsesRawWords(unittest.TestCase):
    """The generated Gate B C source must contain .word for every Allegrex-specific
    instruction-under-test and must not contain assembler mnemonics for them."""

    @classmethod
    def setUpClass(cls):
        cls.src = REPO / "build" / "microtest_b_isa_check.c"
        cls.src.parent.mkdir(parents=True, exist_ok=True)
        gen_microtest(cls.src, 2, "allegrex")
        cls.text = cls.src.read_text(encoding="ascii")

    @classmethod
    def tearDownClass(cls):
        if cls.src.exists():
            cls.src.unlink()

    def _assert_no_assembler_mnemonics(self, mnemonics):
        for m in mnemonics:
            self.assertNotIn(m, self.text,
                             f"Gate B source contains assembler mnemonic '{m}' instead of raw .word")

    def test_madd_family_are_raw_words(self):
        self.assertIn(".word 0x014B001C", self.text)
        self.assertIn(".word 0x014B001D", self.text)
        self.assertIn(".word 0x014B002E", self.text)
        self.assertIn(".word 0x014B002F", self.text)
        self._assert_no_assembler_mnemonics(["madd", "maddu", "msub", "msubu"])

    def test_clz_clo_are_raw_words(self):
        self.assertIn(".word 0x01005016", self.text)
        self.assertIn(".word 0x01005017", self.text)
        self._assert_no_assembler_mnemonics(["clz", "clo"])

    def test_ext_ins_are_raw_words(self):
        self._assert_no_assembler_mnemonics(["ext ", "ins "])

    def test_bitbyte_ops_are_raw_words(self):
        self.assertIn(".word 0x7C0850A0", self.text) # wsbh
        self.assertIn(".word 0x7C0858E0", self.text) # wsbw
        self.assertIn(".word 0x7C086420", self.text) # seb
        self.assertIn(".word 0x7C086E20", self.text) # seh
        self.assertIn(".word 0x7C087520", self.text) # bitrev
        self._assert_no_assembler_mnemonics(["wsbh", "wsbw", "seb", "seh", "bitrev"])

    def test_max_min_are_raw_words(self):
        self.assertIn(".word 0x0109502C", self.text)
        self.assertIn(".word 0x0109502D", self.text)


class TestGateBElfEncodingAudit(unittest.TestCase):
    """Compiled Gate B ELF must contain the expected raw PSP Allegrex words and
    must NOT contain generic MIPS32 SPECIAL2 arithmetic encodings for instructions
    that are supposed to be Allegrex SPECIAL."""

    @classmethod
    def setUpClass(cls):
        if not _HAS_MIPS_GCC:
            raise unittest.SkipTest("mipsel-linux-gnu-gcc not available")
        cls.src = REPO / "build" / "microtest_b_audit.c"
        cls.elf = REPO / "build" / "microtest_b_audit.elf"
        gen_microtest(cls.src, 2, "allegrex")
        compile_elf(cls.src, cls.elf, march="r4000")
        cls.text_data = read_text_section(cls.elf)

    @classmethod
    def tearDownClass(cls):
        if cls.src.exists():
            cls.src.unlink()
        if cls.elf.exists():
            cls.elf.unlink()

    def _collect_words(self):
        words = []
        for pc, w in disassemble_words(self.text_data):
            words.append((pc, w))
        return words

    def test_madd_is_special_not_special2(self):
        for _, w in self._collect_words():
            if (w >> 26) == 0x00 and (w & 0x3F) == 0x1C:
                return  # found correct PSP madd
        self.fail("No Allegrex madd (opcode=0, funct=0x1C) found in Gate B ELF")

    def test_no_generic_special2_madd(self):
        for pc, w in self._collect_words():
            if w == 0x71090000:
                self.fail(f"Found generic SPECIAL2 madd word 0x71090000 at pc=0x{pc:08x}")

    def test_no_generic_special2_clz(self):
        for pc, w in self._collect_words():
            opcode = (w >> 26) & 0x3F
            funct = w & 0x3F
            if opcode == 0x1C and funct == 0x20:
                self.fail(f"Found generic SPECIAL2 clz (opcode=0x1C, funct=0x20) at pc=0x{pc:08x}")

    def test_termination_is_explicit_syscall(self):
        exit_stub_addr = find_symbol_address(self.elf, "exit_stub")
        if exit_stub_addr is None:
            self.fail("Could not find 'exit_stub' symbol in Gate B ELF")
        
        words_dict = {pc: w for pc, w in self._collect_words()}
        
        found_syscall = False
        found_pc = None
        found_w = None
        for offset in range(0, 16, 4):
            pc = exit_stub_addr + offset
            w = words_dict.get(pc)
            if w is not None:
                if (w >> 26) == 0 and (w & 0x3F) == 0x0C:
                    found_syscall = True
                    found_pc = pc
                    found_w = w
                    break
        
        self.assertTrue(found_syscall, f"No syscall instruction found in exit_stub at 0x{exit_stub_addr:08x}")
        code = (found_w >> 6) & 0xFFFFF
        self.assertEqual(code, 0x210c, f"exit syscall in exit_stub at 0x{found_pc:08x} has code 0x{code:x}, want 0x210c")

    def test_syscall_translations_are_not_collapsed(self):
        # syscall 0x123 -> op = (0x123 << 6) | 0x0C = 0x48CC
        stmt1, _, _ = codegen.effect(0, 0x48CC)
        # syscall 0x456 -> op = (0x456 << 6) | 0x0C = 0x1158C
        stmt2, _, _ = codegen.effect(0, 0x1158C)
        # Match the code argument only: sr_raw_syscall also takes the pc.
        self.assertIn("sr_raw_syscall(s, 291u", stmt1)   # 0x123 = 291
        self.assertIn("sr_raw_syscall(s, 1110u", stmt2)  # 0x456 = 1110
        self.assertNotEqual(stmt1, stmt2)


class TestAllegrexSemantics(unittest.TestCase):
    """Semantic verification of generated Allegrex instruction C code via host GCC."""

    def _run_snippet(self, stmt, init_regs, opt="-O0"):
        src = f"""
#include <stdint.h>
#include <stdio.h>
struct CpuState {{ uint32_t r[32]; }};
int main(void) {{
    struct CpuState state = {{0}};
"""
        for reg, val in init_regs.items():
            src += f"    state.r[{reg}] = 0x{val:08x}u;\n"
        src += f"    {stmt}\n"
        src += """    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            c_path = Path(td) / "snippet.c"
            exe_path = Path(td) / ("snippet.exe" if sys.platform == "win32" else "snippet")
            c_path.write_text(src)
            subprocess.run([GCC, opt, "-std=c11", "-o", str(exe_path), str(c_path)],
                            check=True, capture_output=True, text=True)
            subprocess.run([str(exe_path)], check=True, capture_output=True, text=True)

    def _run_snippet_printf(self, stmt, init_regs, fmt_expr, opt="-O0", print_reg=10):
        src = f"""
#include <stdint.h>
#include <stdio.h>
struct CpuState {{ uint32_t r[32]; }};
int main(void) {{
    struct CpuState state = {{0}};
    struct CpuState *s = &state;
"""
        for reg, val in init_regs.items():
            src += f"    s->r[{reg}] = 0x{val & 0xFFFFFFFF:08x}u;\n"
        src += f"    {stmt}\n"
        src += f'    printf("{fmt_expr}\\n", s->r[{print_reg}]);\n'
        src += """    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            c_path = Path(td) / "snippet.c"
            exe_path = Path(td) / ("snippet.exe" if sys.platform == "win32" else "snippet")
            c_path.write_text(src)
            result = subprocess.run([GCC, opt, "-std=c11", "-o", str(exe_path), str(c_path)],
                            capture_output=True, text=True)
            if result.returncode != 0:
                self.fail(f"gcc failed: {result.stderr}\nGenerated source:\n{src}")
            out = subprocess.run([str(exe_path)], capture_output=True, text=True, check=True)
            return out.stdout.strip()

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_max_signed_semantics(self):
        cases = [
            (0, 1, 1), (-1, 0, 0), (0x80000000, 0x7FFFFFFF, 0x7FFFFFFF),
            (5, -3, 5), (7, 7, 7), (0xFFFFFFFF, 0x00000001, 1),
        ]
        for a, b, expected in cases:
            with self.subTest(a=f"0x{a:08x}", b=f"0x{b:08x}"):
                stmt, _, _ = codegen.effect(0, (0 << 26) | (8 << 21) | (9 << 16) | (10 << 11) | (0 << 6) | 0x2C)
                got = self._run_snippet_printf(stmt, {8: a, 9: b}, "%u", print_reg=10)
                self.assertEqual(int(got), expected)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_min_signed_semantics(self):
        cases = [
            (0, 1, 0), (-1, 0, -1), (0x80000000, 0x7FFFFFFF, -2147483648),
            (5, -3, -3), (7, 7, 7), (0xFFFFFFFF, 0x00000001, -1),
        ]
        for a, b, expected in cases:
            with self.subTest(a=f"0x{a:08x}", b=f"0x{b:08x}"):
                stmt, _, _ = codegen.effect(0, (0 << 26) | (8 << 21) | (9 << 16) | (10 << 11) | (0 << 6) | 0x2D)
                got = self._run_snippet_printf(stmt, {8: a, 9: b}, "%d", print_reg=10)
                self.assertEqual(int(got), expected)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_ext_nonzero_positions(self):
        for pos, size, expected in [(4, 8, 0x67), (8, 16, 0x3456), (12, 8, 0x45)]:
            with self.subTest(pos=pos, size=size):
                stmt, _, _ = codegen.effect(0, (0x1F << 26) | (8 << 21) | (9 << 16) | ((size - 1) << 11) | (pos << 6) | 0x00)
                got = self._run_snippet_printf(stmt, {8: 0x12345678}, "%08x", print_reg=9)
                self.assertEqual(int(got, 16), expected)

    @unittest.skipUnless(GCC, "no host C compiler on PATH")
    def test_ins_nonzero_positions(self):
        for pos, size, expected in [(8, 8, 0x1234CD78), (4, 12, 0x1234BCD8), (0, 8, 0x123456CD)]:
            with self.subTest(pos=pos, size=size):
                msb = pos + size - 1
                stmt, _, _ = codegen.effect(0, (0x1F << 26) | (9 << 21) | (10 << 16) | (msb << 11) | (pos << 6) | 0x04)
                got = self._run_snippet_printf(stmt, {9: 0x1234ABCD, 10: 0x12345678}, "%08x", print_reg=10)
                self.assertEqual(int(got, 16), expected)


def _check_mips_gcc():
    try:
        subprocess.run(
            ["mipsel-linux-gnu-gcc", "--version"],
            check=True, capture_output=True, cwd=REPO,
        )
        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False


_HAS_MIPS_GCC = _check_mips_gcc()


if __name__ == "__main__":
    unittest.main()
