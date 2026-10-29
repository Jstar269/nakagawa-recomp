# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The opt-in LLVM IR backend for generated guest functions (#330).

``codegen.py --backend=llvm`` lowers each guest function this module supports to
LLVM IR instead of C; every other function keeps its C body, and the reason is
recorded (``<game>_recomp_llvm.json``), never silently dropped. The C backend stays
the default. docs/LLVM_BACKEND.md is the design.

What the lowering consumes, and what it decides itself
------------------------------------------------------
It never rediscovers MIPS structure. It takes the C emitter's own ownership
(``function_flow``: the instruction set, native labels and continuation exits of
the body), the same entry catalog, resume owners and dispatch boundaries, and the
same transfer rules (``emit_function``): which delay slots are inlined, which run
twice under a label, which call is checked, which transfer is a TAIL. A function
reaches this backend only after the C emitter translated it, so an encoding the C
emitter refuses stays its fail-closed stub.

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


class LLVMUnsupported(Exception):
    """A function this backend does not lower; it keeps its C body (recorded)."""


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
    which the chunk writer links into that chunk's ``.ll`` file.
    """

    module: L.Module
    function: L.Function
    address: int

    @classmethod
    def make(cls, symbol: str, address: int, module: L.Module, function: L.Function):
        text = cls(f"void {symbol}(CpuState *s);  /* body in LLVM IR (#330) */")
        text.module = module
        text.function = function
        text.address = address
        return text

    def with_text(self, text: str) -> "LLVMBody":
        other = LLVMBody(text)
        other.module, other.function, other.address = self.module, self.function, self.address
        return other


#: emit_function's GUEST_ABORT site: the one hst_profile emission site no codegen table
#: names. tools/test_codegen_llvm.py re-derives every `hst_profile and addr == ...` site
#: from codegen.py's AST and checks that hst_emission_sites covers it.
HST_GUEST_ABORT = 0x00000a1c


def hst_emission_sites(cg) -> set[int]:
    """Addresses where emit_function emits HST-profile code instead of the plain word."""
    return (set(cg._SV_SPECIAL) | set(cg.GUEST_PATCHES) | set(cg.NULL_BASE_WORD_LOADS)
            | {HST_GUEST_ABORT})


def census_body_symbol(addr: int) -> str:
    """The wrapped body name codegen.wrap_stack_census_functions gives a callable."""
    return f"_sr_stack_census_body_{addr:08x}"


def _gpr_effect_kind(cg, w):
    """Prototype scope for one non-control word: None when lowered, else the reason."""
    op = w >> 26
    if op == 0:
        fn = w & 0x3F
        if fn in (0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F,
                  0x10, 0x11, 0x12, 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
                  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D,
                  0x2E, 0x2F):
            return None
        return f"SPECIAL funct 0x{fn:02x}"
    if 0x08 <= op <= 0x0F:
        return None
    if op == 0x1F:
        fn = w & 0x3F
        if fn in (0x00, 0x04) or (fn == 0x20 and ((w >> 6) & 0x1F) in (0x02, 0x03, 0x10, 0x14, 0x18)):
            return None
        return f"SPECIAL3 funct 0x{fn:02x}"
    if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x30, 0x38):
        return None
    if op == 0x2F:
        return "cache under --stale-detect" if cg.STALE_DETECT else None
    if op == 0x10:
        return "COP0"
    if op in (0x11, 0x31, 0x39):
        return "FPU"
    return f"opcode 0x{op:02x} (VFPU or other coprocessor)"


class FunctionLowering:
    """One guest function, lowered the way ``codegen.emit_function`` emits it."""

    def __init__(self, cg, elf, start, ranges, known, resume_owners, resumable, profile,
                 dispatch_boundaries, stack_census):
        self.cg = cg
        self.elf = elf
        self.start = start
        self.ranges = ranges
        self.known = known
        self.resume_owners = resume_owners or {}
        self.resumable = resumable
        self.hst_profile = profile == "hst"
        self.dispatch_boundaries = dispatch_boundaries
        self.stack_census = stack_census
        self.symbol = cg.ADDRESS_SPACE.symbol(start, self.resume_owners)
        # Under --stack-census a callable's body is wrapped (codegen.wrap_stack_census_functions):
        # the IR defines the wrapped body and every call still names the wrapper.
        self.definition_name = (census_body_symbol(start)
                                if stack_census and start not in self.resume_owners else self.symbol)
        self.module = L.Module(self.symbol)
        self.builder = L.Builder()
        self.function = None
        self.blocks: dict[str, L.BasicBlock] = {}
        self.sp_entry = None
        self.direct_calls = False
        self._dead = 0

    # -- scope -------------------------------------------------------------
    def _word(self, addr):
        return self.cg.ADDRESS_SPACE.instruction_word(addr, self.cg.read32(self.elf, addr))

    def check_scope(self, insns):
        cg = self.cg
        if cg.ADDRESS_SPACE.runtime_module:
            raise LLVMUnsupported("runtime-placed module (relocatable addresses)")
        if cg.LLE_CPU:
            raise LLVMUnsupported("--lle-cpu exception lowering is not in the prototype scope")
        if cg.SV_ENABLED:
            raise LLVMUnsupported("--static-verify assertions are emitted by the C backend only")
        if cg.ENHANCEMENT_SITES and insns & set(cg.ENHANCEMENT_SITES):
            raise LLVMUnsupported("enhancement-package rewrite site")
        if self.hst_profile:
            hit = sorted(insns & hst_emission_sites(cg))
            if hit:
                raise LLVMUnsupported(f"title-profile special site 0x{hit[0]:08x}")
        for addr in sorted(insns):
            w = self._word(addr)
            if w is None:
                continue
            if cg.is_control(w):
                if cg.is_eret(w):
                    raise LLVMUnsupported(f"eret at 0x{addr:08x}")
                op = w >> 26
                if op in (0x11, 0x12):
                    raise LLVMUnsupported(f"coprocessor branch at 0x{addr:08x}")
                continue
            reason = _gpr_effect_kind(cg, w)
            if reason is not None:
                raise LLVMUnsupported(f"{reason} at 0x{addr:08x}")

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

    def call_target(self, target, call_pc):
        """A guest CALL: the checked native call of a known entry, else dispatch."""
        if target in self.known or target in self.resume_owners:
            self.checked_body_call(self.cg.ADDRESS_SPACE.symbol(target, self.resume_owners), call_pc)
        else:
            self.call("dispatch_call", self.s, self.c32(target), self.c32((call_pc + 8) & 0xFFFFFFFF))

    # -- non-control instructions ------------------------------------------
    def instruction(self, addr, w):
        """codegen.normal_line: the trace pair around one non-control word."""
        self.begin(addr, w)
        traced = self.effect(addr, w)
        if traced is not None:
            self.end(*traced)

    def effect(self, addr, w):
        """codegen.effect, lowered. Returns sr_end's (address, size), or None when the
        instruction closes its own trace record or leaves the body."""
        cg, b = self.cg, self.builder
        op = w >> 26
        a, t, d, sh, fn = cg.rs(w), cg.rt(w), cg.rd(w), cg.sa(w), cg.funct(w)
        imm = self.c32(cg.s16(w))
        zimm = self.c32(w & 0xFFFF)
        if op == 0:
            if fn == 0x00:
                self.W(d, b.shl(self.R(t), self.c32(sh)))
            elif fn == 0x02:
                if a == 1:                                     # Allegrex ROTR
                    x = self.R(t)
                    self.W(d, x if sh == 0 else
                           b.or_(b.lshr(x, self.c32(sh)), b.shl(x, self.c32(32 - sh))))
                else:
                    self.W(d, b.lshr(self.R(t), self.c32(sh)))
            elif fn == 0x03:
                self.W(d, b.ashr(self.R(t), self.c32(sh)))
            elif fn in (0x04, 0x06, 0x07):
                amount = b.and_(self.R(a), self.c32(31))
                x = self.R(t)
                if fn == 0x04:
                    self.W(d, b.shl(x, amount))
                elif fn == 0x07:
                    self.W(d, b.ashr(x, amount))
                elif sh == 1:                                  # Allegrex ROTRV
                    back = b.and_(b.sub(self.c32(32), amount), self.c32(31))
                    self.W(d, b.or_(b.lshr(x, amount), b.shl(x, back)))
                else:
                    self.W(d, b.lshr(x, amount))
            elif fn in (0x0A, 0x0B):                           # movz / movn
                pred = "eq" if fn == 0x0A else "ne"
                moved = b.icmp(pred, self.R(t), self.c32(0))
                if d != 0:
                    _, join = self.split(moved, "mov")
                    self.W(d, self.R(a))
                    self.enter(join)
            elif fn == 0x0C:                                   # syscall: HLE boundary
                code = (w >> 6) & 0xFFFFF
                self.call("sr_raw_syscall", self.s, self.c32(code), self.c32(addr))
                self.ret()
                return None
            elif fn == 0x0D:                                   # break
                code = (w >> 6) & 0xFFFFF
                self.call("sr_break", self.s, self.c32(code), self.c32(addr))
            elif fn == 0x0F:                                   # sync: no effect
                pass
            elif fn == 0x10:
                self.W(d, self.load_field("hi"))
            elif fn == 0x11:
                self.store_field("hi", self.R(a))
            elif fn == 0x12:
                self.W(d, self.load_field("lo"))
            elif fn == 0x13:
                self.store_field("lo", self.R(a))
            elif fn == 0x16:                                   # clz (ctlz is defined at 0)
                self.W(d, self.call("llvm.ctlz.i32", self.R(a), L.const(L.i1, 0)))
            elif fn == 0x17:                                   # clo
                inverted = b.xor(self.R(a), self.c32(0xFFFFFFFF))
                self.W(d, self.call("llvm.ctlz.i32", inverted, L.const(L.i1, 0)))
            elif fn in (0x18, 0x19):                           # mult / multu
                ext = b.sext if fn == 0x18 else b.zext
                product = b.mul(ext(self.R(a), L.i64), ext(self.R(t), L.i64))
                self._store_hilo(product)
            elif fn == 0x1A:
                self._div_signed(a, t)
            elif fn == 0x1B:
                self._div_unsigned(a, t)
            elif fn in (0x1C, 0x1D, 0x2E, 0x2F):               # madd / maddu / msub / msubu
                ext = b.sext if fn in (0x1C, 0x2E) else b.zext
                product = b.mul(ext(self.R(a), L.i64), ext(self.R(t), L.i64))
                acc = b.or_(b.shl(b.zext(self.load_field("hi"), L.i64), L.const(L.i64, 32)),
                            b.zext(self.load_field("lo"), L.i64))
                self._store_hilo(b.add(acc, product) if fn in (0x1C, 0x1D) else b.sub(acc, product))
            elif fn in (0x20, 0x21):
                self.W(d, b.add(self.R(a), self.R(t)))
            elif fn in (0x22, 0x23):
                self.W(d, b.sub(self.R(a), self.R(t)))
            elif fn == 0x24:
                self.W(d, b.and_(self.R(a), self.R(t)))
            elif fn == 0x25:
                self.W(d, b.or_(self.R(a), self.R(t)))
            elif fn == 0x26:
                self.W(d, b.xor(self.R(a), self.R(t)))
            elif fn == 0x27:
                self.W(d, b.xor(b.or_(self.R(a), self.R(t)), self.c32(0xFFFFFFFF)))
            elif fn in (0x2A, 0x2B):                           # slt / sltu
                cmp = b.icmp("slt" if fn == 0x2A else "ult", self.R(a), self.R(t))
                self.W(d, b.zext(cmp, L.i32))
            elif fn in (0x2C, 0x2D):                           # Allegrex max / min
                x, y = self.R(a), self.R(t)
                self.W(d, b.select(b.icmp("sgt" if fn == 0x2C else "slt", x, y), x, y))
            else:
                raise LLVMUnsupported(f"SPECIAL funct 0x{fn:02x} at 0x{addr:08x}")
            return self.c32(0), 0
        if op in (0x08, 0x09):
            self.W(t, b.add(self.R(a), imm))
        elif op in (0x0A, 0x0B):
            cmp = b.icmp("slt" if op == 0x0A else "ult", self.R(a), imm)
            self.W(t, b.zext(cmp, L.i32))
        elif op == 0x0C:
            self.W(t, b.and_(self.R(a), zimm))
        elif op == 0x0D:
            self.W(t, b.or_(self.R(a), zimm))
        elif op == 0x0E:
            self.W(t, b.xor(self.R(a), zimm))
        elif op == 0x0F:
            self.W(t, self.c32((w & 0xFFFF) << 16))
        elif op == 0x1F:
            self._special3(addr, w)
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25):            # loads
            ea = b.add(self.R(a), imm, hint="ea")
            width = {0x20: 8, 0x24: 8, 0x21: 16, 0x25: 16, 0x23: 32}[op]
            value = self.call(f"sr_llvm_r{width}", ea)
            if op in (0x20, 0x21):
                narrow = L.i8 if op == 0x20 else L.i16
                value = b.sext(b.trunc(value, narrow), L.i32)
            self.W(t, value)
        elif op in (0x28, 0x29, 0x2B):                         # stores
            ea = b.add(self.R(a), imm, hint="ea")
            width = {0x28: 8, 0x29: 16, 0x2B: 32}[op]
            self.call(f"sr_llvm_w{width}_pc", ea, self.R(t), self.c32(addr))
            return ea, width // 8
        elif op in (0x22, 0x26):                               # lwl / lwr
            ea = b.add(self.R(a), imm, hint="ea")
            self.W(t, self.call("sr_lwl" if op == 0x22 else "sr_lwr", self.R(t), ea))
        elif op in (0x2A, 0x2E):                               # swl / swr
            ea = b.add(self.R(a), imm, hint="ea")
            self.call("sr_swl_pc" if op == 0x2A else "sr_swr_pc", ea, self.R(t), self.c32(addr))
            return b.and_(ea, self.c32(0xFFFFFFFC)), 4
        elif op == 0x30:                                       # ll
            ea = b.add(self.R(a), imm, hint="ea")
            if t != 0:
                self.W(t, self.call("sr_llvm_r32", ea))
            self.store_field("llbit", self.c32(1))
        elif op == 0x38:                                       # sc: closes its own trace record
            ea = b.add(self.R(a), imm, hint="ea")
            linked = b.icmp("ne", self.load_field("llbit"), self.c32(0))
            _, join = self.split(linked, "sc.store")
            self.call("sr_llvm_w32_pc", ea, self.R(t), self.c32(addr))
            self.enter(join)
            self.W(t, b.zext(linked, L.i32))
            self.end(ea, b.select(linked, self.c32(4), self.c32(0)))
            return None
        elif op == 0x2F:                                       # cache: no-op without --stale-detect
            pass
        else:
            raise LLVMUnsupported(f"opcode 0x{op:02x} at 0x{addr:08x}")
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

    def _special3(self, addr, w):
        cg, b = self.cg, self.builder
        a, t, d = cg.rs(w), cg.rt(w), cg.rd(w)
        fn = w & 0x3F
        if fn == 0x00:                                         # ext
            pos, size = cg.sa(w), ((w >> 11) & 0x1F) + 1
            mask = 0xFFFFFFFF if size >= 32 else (1 << size) - 1
            self.W(t, b.and_(b.lshr(self.R(a), self.c32(pos)), self.c32(mask)))
        elif fn == 0x04:                                       # ins
            pos, msb = cg.sa(w), (w >> 11) & 0x1F
            size = msb - pos + 1
            if size <= 0:
                raise LLVMUnsupported(f"INS with msb below lsb at 0x{addr:08x}")
            mask = ((0xFFFFFFFF if size >= 32 else (1 << size) - 1) << pos) & 0xFFFFFFFF
            kept = b.and_(self.R(t), self.c32(~mask & 0xFFFFFFFF))
            inserted = b.and_(b.shl(self.R(a), self.c32(pos)), self.c32(mask))
            self.W(t, b.or_(kept, inserted))
        else:
            sub = cg.sa(w)
            x = self.R(t)
            if sub == 0x02:                                    # wsbh
                self.W(d, b.or_(b.shl(b.and_(x, self.c32(0x00FF00FF)), self.c32(8)),
                                b.and_(b.lshr(x, self.c32(8)), self.c32(0x00FF00FF))))
            elif sub == 0x03:                                  # wsbw: a full byte swap
                self.W(d, self.call("llvm.bswap.i32", x))
            elif sub in (0x10, 0x18):                          # seb / seh
                self.W(d, b.sext(b.trunc(x, L.i8 if sub == 0x10 else L.i16), L.i32))
            elif sub == 0x14:                                  # bitrev
                self.W(d, self.call("sr_bitrev", x))
            else:
                raise LLVMUnsupported(f"SPECIAL3 BSHFL 0x{sub:02x} at 0x{addr:08x}")

    # -- control -------------------------------------------------------------
    def cond(self, w):
        """codegen.cond_expr, as an i1."""
        cg, b = self.cg, self.builder
        op = w >> 26
        if op in (4, 20):
            return b.icmp("eq", self.R(cg.rs(w)), self.R(cg.rt(w)))
        if op in (5, 21):
            return b.icmp("ne", self.R(cg.rs(w)), self.R(cg.rt(w)))
        if op in (6, 22):
            return b.icmp("sle", self.R(cg.rs(w)), self.c32(0))
        if op in (7, 23):
            return b.icmp("sgt", self.R(cg.rs(w)), self.c32(0))
        if op == 1:
            sub = cg.rt(w)
            if sub in (0, 2, 0x10):
                return b.icmp("slt", self.R(cg.rs(w)), self.c32(0))
            if sub in (1, 3, 0x11):
                return b.icmp("sge", self.R(cg.rs(w)), self.c32(0))
        raise LLVMUnsupported(f"branch op 0x{op:02x}")

    def delay_slot(self, ds, dsw):
        """codegen.delay_slot_lines without --lle-cpu: the slot runs as a plain word."""
        if dsw is None:
            raise LLVMUnsupported(f"unreadable delay slot at 0x{ds:08x}")
        if self.cg.is_control(dsw):
            raise LLVMUnsupported(f"control in delay slot at 0x{ds:08x}")
        self.instruction(ds, dsw)

    def lower(self):
        cg = self.cg
        insns, labels, continuations = cg.function_flow(
            self.elf, self.start, self.ranges, self.known, resume_owners=self.resume_owners,
            dispatch_boundaries=self.dispatch_boundaries)
        if not insns:
            raise LLVMUnsupported("empty body")
        self.check_scope(insns)
        dup_slot_skips = {}
        for addr in insns:
            w = cg.read32(self.elf, addr)
            if w is None or not cg.is_control(w):
                continue
            op, fn = w >> 26, w & 0x3F
            if op == 2 or (op == 0 and fn == 0x08) or cg.is_eret(w):
                continue
            if addr + 4 in labels and addr + 8 in insns:
                dup_slot_skips[addr] = addr + 8
        labels = set(labels) | set(dup_slot_skips.values())
        if self.start != min(insns):
            labels.add(self.start)

        self.function = self.module.add_function(self.definition_name, BODY_TYPE, ["s"],
                                                  BODY_ATTRIBUTES)
        b = self.builder
        # Prologue (codegen.emit_function): the preemption point, then the entry SP a
        # callable body restores on its host returns. The host-stack check goes in
        # front of both once the body is known to make a direct CALL.
        entry = self.function.append_basic_block("entry")
        b.position_at_end(entry)
        first = self.call("sr_llvm_yield", self.s, self.c32(self.start))
        self.sp_entry = None if self.resumable else self.R(29)
        body = self.function.append_basic_block("body")
        b.br(self.label(f"L_{self.start:08x}") if self.start != min(insns) else body)
        b.position_at_end(body)

        consumed = set()
        for addr in sorted(insns):
            if addr in consumed:
                continue
            if addr in labels:
                self.enter(self.label(f"L_{addr:08x}"))
            w = self._word(addr)
            if w is None:
                continue
            if not cg.is_control(w):
                self.instruction(addr, w)
                if addr in continuations:
                    b.br(self.label(f"cont_{continuations[addr]:08x}"))
                    self.dead()
                continue
            self._control(addr, w, insns, labels, continuations, dup_slot_skips, consumed)

        if continuations:
            if b.block.terminator is None:
                b.br(self.label("fallthrough_return"))
            for target in sorted(set(continuations.values())):
                b.position_at_end(self.label(f"cont_{target:08x}"))
                if self.dispatch_boundaries and target in self.dispatch_boundaries:
                    self.tail_transfer(self.c32(target), None)
                else:
                    self.tail_transfer(self.c32(target),
                                       cg.ADDRESS_SPACE.symbol(target, self.resume_owners))
            self.enter(self.label("fallthrough_return"))
        if b.block.terminator is None:
            if not (self.resumable or self.stack_census):
                self.W(29, self.sp_entry)
            b.ret_void()

        if self.direct_calls:
            b.position_before(first)
            frame = self.call("llvm.frameaddress.p0", L.const(L.i32, 0))
            self.call("sr_llvm_host_stack_check", self.c32(self.start), frame)
        return self.function

    def _control(self, addr, w, insns, labels, continuations, dup_slot_skips, consumed):
        cg, b = self.cg, self.builder
        op, fn = w >> 26, w & 0x3F
        ds = addr + 4
        dsw = self._word(ds)
        if ds not in labels:
            consumed.add(ds)
        ds_is_syscall = dsw is not None and (dsw >> 26) == 0 and (dsw & 0x3F) == 0x0C
        link_value = self.c32((addr + 8) & 0xFFFFFFFF)

        def after_call():
            if addr in continuations:
                b.br(self.label(f"cont_{continuations[addr]:08x}"))
                self.dead()
            elif addr in dup_slot_skips:
                b.br(self.label(f"L_{dup_slot_skips[addr]:08x}"))
                self.dead()

        if op == 3:                                            # jal
            cg.ADDRESS_SPACE.check_jump(addr, w)
            target = cg.jump_target(addr, w)
            self.begin(addr, w)
            self.W(31, link_value)
            self.end()
            self.delay_slot(ds, dsw)
            self.call_target(target, addr)
            after_call()
            return
        if op == 0 and fn == 0x09:                             # jalr rd, rs
            d, a = cg.rd(w), cg.rs(w)
            latched = self.R(a)                                # PR #126: latched at the transfer
            self.begin(addr, w)
            if d != 0:
                self.W(d, link_value)
            self.end()
            self.delay_slot(ds, dsw)
            if d != 0:
                self.call("dispatch_call", self.s, latched, link_value)
                after_call()
            else:
                self.tail_transfer(latched, None)
            return
        if op == 0 and fn == 0x08:                             # jr rs
            a = cg.rs(w)
            if a == 31:
                latched = self.R(31)
                self.call("sr_llvm_return_capture", self.s, latched)
                self.begin(addr, w)
                self.end()
                if ds_is_syscall:
                    self.call("sr_raw_syscall", self.s, self.c32((dsw >> 6) & 0xFFFFF), self.c32(ds))
                else:
                    self.delay_slot(ds, dsw)
                self.host_return()
            elif ds_is_syscall:
                self.begin(addr, w)
                self.end()
                self.call("sr_raw_syscall", self.s, self.c32((dsw >> 6) & 0xFFFFF), self.c32(ds))
                self.host_return()
            else:
                latched = self.R(a)
                self.begin(addr, w)
                self.end()
                self.delay_slot(ds, dsw)
                self.tail_transfer(latched, None)
            return
        if op == 2:                                            # j
            cg.ADDRESS_SPACE.check_jump(addr, w)
            target = cg.jump_target(addr, w)
            self.begin(addr, w)
            self.end()
            self.delay_slot(ds, dsw)
            if target == addr:
                self.yield_(addr)
                b.br(self.label(f"L_{target:08x}"))
                self.dead()
            elif (target in self.known or target in self.resume_owners) and not (
                    target in self.resume_owners and target in labels):
                self.tail_transfer(self.c32(target), cg.ADDRESS_SPACE.symbol(target, self.resume_owners))
            elif target in labels:
                if target <= addr:
                    self.yield_(addr)
                b.br(self.label(f"L_{target:08x}"))
                self.dead()
            else:
                self.tail_transfer(self.c32(target), None)
            return

        # conditional branch: the condition is read before the link and the slot
        target = cg.branch_target(addr, w)
        backward = target <= addr
        taken_cond = self.cond(w)
        self.begin(addr, w)
        if cg.is_link(w):
            self.W(31, link_value)
        self.end()
        if cg.is_linked_call(self.elf, addr, w, self.ranges):
            self.delay_slot(ds, dsw)
            _, join = self.split(taken_cond, "call")
            self.call_target(target, addr)
            self.enter(join)
            after_call()
            return
        likely = cg.is_likely(w)
        if not likely:
            self.delay_slot(ds, dsw)
        _, join = self.split(taken_cond, "taken")
        if likely:
            self.delay_slot(ds, dsw)
        if target in labels:
            if backward:
                self.yield_(addr)
            b.br(self.label(f"L_{target:08x}"))
            self.dead()
        else:
            self.store_field("pc", self.c32(target))
            self.tail_transfer(self.load_field("pc"), None)
        self.enter(join)
        if addr in dup_slot_skips:
            b.br(self.label(f"L_{dup_slot_skips[addr]:08x}"))
            self.dead()


def lower(cg, elf, start, ranges, known, *, resume_owners=None, resumable=False, profile=None,
          dispatch_boundaries=None, stack_census=False) -> LLVMBody:
    """Lower one function the C emitter already translated; raises LLVMUnsupported.

    ``cg`` is the running codegen module (its flags and address space are live state).
    """
    lowering = FunctionLowering(cg, elf, start, ranges, known, resume_owners, resumable,
                                profile, dispatch_boundaries, stack_census)
    function = lowering.lower()
    return LLVMBody.make(lowering.definition_name, start, lowering.module, function)


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
