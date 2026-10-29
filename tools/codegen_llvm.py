# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The opt-in LLVM IR backend for generated guest functions (#330).

``codegen.py --backend=llvm`` lowers each guest function this module supports to
LLVM IR instead of C; every other function keeps its C body, and the reason is
recorded (``<game>_recomp_llvm.json``), never silently dropped. The C backend stays
the default. docs/LLVM_BACKEND.md is the design.

What the lowering consumes
--------------------------
It decodes no MIPS. It renders the function's backend-neutral lowering plan
(``tools/lowering_plan.py``): decoded operations with their operands, and every
transfer with its kind, target, link, condition and delay slot resolved from the
C emitter's own ownership and transfer rules. A test holds the plan to what
``emit_function`` emits for the same function until the C emitter renders the
plan too. A function reaches this backend only after the C emitter translated it,
so an encoding the C emitter refuses stays its fail-closed stub.

What it states explicitly in IR, instead of leaving it to a C compiler:

* guest register state lives in ``CpuState`` (the recomp.h ABI, version checked by
  ``layout_asserts``): every guest read is a load and every guest write a store at
  the field's offset, in program order, so nothing is cached across an escape and
  no promoted value needs a spill or flush;
* every escape to the runtime is a call with ``s``: trace hooks, the preemption
  point, memory helpers, calls, returns, tail transfers and dispatch;
* delay slots are emitted at their owning transfer, with the computed target
  latched before the slot (PR #126) and the branch condition evaluated before it;
* memory goes through the existing access helpers (llvm_abi.h), never a host
  pointer.

Anything outside the prototype subset raises ``LLVMUnsupported`` with a reason,
and the function falls back to C.
"""

from __future__ import annotations

import json

import llvm_ir as L
import lowering_plan as P

#: Identity of the lowering contract, recorded in the manifest and the build profile.
BACKEND_ID = "nakagawa-llvm-ir-1"

#: CpuState byte offsets the IR addresses (src/rt/recomp.h, SR_CPUSTATE_ABI_VERSION 3).
#: ``layout_asserts`` turns each one into a _Static_assert compiled with the build.
CPUSTATE_ABI_VERSION = 3
CPUSTATE_OFFSETS = {
    "r": 0,
    "hi": 128,
    "lo": 132,
    "pc": 136,
    "llbit": 996,
}

_FN = L.FunctionType
#: The runtime functions generated IR calls, with their C types (llvm_abi.h and recomp.h).
ABI_FUNCTIONS = {
    "sr_llvm_begin": _FN(L.void, (L.ptr, L.i32, L.i32)),
    "sr_llvm_end": _FN(L.void, (L.ptr, L.i32, L.i32)),
    "sr_llvm_yield": _FN(L.void, (L.ptr, L.i32)),
    "sr_llvm_host_stack_check": _FN(L.void, (L.i32, L.ptr)),
    "sr_llvm_r8": _FN(L.i32, (L.i32,)),
    "sr_llvm_r16": _FN(L.i32, (L.i32,)),
    "sr_llvm_r32": _FN(L.i32, (L.i32,)),
    "sr_llvm_w8_pc": _FN(L.void, (L.i32, L.i32, L.i32)),
    "sr_llvm_w16_pc": _FN(L.void, (L.i32, L.i32, L.i32)),
    "sr_llvm_w32_pc": _FN(L.void, (L.i32, L.i32, L.i32)),
    "sr_llvm_return_capture": _FN(L.void, (L.ptr, L.i32)),
    "sr_llvm_tail_record": _FN(L.void, (L.ptr, L.i32, L.ptr, L.i32, L.i32)),
    "sr_llvm_tail_drain": _FN(L.void, (L.ptr,)),
    "sr_aot_return_boundary_begin": _FN(L.i32, (L.ptr, L.i32, L.i32, L.i32)),
    "sr_aot_return_boundary_end": _FN(L.i32, (L.ptr,)),
    "dispatch_call": _FN(L.void, (L.ptr, L.i32, L.i32)),
    "sr_lwl": _FN(L.i32, (L.i32, L.i32)),
    "sr_lwr": _FN(L.i32, (L.i32, L.i32)),
    "sr_swl_pc": _FN(L.void, (L.i32, L.i32, L.i32)),
    "sr_swr_pc": _FN(L.void, (L.i32, L.i32, L.i32)),
    "sr_raw_syscall": _FN(L.void, (L.ptr, L.i32, L.i32)),
    "sr_break": _FN(L.void, (L.ptr, L.i32, L.i32)),
    "sr_bitrev": _FN(L.i32, (L.i32,)),
}

#: LLVM intrinsics the lowering uses (defined for every input it passes them).
INTRINSICS = {
    "llvm.ctlz.i32": _FN(L.i32, (L.i32, L.i1)),
    "llvm.bswap.i32": _FN(L.i32, (L.i32,)),
    "llvm.frameaddress.p0": _FN(L.ptr, (L.i32,)),
}

#: The function type of every generated body: void f(CpuState *s).
BODY_TYPE = _FN(L.void, (L.ptr,))
#: Function attributes Clang gives a C function on the supported hosts: C code never
#: unwinds, and the bodies keep unwind tables for stack walks and longjmp (funcdiff.c).
BODY_ATTRIBUTES = ("nounwind", "uwtable")


#: A function this backend does not lower; it keeps its C body, with the reason recorded.
LLVMUnsupported = P.Unsupported
hst_emission_sites = P.hst_emission_sites


def layout_asserts() -> list[str]:
    """C _Static_asserts for every CpuState offset the IR uses."""
    lines = [f"_Static_assert(SR_CPUSTATE_ABI_VERSION == {CPUSTATE_ABI_VERSION}u, "
             '"LLVM backend: CpuState ABI version");']
    for field, offset in CPUSTATE_OFFSETS.items():
        lines.append(f"_Static_assert(offsetof(CpuState, {field}) == {offset}u, "
                     f'"LLVM backend: CpuState {field} offset");')
    return lines


class LLVMBody(str):
    """The C-side text of a function whose body is in LLVM IR.

    The chunk and registration machinery treat it like any other function text
    (it declares the symbol and names it); ``module`` holds the lowered body,
    which the chunk writer links into that chunk's ``.ll`` file. ``chunk_bytes`` is
    the length of the C body it replaces: size-bounded chunking weighs the function
    by that, so both backends cut chunks at the same functions.
    """

    module: L.Module
    function: L.Function
    address: int
    chunk_bytes: int

    @classmethod
    def make(cls, symbol: str, address: int, module: L.Module, function: L.Function,
             chunk_bytes: int):
        text = cls(f"void {symbol}(CpuState *s);  /* body in LLVM IR (#330) */")
        text.module = module
        text.function = function
        text.address = address
        text.chunk_bytes = chunk_bytes
        return text

    def with_text(self, text: str) -> "LLVMBody":
        other = LLVMBody(text)
        other.module, other.function, other.address = self.module, self.function, self.address
        other.chunk_bytes = self.chunk_bytes
        return other


def census_body_symbol(addr: int) -> str:
    """The wrapped body name codegen.wrap_stack_census_functions gives a callable."""
    return f"_sr_stack_census_body_{addr:08x}"


_BINARY = {"add": "add", "sub": "sub", "and": "and_", "or": "or_", "xor": "xor"}
_COND = {"eq": "eq", "ne": "ne", "lez": "sle", "gtz": "sgt", "ltz": "slt", "gez": "sge"}


class FunctionLowering:
    """Render one function's lowering plan (tools/lowering_plan.py) as LLVM IR.

    The plan holds every decision; this class only spells it. The order in which it
    reads guest state, calls the runtime and writes results is the order of
    ``codegen.emit_function`` and ``codegen.effect`` for the same plan step.
    """

    def __init__(self, plan: P.FunctionPlan, stack_census: bool):
        self.plan = plan
        self.start = plan.start
        self.resumable = plan.resumable
        self.stack_census = stack_census
        self.symbol = plan.symbol
        # Under --stack-census a callable's body is wrapped (codegen.wrap_stack_census_functions):
        # the IR defines the wrapped body and every call still names the wrapper.
        self.definition_name = (census_body_symbol(plan.start)
                                if stack_census and not plan.resume_entry else plan.symbol)
        self.module = L.Module(self.symbol)
        self.builder = L.Builder()
        self.function = None
        self.blocks: dict[str, L.BasicBlock] = {}
        self.sp_entry = None
        self.direct_calls = False
        self._dead = 0

    # -- IR plumbing -------------------------------------------------------
    @property
    def s(self):
        return self.function.params[0]

    def _decl(self, name):
        fn_type = ABI_FUNCTIONS.get(name) or INTRINSICS[name]
        return self.module.get_or_declare(name, fn_type)

    def _body_decl(self, symbol):
        return self.module.get_or_declare(symbol, BODY_TYPE, attributes=())

    def c32(self, value):
        return L.const(L.i32, value)

    def call(self, name, *args):
        return self.builder.call(self._decl(name), list(args))

    def label(self, name):
        block = self.blocks.get(name)
        if block is None:
            block = self.function.append_basic_block(name)
            self.blocks[name] = block
        return block

    def at_label(self, addr):
        return self.label(f"L_{addr:08x}")

    def enter(self, block):
        """Fall into ``block`` from the current position (C: straight-line flow)."""
        current = self.builder.block
        if current is not None and current.terminator is None:
            self.builder.br(block)
        self.builder.position_at_end(block)

    def dead(self):
        """Continue after a terminator: C code after `return`/`goto` until a label."""
        self._dead += 1
        self.builder.position_at_end(self.function.append_basic_block(f"after.{self._dead}"))

    def goto(self, block):
        self.builder.br(block)
        self.dead()

    def split(self, cond, hint):
        """Branch on ``cond``; returns (taken block, join block), positioned at taken."""
        self._dead += 1
        taken = self.function.append_basic_block(f"{hint}.{self._dead}")
        join = self.function.append_basic_block(f"{hint}.{self._dead}.join")
        self.builder.cond_br(cond, taken, join)
        self.builder.position_at_end(taken)
        return taken, join

    def ret(self):
        self.builder.ret_void()
        self.dead()

    # -- guest state ---------------------------------------------------------
    def field(self, name, index=0):
        return self.builder.byte_gep(self.s, CPUSTATE_OFFSETS[name] + 4 * index,
                                     hint=f"{name}{index}.p" if name == "r" else f"{name}.p")

    def R(self, i):
        if i == 0:
            return self.c32(0)
        return self.builder.load(L.i32, self.field("r", i), 4, hint=f"r{i}.")

    def W(self, i, value):
        if i != 0:                         # writes to r0 are dropped (ARCHITECTURE section 4)
            self.builder.store(value, self.field("r", i), 4)

    def load_field(self, name):
        return self.builder.load(L.i32, self.field(name), 4, hint=f"{name}.")

    def store_field(self, name, value):
        self.builder.store(value, self.field(name), 4)

    # -- escapes -------------------------------------------------------------
    def begin(self, pc, word):
        self.call("sr_llvm_begin", self.s, self.c32(pc), self.c32(word))

    def end(self, addr=None, size=0):
        size = size if isinstance(size, L.Value) else self.c32(size)
        self.call("sr_llvm_end", self.s, addr if addr is not None else self.c32(0), size)

    def yield_(self, pc):
        self.call("sr_llvm_yield", self.s, self.c32(pc))

    def host_return(self):
        """codegen.emit_host_return."""
        if not (self.resumable or self.stack_census):
            self.W(29, self.sp_entry)
        self.ret()

    def tail_transfer(self, target, symbol):
        """codegen.emit_tail_transfer: record the TAIL transfer and leave."""
        fn = self._body_decl(symbol) if symbol else L.null()
        if self.resumable or self.stack_census:
            self.call("sr_llvm_tail_record", self.s, target, fn, self.c32(0), self.c32(0))
        else:
            self.call("sr_llvm_tail_record", self.s, target, fn, self.sp_entry, self.c32(1))
        self.ret()

    def checked_body_call(self, symbol, call_pc):
        """codegen.emit_checked_body_call: a direct CALL with its return boundary."""
        b = self.builder
        resume = (call_pc + 8) & 0xFFFFFFFF
        refused = self.call("sr_aot_return_boundary_begin", self.s, self.c32(resume),
                            self.c32(call_pc), self.c32(1))
        _, join = self.split(b.icmp("ne", refused, self.c32(0)), "boundary.refused")
        self.ret()
        self.enter(join)
        b.call(self._body_decl(symbol), [self.s])
        self.call("sr_llvm_tail_drain", self.s)
        self.direct_calls = True
        mismatch = self.call("sr_aot_return_boundary_end", self.s)
        _, join = self.split(b.icmp("ne", mismatch, self.c32(0)), "boundary.mismatch")
        self.ret()
        self.enter(join)

    # -- operations ------------------------------------------------------------
    def instruction(self, op: P.Op):
        """codegen.normal_line: the trace pair around one non-control word."""
        self.begin(op.addr, op.word)
        traced = self.effect(op)
        if traced is not None:
            self.end(*traced)

    def effect(self, op: P.Op):
        """One Op, as codegen.effect emits it. Returns sr_end's (address, size), or None
        when the operation closes its own trace record or leaves the body."""
        b, k, d, src = self.builder, op.kind, op.dst, op.src
        if k in _BINARY:
            self.W(d, getattr(b, _BINARY[k])(self.R(src[0]), self.R(src[1])))
        elif k == "nor":
            self.W(d, b.xor(b.or_(self.R(src[0]), self.R(src[1])), self.c32(0xFFFFFFFF)))
        elif k in ("slt", "sltu"):
            cmp = b.icmp("slt" if k == "slt" else "ult", self.R(src[0]), self.R(src[1]))
            self.W(d, b.zext(cmp, L.i32))
        elif k in ("max", "min"):                              # Allegrex max / min
            x, y = self.R(src[0]), self.R(src[1])
            self.W(d, b.select(b.icmp("sgt" if k == "max" else "slt", x, y), x, y))
        elif k in ("shl", "lshr", "ashr"):
            self.W(d, getattr(b, k)(self.R(src[0]), self.c32(op.imm)))
        elif k == "rotr":                                      # Allegrex ROTR
            x = self.R(src[0])
            self.W(d, x if op.imm == 0 else
                   b.or_(b.lshr(x, self.c32(op.imm)), b.shl(x, self.c32(32 - op.imm))))
        elif k in ("shlv", "lshrv", "ashrv", "rotrv"):         # the amount is read first
            amount = b.and_(self.R(src[1]), self.c32(31))
            x = self.R(src[0])
            if k == "shlv":
                self.W(d, b.shl(x, amount))
            elif k == "ashrv":
                self.W(d, b.ashr(x, amount))
            elif k == "rotrv":                                 # Allegrex ROTRV
                back = b.and_(b.sub(self.c32(32), amount), self.c32(31))
                self.W(d, b.or_(b.lshr(x, amount), b.shl(x, back)))
            else:
                self.W(d, b.lshr(x, amount))
        elif k in ("movz", "movn"):
            moved = b.icmp("eq" if k == "movz" else "ne", self.R(src[0]), self.c32(0))
            if d != 0:
                _, join = self.split(moved, "mov")
                self.W(d, self.R(src[1]))
                self.enter(join)
        elif k == "syscall":                                   # HLE boundary: leaves the body
            self.call("sr_raw_syscall", self.s, self.c32(op.imm), self.c32(op.addr))
            self.ret()
            return None
        elif k == "break":
            self.call("sr_break", self.s, self.c32(op.imm), self.c32(op.addr))
        elif k == "nop":                                       # sync, cache
            pass
        elif k in ("mfhi", "mflo"):
            self.W(d, self.load_field("hi" if k == "mfhi" else "lo"))
        elif k in ("mthi", "mtlo"):
            self.store_field("hi" if k == "mthi" else "lo", self.R(src[0]))
        elif k == "clz":                                       # ctlz is defined at 0
            self.W(d, self.call("llvm.ctlz.i32", self.R(src[0]), L.const(L.i1, 0)))
        elif k == "clo":
            inverted = b.xor(self.R(src[0]), self.c32(0xFFFFFFFF))
            self.W(d, self.call("llvm.ctlz.i32", inverted, L.const(L.i1, 0)))
        elif k in ("mult", "multu"):
            ext = b.sext if k == "mult" else b.zext
            self._store_hilo(b.mul(ext(self.R(src[0]), L.i64), ext(self.R(src[1]), L.i64)))
        elif k == "div":
            self._div_signed(*src)
        elif k == "divu":
            self._div_unsigned(*src)
        elif k in ("madd", "maddu", "msub", "msubu"):
            ext = b.sext if k in ("madd", "msub") else b.zext
            product = b.mul(ext(self.R(src[0]), L.i64), ext(self.R(src[1]), L.i64))
            acc = b.or_(b.shl(b.zext(self.load_field("hi"), L.i64), L.const(L.i64, 32)),
                        b.zext(self.load_field("lo"), L.i64))
            self._store_hilo(b.add(acc, product) if k in ("madd", "maddu") else b.sub(acc, product))
        elif k == "addi":
            self.W(d, b.add(self.R(src[0]), self.c32(op.imm)))
        elif k in ("slti", "sltiu"):
            cmp = b.icmp("slt" if k == "slti" else "ult", self.R(src[0]), self.c32(op.imm))
            self.W(d, b.zext(cmp, L.i32))
        elif k in ("andi", "ori", "xori"):
            self.W(d, getattr(b, _BINARY[k[:-1]])(self.R(src[0]), self.c32(op.imm)))
        elif k == "const":                                     # lui
            self.W(d, self.c32(op.imm))
        elif k == "ext":
            self.W(d, b.and_(b.lshr(self.R(src[0]), self.c32(op.imm)), self.c32(op.aux)))
        elif k == "ins":
            kept = b.and_(self.R(src[0]), self.c32(~op.aux & 0xFFFFFFFF))
            inserted = b.and_(b.shl(self.R(src[1]), self.c32(op.imm)), self.c32(op.aux))
            self.W(d, b.or_(kept, inserted))
        elif k in ("wsbh", "wsbw", "seb", "seh", "bitrev"):
            x = self.R(src[0])
            if k == "wsbh":
                self.W(d, b.or_(b.shl(b.and_(x, self.c32(0x00FF00FF)), self.c32(8)),
                                b.and_(b.lshr(x, self.c32(8)), self.c32(0x00FF00FF))))
            elif k == "wsbw":                                  # a full byte swap
                self.W(d, self.call("llvm.bswap.i32", x))
            elif k == "bitrev":
                self.W(d, self.call("sr_bitrev", x))
            else:
                self.W(d, b.sext(b.trunc(x, L.i8 if k == "seb" else L.i16), L.i32))
        elif k == "load":
            ea = b.add(self.R(src[0]), self.c32(op.imm), hint="ea")
            value = self.call(f"sr_llvm_r{op.width * 8}", ea)
            if op.aux:                                         # lb / lh sign-extend
                value = b.sext(b.trunc(value, L.i8 if op.width == 1 else L.i16), L.i32)
            self.W(d, value)
        elif k == "store":
            ea = b.add(self.R(src[0]), self.c32(op.imm), hint="ea")
            self.call(f"sr_llvm_w{op.width * 8}_pc", ea, self.R(src[1]), self.c32(op.addr))
            return ea, op.width
        elif k in ("lwl", "lwr"):
            ea = b.add(self.R(src[1]), self.c32(op.imm), hint="ea")
            self.W(d, self.call(f"sr_{k}", self.R(src[0]), ea))
        elif k in ("swl", "swr"):
            ea = b.add(self.R(src[0]), self.c32(op.imm), hint="ea")
            self.call(f"sr_{k}_pc", ea, self.R(src[1]), self.c32(op.addr))
            return b.and_(ea, self.c32(0xFFFFFFFC)), 4
        elif k == "ll":
            ea = b.add(self.R(src[0]), self.c32(op.imm), hint="ea")
            if d != 0:
                self.W(d, self.call("sr_llvm_r32", ea))
            self.store_field("llbit", self.c32(1))
        elif k == "sc":                                        # closes its own trace record
            ea = b.add(self.R(src[0]), self.c32(op.imm), hint="ea")
            linked = b.icmp("ne", self.load_field("llbit"), self.c32(0))
            _, join = self.split(linked, "sc.store")
            self.call("sr_llvm_w32_pc", ea, self.R(src[1]), self.c32(op.addr))
            self.enter(join)
            self.W(d, b.zext(linked, L.i32))
            self.end(ea, b.select(linked, self.c32(4), self.c32(0)))
            return None
        else:
            raise AssertionError(f"lowering plan op {k!r} has no LLVM rendering")
        return self.c32(0), 0

    def _store_hilo(self, value64):
        b = self.builder
        self.store_field("lo", b.trunc(value64, L.i32))
        self.store_field("hi", b.trunc(b.lshr(value64, L.const(L.i64, 32)), L.i32))

    def _div_signed(self, a, t):
        """codegen.effect `div`: INT_MIN/-1 and division by zero are defined results."""
        b = self.builder
        x, y = self.R(a), self.R(t)
        zero = b.icmp("eq", y, self.c32(0))
        overflow = b.and_(b.zext(b.icmp("eq", x, self.c32(0x80000000)), L.i32),
                          b.zext(b.icmp("eq", y, self.c32(0xFFFFFFFF)), L.i32))
        overflow = b.icmp("ne", overflow, self.c32(0))
        unsafe = b.icmp("ne", b.or_(b.zext(zero, L.i32), b.zext(overflow, L.i32)), self.c32(0))
        divisor = b.select(unsafe, self.c32(1), y)
        quotient, remainder = b.sdiv(x, divisor), b.srem(x, divisor)
        by_zero_lo = b.select(b.icmp("slt", x, self.c32(0)), self.c32(1), self.c32(0xFFFFFFFF))
        lo = b.select(overflow, self.c32(0x80000000), b.select(zero, by_zero_lo, quotient))
        hi = b.select(overflow, self.c32(0xFFFFFFFF), b.select(zero, x, remainder))
        self.store_field("lo", lo)
        self.store_field("hi", hi)

    def _div_unsigned(self, a, t):
        """codegen.effect `divu`: division by zero is a defined result."""
        b = self.builder
        x, y = self.R(a), self.R(t)
        zero = b.icmp("eq", y, self.c32(0))
        divisor = b.select(zero, self.c32(1), y)
        quotient, remainder = b.udiv(x, divisor), b.urem(x, divisor)
        by_zero_lo = b.select(b.icmp("ule", x, self.c32(0xFFFF)), self.c32(0xFFFF), self.c32(0xFFFFFFFF))
        self.store_field("lo", b.select(zero, by_zero_lo, quotient))
        self.store_field("hi", b.select(zero, x, remainder))

    # -- control -------------------------------------------------------------
    def cond(self, cond: P.Cond):
        """codegen.cond_expr, as an i1."""
        lhs = self.R(cond.rs)
        rhs = self.R(cond.rt) if cond.kind in ("eq", "ne") else self.c32(0)
        return self.builder.icmp(_COND[cond.kind], lhs, rhs)

    def lower(self):
        plan, b = self.plan, self.builder
        self.function = self.module.add_function(self.definition_name, BODY_TYPE, ["s"],
                                                  BODY_ATTRIBUTES)
        # Prologue (codegen.emit_function): the preemption point, then the entry SP a
        # callable body restores on its host returns. The host-stack check goes in
        # front of both once the body is known to make a direct CALL.
        entry = self.function.append_basic_block("entry")
        b.position_at_end(entry)
        first = self.call("sr_llvm_yield", self.s, self.c32(self.start))
        self.sp_entry = None if self.resumable else self.R(29)
        body = self.function.append_basic_block("body")
        b.br(self.at_label(self.start) if plan.entry_label else body)
        b.position_at_end(body)

        for step in plan.steps:
            if isinstance(step, P.Label):
                self.enter(self.at_label(step.addr))
            elif isinstance(step, P.Op):
                self.instruction(step)
            elif isinstance(step, P.FallInto):
                self.goto(self.label(f"cont_{step.target:08x}"))
            else:
                self.transfer(step)

        if plan.exits:
            if b.block.terminator is None:
                b.br(self.label("fallthrough_return"))
            for exit_ in plan.exits:
                b.position_at_end(self.label(f"cont_{exit_.target:08x}"))
                self.tail_transfer(self.c32(exit_.target), exit_.symbol)
            self.enter(self.label("fallthrough_return"))
        if b.block.terminator is None:
            if not (self.resumable or self.stack_census):
                self.W(29, self.sp_entry)
            b.ret_void()

        if self.direct_calls:
            b.position_before(first)
            frame = self.call("llvm.frameaddress.p0", L.const(L.i32, 0))
            self.call("sr_llvm_host_stack_check", self.c32(self.start), frame)
        # A transfer that leaves the body opens a fresh block for any C code that follows it
        # (dead()); one that nothing followed, such as the block after each continuation exit,
        # is empty and unreachable, and goes. Anything else malformed fails here, by name.
        for block in list(self.function.blocks):
            if not block.instructions and block is not self.function.blocks[0]:
                self.function.delete_block(block)
        self.function.check()
        return self.function

    def transfer(self, t: P.Transfer):
        """One Transfer, as codegen.emit_function emits its kind."""
        link_value = self.c32((t.addr + 8) & 0xFFFFFFFF)

        def then():
            if t.then_continuation is not None:
                self.goto(self.label(f"cont_{t.then_continuation:08x}"))
            elif t.then_label is not None:
                self.goto(self.at_label(t.then_label))

        def call_target():
            if t.symbol is not None:
                self.checked_body_call(t.symbol, t.addr)
            else:
                self.call("dispatch_call", self.s, self.c32(t.target), link_value)

        if t.kind == "call" and t.cond is None:                # jal
            self.begin(t.addr, t.word)
            self.W(t.link_reg, link_value)
            self.end()
            self.instruction(t.slot)
            call_target()
            then()
        elif t.kind in ("call-computed", "tail-computed"):     # jalr / jr rs
            latched = self.R(t.target_reg)                     # PR #126: latched at the transfer
            self.begin(t.addr, t.word)
            if t.link_reg is not None:
                self.W(t.link_reg, link_value)
            self.end()
            self.instruction(t.slot)
            if t.kind == "call-computed":
                self.call("dispatch_call", self.s, latched, link_value)
                then()
            else:
                self.tail_transfer(latched, None)
        elif t.kind == "return":                               # jr $ra
            if t.target_reg is not None:
                self.call("sr_llvm_return_capture", self.s, self.R(t.target_reg))
            self.begin(t.addr, t.word)
            self.end()
            if t.slot_syscall is not None:
                self.call("sr_raw_syscall", self.s, self.c32(t.slot_syscall),
                          self.c32(t.addr + 4))
            else:
                self.instruction(t.slot)
            self.host_return()
        elif t.kind.startswith("jump-"):                       # j
            self.begin(t.addr, t.word)
            self.end()
            self.instruction(t.slot)
            if t.kind == "jump-tail":
                self.tail_transfer(self.c32(t.target), t.symbol)
            else:
                if t.backward:
                    self.yield_(t.addr)
                self.goto(self.at_label(t.target))
        else:                                                  # conditional: cond, link, slot
            taken_cond = self.cond(t.cond)
            self.begin(t.addr, t.word)
            if t.link_reg is not None:
                self.W(t.link_reg, link_value)
            self.end()
            if t.kind == "call":                               # linked branch CALL (#968)
                self.instruction(t.slot)
                _, join = self.split(taken_cond, "call")
                call_target()
                self.enter(join)
                then()
                return
            if not t.likely:
                self.instruction(t.slot)
            _, join = self.split(taken_cond, "taken")
            if t.likely:
                self.instruction(t.slot)
            if t.kind == "branch-label":
                if t.backward:
                    self.yield_(t.addr)
                self.goto(self.at_label(t.target))
            else:
                self.store_field("pc", self.c32(t.target))
                self.tail_transfer(self.load_field("pc"), None)
            self.enter(join)
            if t.then_label is not None:
                self.goto(self.at_label(t.then_label))


def lower(cg, elf, start, ranges, known, *, c_text: str, resume_owners=None, resumable=False,
          profile=None, dispatch_boundaries=None, stack_census=False) -> LLVMBody:
    """Lower one function the C emitter already translated; raises LLVMUnsupported.

    ``cg`` is the running codegen module (its flags and address space are live state).
    """
    plan = P.plan_function(cg, elf, start, ranges, known, resume_owners=resume_owners,
                           resumable=resumable, profile=profile,
                           dispatch_boundaries=dispatch_boundaries)
    lowering = FunctionLowering(plan, stack_census)
    function = lowering.lower()
    return LLVMBody.make(lowering.definition_name, start, lowering.module, function, len(c_text))


def chunk_module(name: str, bodies, renames: dict[str, str]) -> L.Module:
    """One chunk's module: the lowered bodies linked, generated names renamed."""
    module = L.Module(name)
    for body in bodies:
        module.link_in(body.module)
    module.rename_symbols(renames)
    return module


def manifest(lowered: list[int], fallbacks: list[tuple[int, str]]) -> str:
    """The per-build record of which functions are LLVM IR and why the others are not."""
    document = {
        "schema": "nakagawa-llvm-backend-manifest-v1",
        "backend": BACKEND_ID,
        "lowered": [f"0x{a:08x}" for a in sorted(lowered)],
        "fallbacks": [{"address": f"0x{a:08x}", "reason": reason}
                      for a, reason in sorted(fallbacks)],
    }
    return json.dumps(document, indent=1, sort_keys=True) + "\n"


def check_manifest(path: str, min_lowered: int) -> int:
    """Print a build's lowering record; fail when fewer than ``min_lowered`` were lowered."""
    with open(path, encoding="ascii") as handle:
        document = json.load(handle)
    if document.get("backend") != BACKEND_ID:
        print(f"LLVM_MANIFEST FAIL: {path} records backend {document.get('backend')!r}, "
              f"expected {BACKEND_ID}")
        return 1
    lowered, fallbacks = document["lowered"], document["fallbacks"]
    for row in fallbacks:
        print(f"LLVM_MANIFEST fallback {row['address']}: {row['reason']}")
    verdict = "OK" if len(lowered) >= min_lowered else "FAIL"
    print(f"LLVM_MANIFEST {verdict}: {len(lowered)} lowered, {len(fallbacks)} kept in C "
          f"(minimum {min_lowered} lowered)")
    return 0 if verdict == "OK" else 1


def main(argv=None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description="LLVM backend build records (#330)")
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check-manifest", help="summarize <game>_recomp_llvm.json")
    check.add_argument("manifest")
    check.add_argument("--min-lowered", type=int, default=1)
    args = parser.parse_args(argv)
    return check_manifest(args.manifest, args.min_lowered)


if __name__ == "__main__":
    raise SystemExit(main())
