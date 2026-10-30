# SPDX-License-Identifier: GPL-3.0-or-later

"""Opt-in local guest-register promotion (#284, ``codegen.py --promote-registers``).

A promoted instruction reads and writes C locals (`_gpr<n>`) instead of s->r[].
These tests pin the rules that keep that invisible to everything outside a span:
the default output carries no trace of the option; a GPR is loaded before its
first read in a span; every dirty local is written back before a label, a
control transfer, an instruction emitted the legacy way and the end of the body;
r0 is never promoted; a write that may not happen keeps the old value; and a
line that touches other guest state or calls a runtime helper is never promoted.
The executable veto is the cosimulation corpus run with the option
(``mingw32-make cosim-selftest-promoted``).
"""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


JR_RA = 0x03E00008
NOP = 0x00000000
SYSCALL = 0x0000000C


def addu(rd, rs, rt):
    return (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | 0x21


def addiu(rt, rs, imm):
    return 0x09 << 26 | (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (imm & 0xFFFF)


def lw(rt, base, off):
    return 0x23 << 26 | (base & 0x1F) << 21 | (rt & 0x1F) << 16 | (off & 0xFFFF)


def sw(rt, base, off):
    return 0x2B << 26 | (base & 0x1F) << 21 | (rt & 0x1F) << 16 | (off & 0xFFFF)


def movz(rd, rs, rt):
    return (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (rd & 0x1F) << 11 | 0x0A


def mfc1(rt, fs):
    return 0x11 << 26 | (rt & 0x1F) << 16 | (fs & 0x1F) << 11


def bne(rs, rt, off):
    return 0x05 << 26 | (rs & 0x1F) << 21 | (rt & 0x1F) << 16 | (off & 0xFFFF)


def jal(target):
    return 0x03 << 26 | ((target >> 2) & 0x03FFFFFF)


class RegisterPromotionTests(unittest.TestCase):
    def setUp(self):
        self._saved = codegen.PROMOTE_REGISTERS
        self.addCleanup(setattr, codegen, "PROMOTE_REGISTERS", self._saved)

    def _emit(self, words, promote, start=0x1000, end=0x1100, known=None):
        codegen.PROMOTE_REGISTERS = promote
        elf = FakeElf(words)
        known = known if known is not None else {start}
        return codegen.emit_function(elf, start, [(start, end)], known)

    def _body(self, promote):
        return {
            0x1000: lw(8, 4, 0),        # t0 = [a0]
            0x1004: addiu(8, 8, 1),     # t0 += 1
            0x1008: sw(8, 4, 0),        # [a0] = t0
            0x100C: addu(2, 8, 0),      # v0 = t0
            0x1010: JR_RA,
            0x1014: NOP,
        }

    def test_default_output_has_no_promotion(self):
        lines = self._emit(self._body(False), promote=False)
        text = "\n".join(lines)
        self.assertNotIn("_gpr", text)
        self.assertNotIn("SR_PROMO_SYNC", text)
        self.assertIn("s->r[8] = MEM_R32(s->r[4] + 0x00000000u);", text)

    def test_span_loads_once_and_writes_back_before_the_return(self):
        lines = self._emit(self._body(True), promote=True)
        text = "\n".join(lines)
        self.assertIn("uint32_t _gpr2, _gpr4, _gpr8;", text)
        self.assertEqual(text.count("_gpr4 = s->r[4];"), 1, "a cached GPR is loaded once per span")
        self.assertIn("_gpr8 = MEM_R32(_gpr4 + 0x00000000u); SR_PROMO_SYNC(s, 8u, _gpr8);", text)
        self.assertIn("MEM_W32_PC(_gpr4 + 0x00000000u, _gpr8, 0x00001008u);", text)
        writeback = text.index("s->r[8] = _gpr8;")
        self.assertIn("s->r[2] = _gpr2;", text)
        self.assertNotIn("s->r[4] = _gpr4;", text, "a GPR only read is not written back")
        self.assertLess(writeback, text.index("_sr_return_target"), "write-back after the return")

    def test_no_register_file_access_inside_a_span(self):
        lines = self._emit(self._body(True), promote=True)
        for line in lines:
            if "_gpr" in line and "s->r[" in line:
                stripped = line.strip()
                self.assertTrue(
                    stripped.startswith("s->r[") or stripped.startswith("_gpr"),
                    f"only loads and write-backs touch s->r beside a local: {line}")

    def test_a_label_writes_back_and_forgets_the_cache(self):
        # loop: t0 += 1; bne t0, t1, loop; nop; jr ra
        words = {
            0x1000: addiu(8, 8, 1),
            0x1004: bne(8, 9, -2),
            0x1008: NOP,
            0x100C: addu(2, 8, 0),
            0x1010: JR_RA,
            0x1014: NOP,
        }
        lines = self._emit(words, promote=True)
        text = "\n".join(lines)
        label = text.index("L_00001000: ;")
        self.assertLess(text.index("_gpr8 = s->r[8];"), text.index("_gpr8 = (_gpr8 + 0x00000001u)"))
        self.assertGreater(text.index("_gpr8 = s->r[8];"), label, "the load follows the label")
        branch = text.index("0x1509fffeu")
        self.assertLess(text.index("s->r[8] = _gpr8;"), branch, "write-back before the branch")
        after = text[branch:]
        self.assertIn("_gpr8 = s->r[8];", after, "the span after the branch reloads")

    def test_a_legacy_instruction_ends_the_span(self):
        words = {
            0x1000: addiu(8, 8, 1),
            0x1004: mfc1(9, 2),         # FPU move: not promoted
            0x1008: addu(2, 8, 9),
            0x100C: JR_RA,
            0x1010: NOP,
        }
        text = "\n".join(self._emit(words, promote=True))
        fpu = text.index("0x44091000u")
        self.assertLess(text.index("s->r[8] = _gpr8;"), fpu)
        after = text[fpu:]
        self.assertIn("_gpr8 = s->r[8];", after)
        self.assertIn("_gpr9 = s->r[9];", after)

    def test_syscall_and_calls_end_the_span(self):
        words = {
            0x1000: addiu(4, 0, 7),
            0x1004: SYSCALL | (0x2015 << 6),
            0x1008: JR_RA,
            0x100C: NOP,
        }
        text = "\n".join(self._emit(words, promote=True))
        self.assertLess(text.index("s->r[4] = _gpr4;"), text.index("sr_raw_syscall"))
        words = {
            0x1000: addiu(4, 0, 7),
            0x1004: jal(0x1040),
            0x1008: NOP,
            0x100C: JR_RA,
            0x1010: NOP,
            0x1040: JR_RA,
            0x1044: NOP,
        }
        text = "\n".join(self._emit(words, promote=True, known={0x1000, 0x1040}))
        self.assertLess(text.index("s->r[4] = _gpr4;"), text.index("s->r[31] = 0x0000100cu;"))

    def test_r0_is_never_promoted(self):
        words = {0x1000: addu(0, 4, 5), 0x1004: addiu(2, 0, 3), 0x1008: JR_RA, 0x100C: NOP}
        text = "\n".join(self._emit(words, promote=True))
        self.assertNotIn("_gpr0", text)
        self.assertIn("_gpr2 = (0u + 0x00000003u);", text)

    def test_a_conditional_write_keeps_the_old_value(self):
        words = {0x1000: movz(2, 4, 5), 0x1004: JR_RA, 0x1008: NOP}
        text = "\n".join(self._emit(words, promote=True))
        load = text.index("_gpr2 = s->r[2];")
        self.assertLess(load, text.index("if (_gpr5 == 0) _gpr2 = _gpr4;"))

    def test_the_safety_check_refuses_other_state_and_helpers(self):
        ok = "    sr_begin(s, 0x1u, 0x0u); _gpr2 = MEM_R32(_gpr4 + 0x4u); SR_PROMO_SYNC(s, 2u, _gpr2); sr_end(s, 0u, 0);"
        self.assertTrue(codegen.promoted_line_is_safe(ok))
        self.assertTrue(codegen.promoted_line_is_safe("{ s->lo = _gpr4; s->hi = _gpr5; }"))
        for bad in ("s->pc = 1u;", "s->fi[2] = _gpr3;", "sr_lwl(_gpr2, _gpr3)", "return;",
                    "sr_raw_syscall(s, 1u, 2u);", "dispatch(s, _gpr2);", "goto L_00001000;"):
            self.assertFalse(codegen.promoted_line_is_safe(bad), bad)

    def test_lle_guarded_accesses_are_not_promoted(self):
        words = {0x1000: lw(8, 4, 0), 0x1004: JR_RA, 0x1008: NOP}
        codegen.PROMOTE_REGISTERS = True
        try:
            text = "\n".join(codegen.emit_function(FakeElf(words), 0x1000, [(0x1000, 0x1100)],
                                                   {0x1000}, lle_cpu=True))
        finally:
            codegen.PROMOTE_REGISTERS = self._saved
        self.assertIn("sr_cpu_guard_access", text)
        self.assertNotIn("_gpr8", text)


if __name__ == "__main__":
    unittest.main()
