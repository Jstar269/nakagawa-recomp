# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The backend-neutral lowering plan (``tools/lowering_plan.py``, #330).

The LLVM backend renders only the plan; the C emitter still makes the same
decisions itself. Until ``emit_function`` consumes the plan, the drift guard here
holds the two together: for every function the LLVM backend lowers in a real
codegen run over the cosimulation's source-owned guest, the plan's native labels,
continuation exits, checked CALLs, dispatched CALLs, TAIL records, returns and
preemption points must equal what ``emit_function`` wrote in C for that function.
A transfer rule changed on one side only fails here. The decoder tests pin a few
operand orders a renderer depends on.
"""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen  # noqa: E402
import codegen_llvm  # noqa: E402
import lowering_plan as P  # noqa: E402

BASE = 0x08900000


def plan_signature(plan: P.FunctionPlan) -> dict:
    labels, calls, dispatches, tails, yields, returns = set(), [], [], [], [], 0
    for step in plan.steps:
        if isinstance(step, P.Label):
            labels.add(step.addr)
        if not isinstance(step, P.Transfer):
            continue
        t, resume = step, (step.addr + 8) & 0xFFFFFFFF
        if t.kind == "call":
            if t.symbol is not None:
                calls.append((t.addr, t.symbol))
            else:
                dispatches.append((t.target, resume))
        elif t.kind == "call-computed":
            dispatches.append((None, resume))
        elif t.kind == "tail-computed":
            tails.append((None, None))
        elif t.kind == "return":
            returns += t.target_reg == 31
        elif t.kind == "jump-tail" or t.kind == "branch-tail":
            tails.append((t.target, t.symbol))
        if t.kind in ("jump-self", "jump-label", "branch-label") and t.backward:
            yields.append(t.addr)
    tails += [(e.target, e.symbol) for e in plan.exits]
    return {"labels": sorted(labels), "continuations": sorted(e.target for e in plan.exits),
            "calls": sorted(calls), "dispatches": Counter(dispatches), "tails": Counter(tails),
            "returns": returns, "yields": sorted(yields)}


def _addr(text: str) -> int:
    return int(text.rstrip("u"), 16)


def c_signature(text: str) -> dict:
    """The same facts, read from emit_function's C for one function."""
    # Each TAIL record, with a `_t` or `s->pc` target bound to the literal it was loaded from.
    tails = []
    for match in re.finditer(r"(?:uint32_t _t = (0x[0-9a-f]{8}u); )?(?:s->pc = (0x[0-9a-f]{8}u); )?"
                             r"sr_tail_record\(s, ([^,]+), ([^,]+),", text):
        loaded_t, loaded_pc, target, fn = match.groups()
        fn = None if fn == "NULL" else fn
        if target.startswith("0x"):
            tails.append((_addr(target), fn))
        elif target == "_t" and loaded_t:
            tails.append((_addr(loaded_t), fn))
        elif target == "s->pc" and loaded_pc:
            tails.append((_addr(loaded_pc), fn))
        else:
            tails.append((None, fn))
    calls = [(_addr(pc), sym) for pc, sym in re.findall(
        r"sr_aot_return_boundary_begin\(s, 0x[0-9a-f]{8}u, (0x[0-9a-f]{8}u), 1u\)\) return; "
        r"(\w+)\(s\);", text)]
    dispatches = [(None if target == "_t" else _addr(target), _addr(resume)) for target, resume in
                  re.findall(r"dispatch_call\(s, (0x[0-9a-f]{8}u|_t), (0x[0-9a-f]{8}u)\)", text)]
    yields = [_addr(pc) for pc in re.findall(r"SR_YIELD\(s, (0x[0-9a-f]{8}u)\); goto", text)]
    return {"labels": sorted(int(a, 16) for a in re.findall(r"^  L_([0-9a-f]{8}): ;$", text, re.M)),
            "continuations": sorted(int(a, 16) for a in re.findall(r"^  _sr_cont_([0-9a-f]{8}): ;$",
                                                                   text, re.M)),
            "calls": sorted(calls), "dispatches": Counter(dispatches), "tails": Counter(tails),
            "returns": text.count("SR_AOT_RETURN_CAPTURE(s,"), "yields": sorted(yields)}


class DriftGuardTests(unittest.TestCase):
    """The plan and emit_function agree on every lowered function of a real build."""

    def _run(self, *options):
        captured = []
        real = codegen_llvm.lower

        def recording(cg, elf, start, ranges, known, *, c_text, **kwargs):
            plan_kwargs = {k: kwargs.get(k) for k in ("resume_owners", "resumable", "profile",
                                                       "dispatch_boundaries")}
            plan = P.plan_function(cg, elf, start, ranges, known, **plan_kwargs)
            body = real(cg, elf, start, ranges, known, c_text=c_text, **kwargs)
            captured.append((start, c_text, plan))
            return body

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run([sys.executable, str(ROOT / "fixtures" / "cosim" / "generate.py"),
                            "generate", "--out-dir", str(root / "fx")], check=True,
                           capture_output=True)
            codegen_llvm.lower = recording
            try:
                rc = codegen.main(["codegen.py", str(root / "fx" / "guest.prx"),
                                   str(root / "g_recomp.c"), f"--base=0x{BASE:08x}",
                                   "--backend=llvm", *options])
            finally:
                codegen_llvm.lower = real
        self.assertEqual(rc, 0)
        return captured

    def _check(self, captured):
        self.assertGreaterEqual(len(captured), 20)
        kinds = Counter()
        for start, c_text, plan in captured:
            with self.subTest(function=f"0x{start:08x}"):
                self.assertEqual(plan_signature(plan), c_signature(c_text))
            kinds.update(step.kind for step in plan.steps if isinstance(step, P.Transfer))
        # The corpus exercises every transfer kind but a self-jump, an in-body `j` and a
        # branch out of the body; the unit tests below cover those.
        self.assertLessEqual({"call", "call-computed", "tail-computed", "return", "jump-tail",
                              "branch-label"}, set(kinds))

    def test_plans_match_emit_function(self):
        self._check(self._run("--funcs-per-chunk=1"))

    def test_plans_match_emit_function_under_the_stack_census(self):
        self._check(self._run("--funcs-per-chunk=1", "--stack-census"))


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


NOP, JR_RA = 0x00000000, 0x03E00008


def _plan_and_c(words, start=0x1000, known=None):
    elf, ranges, known = FakeElf(words), [(start, start + 0x100)], known or {start}
    plan = P.plan_function(codegen, elf, start, ranges, known)
    c_text = "\n".join(codegen.emit_function(elf, start, ranges, known))
    return plan, c_text


class TransferRuleTests(unittest.TestCase):
    def test_self_jump_inner_jump_and_branch_out_match_the_c_emitter(self):
        j = lambda target: 0x02 << 26 | (target >> 2)            # noqa: E731
        beq_far = 0x04 << 26 | 4 << 21 | 5 << 16 | 0x0100        # beq r4, r5, +0x400: out of body
        for words in (
            {0x1000: j(0x1000), 0x1004: NOP},                      # spin wait
            {0x1000: NOP, 0x1004: j(0x1000), 0x1008: NOP},         # backward in-body jump
            {0x1000: beq_far, 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP},
        ):
            plan, c_text = _plan_and_c(words)
            self.assertEqual(plan_signature(plan), c_signature(c_text))

    def test_operand_order_of_register_shifts_and_moves(self):
        srav = 5 << 21 | 6 << 16 | 7 << 11 | 0x07                 # srav r7, r6, r5
        movz = 5 << 21 | 6 << 16 | 7 << 11 | 0x0A                 # movz r7, r5, r6
        lwl = 0x22 << 26 | 5 << 21 | 6 << 16 | 0xFFFC             # lwl r6, -4(r5)
        self.assertEqual(P.decode_op(0, srav), P.Op(0, srav, "ashrv", 7, (6, 5)))
        self.assertEqual(P.decode_op(0, movz), P.Op(0, movz, "movz", 7, (6, 5)))
        self.assertEqual(P.decode_op(0, lwl), P.Op(0, lwl, "lwl", 6, (6, 5), 0xFFFFFFFC))

    def test_forms_outside_the_plan_are_refused_with_a_reason(self):
        with self.assertRaisesRegex(P.Unsupported, "FPU at 0x00000010"):
            P.decode_op(0x10, 0x46000002)                          # mul.s
        with self.assertRaisesRegex(P.Unsupported, "COP0 at"):
            P.decode_op(0x10, 0x40026000)                          # mfc0
        with self.assertRaisesRegex(P.Unsupported, "INS with msb below lsb"):
            P.decode_op(0x10, 0x1F << 26 | 1 << 11 | 4 << 6 | 0x04)


if __name__ == "__main__":
    unittest.main()
