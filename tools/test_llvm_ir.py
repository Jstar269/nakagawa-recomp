# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The structured LLVM IR builder (``tools/llvm_ir.py``, #330).

The builder rejects, at the offending call, what LLVM's verifier would reject in
the printed module: mismatched operand types, a non-i1 branch condition, a call
that does not match its callee, an instruction after a terminator, a block with
no terminator, and a branch into another function. Linking merges modules the
way the chunk writer needs (a definition replaces a declaration; two definitions
of a name are an error) and renaming follows every use. When LLVM's ``opt`` is
installed, a module that uses every instruction form is also run through
``opt -passes=verify``; without it that one test reports SKIP.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import llvm_ir as L  # noqa: E402

VOID_PTR = L.FunctionType(L.void, (L.ptr,))


def llvm_tool(name: str) -> str | None:
    """An LLVM tool from PATH, or from MSYS2 UCRT64 on Windows (docs/SETUP.md)."""
    found = shutil.which(name)
    if found:
        return found
    if os.name == "nt":
        candidate = Path(os.environ.get("MSYS2_ROOT", "C:/msys64")) / "ucrt64" / "bin" / f"{name}.exe"
        if candidate.is_file():
            return str(candidate)
    return None


def verify(text: str) -> subprocess.CompletedProcess:
    opt = llvm_tool("opt")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "module.ll"
        path.write_text(text, encoding="ascii", newline="\n")
        return subprocess.run([opt, "-passes=verify", "-disable-output", str(path)],
                              capture_output=True, text=True, check=False)


def new_function(module=None, name="f"):
    module = module or L.Module("m")
    function = module.add_function(name, VOID_PTR, ["s"], ["nounwind"])
    builder = L.Builder()
    builder.position_at_end(function.append_basic_block("entry"))
    return module, function, builder


class BuilderTypeTests(unittest.TestCase):
    def test_binary_operands_must_share_an_integer_type(self):
        _, _, b = new_function()
        with self.assertRaises(TypeError):
            b.add(L.const(L.i32, 1), L.const(L.i64, 1))

    def test_branch_condition_must_be_i1(self):
        _, f, b = new_function()
        other = f.append_basic_block("other")
        with self.assertRaises(TypeError):
            b.cond_br(L.const(L.i32, 1), other, other)

    def test_call_must_match_the_callee_type(self):
        m, _, b = new_function()
        callee = m.get_or_declare("g", L.FunctionType(L.i32, (L.i32,)))
        with self.assertRaises(TypeError):
            b.call(callee, [])
        with self.assertRaises(TypeError):
            b.call(callee, [L.const(L.i64, 0)])

    def test_redeclaration_with_another_type_is_refused(self):
        m, _, _ = new_function()
        m.get_or_declare("g", L.FunctionType(L.i32, (L.i32,)))
        with self.assertRaises(TypeError):
            m.get_or_declare("g", L.FunctionType(L.void, (L.i32,)))

    def test_nothing_follows_a_terminator(self):
        _, _, b = new_function()
        b.ret_void()
        with self.assertRaises(ValueError):
            b.add(L.const(L.i32, 1), L.const(L.i32, 2))

    def test_every_block_needs_a_terminator(self):
        m, f, b = new_function()
        f.append_basic_block("open")
        b.ret_void()
        with self.assertRaises(ValueError):
            m.render()

    def test_a_branch_cannot_leave_its_function(self):
        m, f, b = new_function()
        _, g, _ = new_function(m, "g")
        with self.assertRaises(ValueError):
            b.br(g.blocks[0])

    def test_casts_check_their_direction(self):
        _, _, b = new_function()
        with self.assertRaises(TypeError):
            b.zext(L.const(L.i32, 1), L.i8)
        with self.assertRaises(TypeError):
            b.trunc(L.const(L.i8, 1), L.i32)

    def test_constants_print_as_signed_bit_patterns(self):
        self.assertEqual(L.const(L.i32, 0xFFFFFFFF).ref(), "-1")
        self.assertEqual(L.const(L.i32, 0x80000000).ref(), "-2147483648")
        self.assertEqual(L.const(L.i32, -1).value, 0xFFFFFFFF)
        self.assertEqual(L.const(L.i1, 1).ref(), "true")
        self.assertEqual(L.null().ref(), "null")


class ModuleTests(unittest.TestCase):
    def test_position_before_inserts_ahead_of_an_instruction(self):
        m, f, b = new_function()
        g = m.get_or_declare("g", L.FunctionType(L.void, ()))
        h = m.get_or_declare("h", L.FunctionType(L.void, ()))
        second = b.call(h, [])
        b.ret_void()
        b.position_before(second)
        b.call(g, [])
        lines = m.render().splitlines()
        self.assertLess(lines.index("  call void @g()"), lines.index("  call void @h()"))

    def test_link_in_replaces_a_declaration_with_the_definition(self):
        caller, _, b = new_function(L.Module("a"), "caller")
        b.call(caller.get_or_declare("callee", VOID_PTR), [caller.functions["caller"].params[0]])
        b.ret_void()
        callee, _, b2 = new_function(L.Module("b"), "callee")
        b2.ret_void()
        chunk = L.Module("chunk")
        chunk.link_in(caller)
        chunk.link_in(callee)
        text = chunk.render()
        self.assertIn("define void @callee(ptr %s)", text)
        self.assertNotIn("declare void @callee", text)

    def test_link_in_refuses_two_definitions(self):
        one, _, b = new_function(L.Module("a"), "f")
        b.ret_void()
        two, _, b2 = new_function(L.Module("b"), "f")
        b2.ret_void()
        chunk = L.Module("chunk")
        chunk.link_in(one)
        with self.assertRaises(ValueError):
            chunk.link_in(two)

    def test_rename_follows_every_use(self):
        m, f, b = new_function(name="f_08900000")
        b.call(m.get_or_declare("f_08900100", VOID_PTR), [f.params[0]])
        b.ret_void()
        chunk = L.Module("chunk")
        chunk.link_in(m)
        chunk.rename_symbols({"f_08900100": "f_08900100_named"})
        text = chunk.render()
        self.assertIn("call void @f_08900100_named(ptr %s)", text)
        self.assertIn("declare void @f_08900100_named(ptr)", text)


class VerifierTests(unittest.TestCase):
    @unittest.skipIf(llvm_tool("opt") is None, "SKIP: LLVM opt is not installed")
    def test_a_module_using_every_form_passes_the_verifier(self):
        m, f, b = new_function()
        s = f.params[0]
        helper = m.get_or_declare("helper", L.FunctionType(L.i32, (L.i32, L.ptr)))
        p = b.byte_gep(s, 128)
        x = b.load(L.i32, p, 4)
        y = b.call(helper, [x, f])
        z = b.select(b.icmp("slt", x, y), x, y)
        wide = b.mul(b.sext(z, L.i64), b.zext(y, L.i64))
        b.store(b.trunc(b.lshr(wide, L.const(L.i64, 32)), L.i32), p, 4)
        then, join = f.append_basic_block("then"), f.append_basic_block("join")
        b.cond_br(b.icmp("eq", x, L.const(L.i32, 0)), then, join)
        b.position_at_end(then)
        b.store(b.sdiv(x, L.const(L.i32, 3)), p, 4)
        b.br(join)
        b.position_at_end(join)
        phi = b.phi(L.i32)
        phi.add_incoming(x, f.blocks[0])
        phi.add_incoming(L.const(L.i32, 7), then)
        b.store(phi, p, 4)
        b.ret_void()
        result = verify(m.render())
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipIf(llvm_tool("opt") is None, "SKIP: LLVM opt is not installed")
    def test_the_verifier_rejects_what_the_builder_cannot_see(self):
        # Use before definition across blocks is the verifier's to catch (dominance);
        # proving opt rejects it keeps the test above from passing vacuously.
        m, f, b = new_function()
        late = f.append_basic_block("late")
        b.br(late)
        b.position_at_end(late)
        x = b.load(L.i32, f.params[0], 4)
        b.ret_void()
        early = f.blocks[0]
        early.instructions.insert(0, L.Instruction(
            early, L.i32, lambda: f"add i32 {x.ref()}, 1", name="early"))
        result = verify(m.render())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dominate", result.stderr)


if __name__ == "__main__":
    unittest.main()
