# SPDX-License-Identifier: GPL-2.0-or-later
"""LLE Phase 1 (PR 2) gates: COP0, exceptions, eret, and interpreter support.

Failing-before evidence (spec section 11, PR 2): on the base tree generated
syscalls use sr_raw_syscall, generated breaks use sr_break, effect() raises
Unsupported for every COP0 word, is_control() misses eret, function_flow()
falls through eret, delay_slot_lines/LLE_CPU do not exist, src/rt/cpu_lle.*
do not exist, the interpreter answers UNSUPPORTED for syscall/break with no
eret path, and the Makefile has no cpu-lle-selftest target. Every LLE test
below fails there and passes here; the HLE-default tests pin the behavior
that must not move.
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen


CPU_LLE_H = ROOT / "src" / "rt" / "cpu_lle.h"
CPU_LLE_C = ROOT / "src" / "rt" / "cpu_lle.c"
GUEST_INTERP_H = ROOT / "src" / "rt" / "guest_interp.h"
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


def mfc0(rt, rd, sel=0):
    return (0x10 << 26) | (rt << 16) | (rd << 11) | sel


def mtc0(rt, rd, sel=0):
    return (0x10 << 26) | (4 << 21) | (rt << 16) | (rd << 11) | sel


ERET = 0x42000018
SYSCALL = 0x0000000C
BREAK = 0x0000000D
JR_RA = 0x03E00008
NOP = 0x00000000


class HleDefaultPreservedTests(unittest.TestCase):
    """Default-profile codegen still routes through the HLE path."""

    def test_default_syscall_uses_raw_syscall(self):
        stmt, _, _ = codegen.effect(0x1000, SYSCALL)
        self.assertIn("sr_raw_syscall(s, 0u", stmt)
        self.assertNotIn("sr_cpu_raise_exception", stmt)

    def test_default_break_uses_sr_break(self):
        stmt, _, _ = codegen.effect(0x1000, BREAK)
        self.assertIn("sr_break(s, 0u", stmt)
        self.assertNotIn("sr_cpu_raise_exception", stmt)

    def test_default_syscall_codes_survive(self):
        stmt1, _, _ = codegen.effect(0, 0x48CC)
        stmt2, _, _ = codegen.effect(0, 0x1158C)
        self.assertIn("sr_raw_syscall(s, 291u", stmt1)
        self.assertIn("sr_raw_syscall(s, 1110u", stmt2)

    def test_emit_default_syscall_function(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: SYSCALL}),
                0x1000, [(0x1000, 0x1008)], {0x1000},
            )
        )
        self.assertIn("sr_raw_syscall", text)
        self.assertNotIn("sr_cpu_raise_exception", text)

    def test_production_fixture_recipes_do_not_opt_into_lle(self):
        for target in ("production-smoke:", "cosim-selftest:"):
            recipe = MAKEFILE.split(target, 1)[1].split("\n\n", 1)[0]
            self.assertNotIn("--lle-cpu", recipe)


class LleCodegenTests(unittest.TestCase):
    """LLE-mode codegen raises guest exceptions with flow propagation."""

    def test_lle_syscall_raises_sys(self):
        stmt, _, _ = codegen.effect(0x1000, SYSCALL, lle_cpu=True)
        self.assertIn("sr_cpu_raise_exception(s, 8u, 0x00001000u", stmt)
        self.assertIn("return;", stmt)
        self.assertNotIn("sr_raw_syscall", stmt)

    def test_lle_break_raises_bp(self):
        stmt, _, _ = codegen.effect(0x1000, BREAK, lle_cpu=True)
        self.assertIn("sr_cpu_raise_exception(s, 9u, 0x00001000u", stmt)
        self.assertNotIn("sr_break", stmt)

    def test_mfc0_emits_helper(self):
        stmt, _, _ = codegen.effect(0x1000, mfc0(8, 12))
        self.assertIn("sr_cp0_mfc0(s, 8u, 12u, 0u, 0x00001000u)", stmt)

    def test_mtc0_emits_helper(self):
        stmt, _, _ = codegen.effect(0x1000, mtc0(8, 12))
        self.assertIn("sr_cp0_mtc0(s, 8u, 12u, 0u, 0x00001000u)", stmt)

    def test_eret_emits_helper(self):
        stmt, _, _ = codegen.effect(0x1000, ERET)
        self.assertIn("sr_cpu_eret(s, 0x00001000u)", stmt)

    def test_cop0_reserved_bits_stay_untranslatable(self):
        with self.assertRaises(codegen.Unsupported):
            codegen.effect(0x1000, mfc0(8, 12) | 0x8)

    def test_unknown_cop0_rs_stays_untranslatable(self):
        with self.assertRaises(codegen.Unsupported):
            codegen.effect(0x1000, (0x10 << 26) | (0x08 << 21))

    def test_eret_is_control_and_exact(self):
        self.assertTrue(codegen.is_eret(ERET))
        self.assertFalse(codegen.is_eret(ERET | 0x1))
        self.assertTrue(codegen.is_control(ERET))

    def test_function_flow_terminates_at_eret(self):
        insns, _, _ = codegen.function_flow(
            FakeElf({0x1000: ERET, 0x1004: NOP}),
            0x1000, [(0x1000, 0x100C)], {0x1000},
        )
        self.assertIn(0x1000, insns)
        self.assertNotIn(0x1004, insns)

    def test_emit_lle_syscall_function(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: SYSCALL}),
                0x1000, [(0x1000, 0x1008)], {0x1000},
                lle_cpu=True,
            )
        )
        self.assertIn("sr_cpu_raise_exception(s, 8u", text)
        self.assertNotIn("sr_raw_syscall", text)

    def test_emit_mfc0_propagates_flow(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: mfc0(8, 12), 0x1004: JR_RA, 0x1008: NOP}),
                0x1000, [(0x1000, 0x100C)], {0x1000},
            )
        )
        self.assertIn("sr_cp0_mfc0(s, 8u, 12u, 0u", text)
        self.assertIn("if (s->flow_kind != 0u)", text)

    def test_emit_eret_terminates_function(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: ERET, 0x1004: NOP}),
                0x1000, [(0x1000, 0x100C)], {0x1000},
            )
        )
        self.assertIn("sr_cpu_eret(s, 0x00001000u)", text)
        self.assertIn("return;", text)

    def test_delay_slot_syscall_carries_branch_pc_in_lle(self):
        lines = codegen.delay_slot_lines(
            0x1004, SYSCALL, 0x1000, lle_cpu=True, resumable=False)
        text = "\n".join(lines)
        self.assertIn("0x00001004u, 0x00001000u, 0u, 1u, 0u", text)

    def test_delay_slot_hle_syscall_is_unchanged(self):
        lines = codegen.delay_slot_lines(0x1004, SYSCALL, 0x1000)
        self.assertEqual(len(lines), 1)
        self.assertIn("sr_raw_syscall", lines[0])

    def test_delay_slot_control_fails_closed(self):
        beq = 0x10000000 | (1 << 21) | (2 << 16) | 1
        with self.assertRaises(codegen.Unsupported):
            codegen.delay_slot_lines(0x1004, beq, 0x1000)

    def test_emit_eret_preserves_guest_sp(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: ERET, 0x1004: NOP}),
                0x1000, [(0x1000, 0x100C)], {0x1000},
                resumable=False,
            )
        )
        self.assertIn("sr_cpu_eret(s, 0x00001000u);", text)
        self.assertIn("sr_end(s, 0u, 0);\n    return;", text)
        self.assertNotIn("sr_end(s, 0u, 0);\n    s->r[29] = _sp_entry;", text)

    def test_delay_slot_exception_unwind_preserves_guest_sp(self):
        lines_sys = codegen.delay_slot_lines(
            0x1004, SYSCALL, 0x1000, lle_cpu=True, resumable=False)
        text_sys = "\n".join(lines_sys)
        self.assertNotIn("s->r[29] = _sp_entry", text_sys)
        self.assertIn("{ sr_end(s, 0u, 0); return; }", text_sys)

        lines_brk = codegen.delay_slot_lines(
            0x1004, BREAK, 0x1000, lle_cpu=True, resumable=False)
        text_brk = "\n".join(lines_brk)
        self.assertNotIn("s->r[29] = _sp_entry", text_brk)
        self.assertIn("{ sr_end(s, 0u, 0); return; }", text_brk)

        lines_mfc0 = codegen.delay_slot_lines(
            0x1004, mfc0(8, 12), 0x1000, lle_cpu=True, resumable=False)
        text_mfc0 = "\n".join(lines_mfc0)
        self.assertNotIn("s->r[29] = _sp_entry", text_mfc0)

    def test_delay_slot_cop0_saves_and_restores_context(self):
        lines = codegen.delay_slot_lines(
            0x1004, mfc0(8, 12), 0x1000, lle_cpu=True, resumable=False)
        text = "\n".join(lines)
        self.assertIn("_prev_npc = s->next_pc", text)
        self.assertIn("_prev_ids = s->in_delay_slot", text)
        self.assertIn("s->in_delay_slot = 1u; s->next_pc = 0x00001000u;", text)
        self.assertRegex(
            text,
            r"if \(sr_cp0_mfc0\(.*?\)\s*<\s*0\)\s*\{\s*s->in_delay_slot = _prev_ids;\s*s->next_pc = _prev_npc;\s*sr_end\(s, 0u, 0\);\s*return;\s*\}"
        )
        self.assertIn("s->in_delay_slot = _prev_ids; s->next_pc = _prev_npc; }", text)

    def test_direct_jal_caller_propagates_lle_flow(self):
        jal_target = 0x00001008
        jal_insn = 0x0C000000 | ((jal_target >> 2) & 0x03FFFFFF)
        text_lle = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: jal_insn, 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP}),
                0x1000, [(0x1000, 0x1008)], {0x1000, 0x1008},
                lle_cpu=True,
            )
        )
        self.assertIn("f_00001008(s);", text_lle)
        self.assertIn("if (s->flow_kind != 0u) { return; }", text_lle)

        text_default = "\n".join(
            codegen.emit_function(
                FakeElf({0x1000: jal_insn, 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP}),
                0x1000, [(0x1000, 0x1008)], {0x1000, 0x1008},
                lle_cpu=False,
            )
        )
        self.assertIn("f_00001008(s);", text_default)
        self.assertNotIn("flow_kind", text_default)

    def test_sr_register_all_enables_lle_runtime_when_lle_cpu(self):
        source = (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8")
        self.assertIn("if LLE_CPU:", source)
        self.assertIn("sr_cpu_lle_set_enabled(1);", source)

    def test_lle_cli_flag_drives_the_module_default(self):
        source = (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8")
        self.assertIn('"--lle-cpu"', source)
        previous = codegen.LLE_CPU
        try:
            codegen.LLE_CPU = True
            text = "\n".join(
                codegen.emit_function(
                    FakeElf({0x1000: SYSCALL}),
                    0x1000, [(0x1000, 0x1008)], {0x1000},
                )
            )
            self.assertIn("sr_cpu_raise_exception(s, 8u", text)
        finally:
            codegen.LLE_CPU = previous


class CpuLleSourcesTests(unittest.TestCase):
    """Source-owned COP0/exception implementation matches spec 3.2/3.3."""

    def test_header_defines_cop0_registers(self):
        text = CPU_LLE_H.read_text(encoding="utf-8")
        for name, value in (("SR_CP0_BADVADDR", 8), ("SR_CP0_COUNT", 9),
                            ("SR_CP0_COMPARE", 11), ("SR_CP0_STATUS", 12),
                            ("SR_CP0_CAUSE", 13), ("SR_CP0_EPC", 14),
                            ("SR_CP0_PRID", 15), ("SR_CP0_CONFIG", 16)):
            self.assertRegex(text, rf"{name}\s*=?\s*{value}u?")

    def test_header_defines_exceptions_and_flows(self):
        text = CPU_LLE_H.read_text(encoding="utf-8")
        for name, value in (("SR_EXC_INT", 0), ("SR_EXC_ADEL", 4),
                            ("SR_EXC_ADES", 5), ("SR_EXC_SYS", 8),
                            ("SR_EXC_BP", 9), ("SR_EXC_RI", 10),
                            ("SR_EXC_CPU", 11), ("SR_EXC_OV", 12),
                            ("SR_FLOW_NONE", 0), ("SR_FLOW_EXCEPTION", 1),
                            ("SR_FLOW_ERET", 2), ("SR_FLOW_INTERRUPT", 3),
                            ("SR_FLOW_FATAL", 4)):
            self.assertRegex(text, rf"{name}\s*=?\s*{value}u?")

    def test_header_declares_spec_helpers(self):
        text = CPU_LLE_H.read_text(encoding="utf-8")
        for proto in (
                r"sr_cp0_mfc0\s*\(",
                r"sr_cp0_mtc0\s*\(",
                r"sr_cpu_raise_exception\s*\(",
                r"sr_cpu_eret\s*\(",
                r"sr_cpu_poll\s*\(",
                r"sr_cpu_in_kernel\s*\(",
                r"sr_cpu_clear_flow\s*\("):
            self.assertRegex(text, proto)

    def test_raise_sets_epc_cause_exl_vector(self):
        text = CPU_LLE_C.read_text(encoding="utf-8")
        self.assertIn("SR_CP0_EPC", text)
        self.assertIn("SR_CAUSE_BD", text)
        self.assertIn("SR_STATUS_EXL", text)
        self.assertIn("SR_FLOW_EXCEPTION", text)
        self.assertIn("sr_exec_span_owns_fetch", text)
        self.assertNotIn("sr_guest_span_readable", text)

    def test_status_cause_writes_are_masked(self):
        text = CPU_LLE_C.read_text(encoding="utf-8")
        self.assertIn("SR_STATUS_WRITABLE_MASK", text)
        self.assertIn("SR_CAUSE_IP_SW_MASK", text)
        header = CPU_LLE_H.read_text(encoding="utf-8")
        mask_def = re.search(r"#define SR_STATUS_WRITABLE_MASK.*?\)", header, re.DOTALL)
        self.assertIsNotNone(mask_def)
        self.assertNotIn("SR_STATUS_ERL", mask_def.group(0))

    def test_interp_results_cover_exception_flows(self):
        text = GUEST_INTERP_H.read_text(encoding="utf-8")
        self.assertIn("SR_GUEST_INTERP_EXCEPTION", text)
        self.assertIn("SR_GUEST_INTERP_ERET", text)


class MakefileWiringTests(unittest.TestCase):
    """The new selftest is wired like the existing native selftests."""

    def test_cpu_lle_selftest_target_exists(self):
        self.assertIn("cpu-lle-selftest:", MAKEFILE)
        recipe = MAKEFILE.split("cpu-lle-selftest:", 1)[1].split("\n\n", 1)[0]
        self.assertIn("src/rt/cpu_lle_selftest.c", recipe)
        self.assertIn("src/rt/guest_interp.c", recipe)
        self.assertIn("$(BUILD_DIR)/cpu_lle_selftest.exe", recipe)

    def test_cpu_lle_selftest_is_phony(self):
        block = re.search(r"(?ms)^PUBLIC_TARGETS := \\\n(.*?)(?=^INTERNAL_TARGETS :=)", MAKEFILE)
        self.assertIsNotNone(block)
        self.assertIn("cpu-lle-selftest", block.group(1).split())
        self.assertIn(".PHONY: $(PUBLIC_TARGETS) $(INTERNAL_TARGETS)", MAKEFILE)

    def test_runtime_source_lists_carry_cpu_lle(self):
        for var in ("RT_SRCS", "PORTABLE_CORE_SRCS"):
            block = re.search(rf"^{var}\s*:?=\s*(.*?)(?=\n\S|\Z)",
                              MAKEFILE, re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(block, var)
            self.assertIn("src/rt/cpu_lle.c", block.group(1))

    def test_every_guest_interp_link_also_links_cpu_lle(self):
        offenders = []
        for number, raw in enumerate(MAKEFILE.splitlines(), start=1):
            if "src/rt/guest_interp.c" in raw and "-o " in raw:
                if "src/rt/cpu_lle.c" not in raw:
                    offenders.append(f"Makefile:{number}: {raw.strip()}")
        self.assertEqual(offenders, [])

    def test_every_python_harness_link_also_links_cpu_lle(self):
        # tools/*.py inline gcc commands that link guest_interp.c must also
        # link cpu_lle.c (same drift class as test_native_gate_stub_link).
        # Only quoted path operands count, so prose mentions in docstrings
        # are not mistaken for link surfaces; only files that actually build
        # a link command (an "-o" output operand) are surfaces at all, so
        # audit-only tests that quote the filename are excluded.
        # NOTE: .github/workflows/ci.yml carries two more such commands
        # (vfpu_interp_selftest + its ASan twin); agents may not edit
        # .github/, so that hunk is a maintainer followup, not asserted here.
        offenders = []
        for path in sorted((ROOT / "tools").glob("*.py")):
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"""["']guest_interp\.c["']""", text) and '"-o "' in text:
                if "cpu_lle.c" not in text:
                    offenders.append(path.name)
        self.assertEqual(offenders, [])

    def test_native_core_tests_depends_on_cpu_lle_selftest(self):
        self.assertRegex(MAKEFILE, r"(?m)^native-core-tests:\s*cpu-lle-selftest\b")


def _mem(op, base, rt, imm):
    return (op << 26) | (base << 21) | (rt << 16) | (imm & 0xFFFF)


LW = _mem(0x23, 13, 14, 4)
SW = _mem(0x2B, 13, 14, 4)
LB = _mem(0x20, 13, 14, 1)
LWL = _mem(0x22, 13, 14, 1)
LWR = _mem(0x26, 13, 14, 1)
SWL = _mem(0x2A, 13, 14, 1)
SWR = _mem(0x2E, 13, 14, 1)


def _vfpu(op, base=13, vt=0, off=0):
    return (op << 26) | (base << 21) | (vt << 16) | (off & 0xFFFF)


LVS = _vfpu(0x32, 13, 0, 0)
SVS = _vfpu(0x3A, 13, 0, 0)
LVQ = _vfpu(0x36, 13, 0, 0)
SVQ = _vfpu(0x3E, 13, 0, 0)
LVL = _vfpu(0x35, 13, 0, 0)
LVR = _vfpu(0x35, 13, 0, 2)
SVL = _vfpu(0x3D, 13, 0, 0)
SVR = _vfpu(0x3D, 13, 0, 2)


class LleDataAccessGuardTests(unittest.TestCase):
    """Loads and stores are address-checked under --lle-cpu (spec 3.3).

    Failing-before evidence: on the base tree LLE_ACCESS and
    _lle_access_stmt do not exist, and effect(..., lle_cpu=True) emits the
    same bare MEM_* access as the default lane, so a user-mode kernel-segment
    load is masked into RAM and silently succeeds.
    """

    def test_lw_is_guarded(self):
        stmt, saddr, ssize = codegen.effect(0x1000, LW, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _ea, 4u, 0, 0x00001000u", stmt)
        self.assertIn("MEM_R32(_ea)", stmt)
        self.assertIsNone(saddr)
        self.assertEqual(ssize, 0)

    def test_sw_is_guarded_and_keeps_store_metadata(self):
        stmt, saddr, ssize = codegen.effect(0x1000, SW, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _ea, 4u, 1, 0x00001000u", stmt)
        self.assertIn("MEM_W32_PC(_ea,", stmt)
        self.assertEqual(saddr, "(s->r[13] + 0x00000004u)")
        self.assertEqual(ssize, 4)

    def test_byte_access_is_guarded_at_width_one(self):
        # A byte has no alignment constraint, but the segment check still applies.
        stmt, _, _ = codegen.effect(0x1000, LB, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _ea, 1u, 0,", stmt)

    def test_guarded_access_leaves_the_body_on_fault(self):
        stmt, _, _ = codegen.effect(0x1000, LW, lle_cpu=True)
        self.assertIn("{ sr_end(s, 0u, 0); return; }", stmt)

    def test_effective_address_is_evaluated_once(self):
        stmt, _, _ = codegen.effect(0x1000, LW, lle_cpu=True)
        self.assertEqual(stmt.count("uint32_t _ea ="), 1)

    def test_unaligned_access_forms_are_never_guarded(self):
        # lwl/lwr/swl/swr exist to cross an alignment boundary; a misaligned
        # effective address is their normal case, not an address error.
        for name, word in (("lwl", LWL), ("lwr", LWR), ("swl", SWL), ("swr", SWR)):
            with self.subTest(form=name):
                lle, _, _ = codegen.effect(0x1000, word, lle_cpu=True)
                default, _, _ = codegen.effect(0x1000, word)
                self.assertNotIn("sr_cpu_guard_access", lle)
                self.assertEqual(lle, default)

    def test_delay_slot_access_carries_the_branch_pc(self):
        lines = codegen.delay_slot_lines(0x1004, LW, 0x1000, lle_cpu=True)
        body = "\n".join(lines)
        # instr_pc is the load, branch_pc is the branch, in_delay is set.
        self.assertIn("0x00001004u, 0x00001000u, 1u)", body)

    def test_default_lane_access_is_unchanged(self):
        for name, word in (("lw", LW), ("sw", SW), ("lb", LB)):
            with self.subTest(form=name):
                stmt, _, _ = codegen.effect(0x1000, word)
                self.assertNotIn("sr_cpu_guard_access", stmt)
                self.assertNotIn("_ea", stmt)

    def test_default_delay_slot_access_is_unchanged(self):
        lines = codegen.delay_slot_lines(0x1004, LW, 0x1000)
        self.assertNotIn("sr_cpu_guard_access", "\n".join(lines))

    def test_guard_helper_exists_in_the_runtime(self):
        header = CPU_LLE_H.read_text(encoding="utf-8")
        source = CPU_LLE_C.read_text(encoding="utf-8")
        self.assertIn("sr_cpu_guard_access", header)
        self.assertIn("sr_cpu_data_access_fault", header)
        self.assertIn("int sr_cpu_guard_access(", source)


class LleVfpuGuardTests(unittest.TestCase):
    """VFPU loads/stores carry the measured alignment guard under --lle-cpu.

    Failing-before evidence: vfpu_effect() ignored lle_cpu, so effect(...,
    lle_cpu=True) for lv.s/lv.q/sv.s/sv.q emitted the same bare access as the
    default lane with no sr_cpu_guard_access call.
    """

    def test_lvs_guarded_at_width_four(self):
        stmt, saddr, ssize = codegen.effect(0x1000, LVS, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _ea, 4u, 0, 0x00001000u", stmt)
        self.assertIn("MEM_R32(_ea)", stmt)
        self.assertIsNone(saddr)
        self.assertEqual(ssize, 0)

    def test_svs_guarded_at_width_four_store(self):
        stmt, saddr, ssize = codegen.effect(0x1000, SVS, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _ea, 4u, 1, 0x00001000u", stmt)
        self.assertIn("MEM_W32_PC(_ea,", stmt)
        self.assertEqual(ssize, 4)

    def test_lvq_guarded_at_width_sixteen(self):
        stmt, saddr, ssize = codegen.effect(0x1000, LVQ, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _a, 16u, 0, 0x00001000u", stmt)
        self.assertIsNone(saddr)
        self.assertEqual(ssize, 0)

    def test_svq_guarded_at_width_sixteen_store(self):
        stmt, saddr, ssize = codegen.effect(0x1000, SVQ, lle_cpu=True)
        self.assertIn("sr_cpu_guard_access(s, _a, 16u, 1, 0x00001000u", stmt)
        self.assertEqual(ssize, 16)

    def test_vfpu_left_right_never_guarded(self):
        # lvl/lvr/svl/svr bypass like lwl/lwr/swl/swr (width 0, PSP-A3-11).
        for name, word in (("lvl.q", LVL), ("lvr.q", LVR),
                           ("svl.q", SVL), ("svr.q", SVR)):
            with self.subTest(form=name):
                lle, _, _ = codegen.effect(0x1000, word, lle_cpu=True)
                default, _, _ = codegen.effect(0x1000, word)
                self.assertNotIn("sr_cpu_guard_access", lle)
                self.assertEqual(lle, default)

    def test_delay_slot_vfpu_carries_the_branch_pc(self):
        lines = codegen.delay_slot_lines(0x1004, LVQ, 0x1000, lle_cpu=True)
        body = "\n".join(lines)
        self.assertIn("sr_cpu_guard_access(s, _a, 16u, 0,", body)
        self.assertIn("0x00001004u, 0x00001000u, 1u)", body)

    def test_default_lane_vfpu_is_unchanged(self):
        for name, word in (("lv.s", LVS), ("sv.s", SVS),
                           ("lv.q", LVQ), ("sv.q", SVQ)):
            with self.subTest(form=name):
                stmt, _, _ = codegen.effect(0x1000, word)
                self.assertNotIn("sr_cpu_guard_access", stmt)


if __name__ == "__main__":
    unittest.main()
