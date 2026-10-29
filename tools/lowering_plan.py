# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The backend-neutral lowering plan of one guest function (#330).

A code generator should not decode MIPS. This module decodes it once per
function and resolves every decision a backend would otherwise make on its own:
which addresses are native labels, which delay slot runs where and how often,
which transfer is a checked CALL, a dispatched CALL, a TAIL record or an in-body
edge, which symbol a known target has, and where a body falls through into
another entry. Each instruction becomes an ``Op`` that names its semantic
operation and operands; each transfer a ``Transfer`` with its kind, target,
link, condition and slot. A backend renders the plan and decides nothing.

Design rule (owner, 2026-10-29): every backend lowers from the same analysis
representation, so a semantic fix lands once and backends cannot drift. Where
that stands:

* the plan is built from the C emitter's own ownership (``codegen.function_flow``,
  the entry catalog, resume owners and dispatch boundaries) and reproduces
  ``codegen.emit_function``'s transfer rules; it is the per-function view the
  #305 analysis IR (``tools/analysis_ir.py``) should feed once codegen consumes
  that IR, which today is advisory by its own contract;
* the LLVM IR backend (``tools/codegen_llvm.py``) renders only this plan;
* the C emitter does not consume it yet. Until it does, a test compares each
  plan with what ``emit_function`` emits for the same function (labels,
  continuation exits, CALL, dispatch and TAIL sites), so the two cannot drift
  silently. Moving ``emit_function`` onto the plan, and ``effect()``'s
  per-instruction C onto ``Op``, is the next step (docs/LLVM_BACKEND.md).

The plan covers the LLVM backend's first-stage subset; anything else raises
``Unsupported`` with a reason, and the caller keeps the C body.
"""

from __future__ import annotations

from dataclasses import dataclass, field

PLAN_VERSION = "nakagawa-lowering-plan-1"


class Unsupported(Exception):
    """A function or form the plan does not model; the function keeps its C body."""


@dataclass(frozen=True)
class Op:
    """One non-control instruction as a semantic operation.

    ``kind`` names the operation; ``dst`` is the GPR it writes (r0 included: a
    renderer drops that write), ``src`` the GPRs it reads in operand order, ``imm``
    the immediate as a 32-bit pattern (shift amount, extended field, code or
    offset) and ``aux`` a second constant (a field mask, a load's signedness).
    ``width`` is a memory access width in bytes.
    """

    addr: int
    word: int
    kind: str
    dst: int | None = None
    src: tuple[int, ...] = ()
    imm: int = 0
    aux: int = 0
    width: int = 0


@dataclass(frozen=True)
class Cond:
    """A branch condition, read before the branch's link write and delay slot."""

    kind: str            # eq, ne, lez, gtz, ltz, gez
    rs: int
    rt: int = 0


@dataclass(frozen=True)
class Transfer:
    """One control instruction with every decision resolved.

    Kinds:
      call            jal, or a linked branch to a callable (``cond`` set): a checked
                      native CALL of ``symbol``, or ``dispatch_call`` when it is None
      call-computed   jalr rd != 0: dispatch the latched ``target_reg``
      tail-computed   jr rs != 31, jalr $zero: TAIL-record the latched ``target_reg``
      return          jr $ra: capture the latched target, host return
      jump-self       j to itself: a preemption point and an in-body edge
      jump-label      j to an in-body label (``backward``: preemption point first)
      jump-tail       j out of the body: TAIL-record ``target`` (``symbol`` when known)
      branch-label    a conditional branch to an in-body label
      branch-tail     a conditional branch out of the body: TAIL-record ``target``
    """

    addr: int
    word: int
    kind: str
    target: int | None = None
    target_reg: int | None = None
    symbol: str | None = None
    link_reg: int | None = None
    cond: Cond | None = None
    likely: bool = False
    backward: bool = False
    slot: Op | None = None
    slot_syscall: int | None = None   # jr with a syscall in its slot: the raw syscall code
    then_continuation: int | None = None
    then_label: int | None = None


@dataclass(frozen=True)
class Label:
    addr: int


@dataclass(frozen=True)
class FallInto:
    """A body that runs into another entry: the continuation exit to ``target``."""

    target: int


@dataclass(frozen=True)
class ContinuationExit:
    target: int
    symbol: str | None    # None: a dispatch boundary, reached by address


@dataclass
class FunctionPlan:
    version: str
    start: int
    symbol: str
    resumable: bool                    # entered on a live owner frame (no entry SP to restore)
    resume_entry: bool                 # a resume body (r_<addr>), never stack-census wrapped
    entry_label: bool                  # the body starts at a label, not its first word
    steps: list = field(default_factory=list)
    exits: list[ContinuationExit] = field(default_factory=list)

    @property
    def labels(self) -> set[int]:
        return {step.addr for step in self.steps if isinstance(step, Label)}


# ---------------------------------------------------------------------------
# Instruction decoding: one place, MIPS32/Allegrex public encodings
# ---------------------------------------------------------------------------

_SPECIAL_RRR = {0x20: "add", 0x21: "add", 0x22: "sub", 0x23: "sub", 0x24: "and", 0x25: "or",
                0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2C: "max", 0x2D: "min"}
_SPECIAL_HILO = {0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x1C: "madd",
                 0x1D: "maddu", 0x2E: "msub", 0x2F: "msubu"}
_IMMEDIATE = {0x08: "addi", 0x09: "addi", 0x0A: "slti", 0x0B: "sltiu"}
_LOGICAL_IMMEDIATE = {0x0C: "andi", 0x0D: "ori", 0x0E: "xori"}
_LOADS = {0x20: (1, 1), 0x21: (2, 1), 0x23: (4, 0), 0x24: (1, 0), 0x25: (2, 0)}
_STORES = {0x28: 1, 0x29: 2, 0x2B: 4}
_BSHFL = {0x02: "wsbh", 0x03: "wsbw", 0x10: "seb", 0x14: "bitrev", 0x18: "seh"}


def _sext16(w: int) -> int:
    return ((w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF) & 0xFFFFFFFF


def decode_op(addr: int, w: int, *, stale_detect: bool = False) -> Op:
    """The semantic operation of one non-control word, or Unsupported."""
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    if op == 0:
        if fn in _SPECIAL_RRR:
            return Op(addr, w, _SPECIAL_RRR[fn], rd, (rs, rt))
        if fn in _SPECIAL_HILO:
            return Op(addr, w, _SPECIAL_HILO[fn], None, (rs, rt))
        if fn == 0x00:
            return Op(addr, w, "shl", rd, (rt,), sa)
        if fn == 0x02:
            if rs == 1:
                return Op(addr, w, "rotr", rd, (rt,), sa)          # Allegrex ROTR
            if rs == 0:
                return Op(addr, w, "lshr", rd, (rt,), sa)
        if fn == 0x03:
            return Op(addr, w, "ashr", rd, (rt,), sa)
        if fn == 0x04:
            return Op(addr, w, "shlv", rd, (rt, rs))
        if fn == 0x06:
            if sa == 1:
                return Op(addr, w, "rotrv", rd, (rt, rs))          # Allegrex ROTRV
            if sa == 0:
                return Op(addr, w, "lshrv", rd, (rt, rs))
        if fn == 0x07:
            return Op(addr, w, "ashrv", rd, (rt, rs))
        if fn in (0x0A, 0x0B):                                     # movz / movn: rs if rt (==|!=) 0
            return Op(addr, w, "movz" if fn == 0x0A else "movn", rd, (rt, rs))
        if fn in (0x0C, 0x0D):
            return Op(addr, w, "syscall" if fn == 0x0C else "break", imm=(w >> 6) & 0xFFFFF)
        if fn == 0x0F and (w & 0x03FFFFC0) == 0:
            return Op(addr, w, "nop")                              # sync stype 0
        if fn in (0x10, 0x12):
            return Op(addr, w, "mfhi" if fn == 0x10 else "mflo", rd)
        if fn in (0x11, 0x13):
            return Op(addr, w, "mthi" if fn == 0x11 else "mtlo", None, (rs,))
        if fn in (0x16, 0x17):
            return Op(addr, w, "clz" if fn == 0x16 else "clo", rd, (rs,))
        raise Unsupported(f"SPECIAL funct 0x{fn:02x} at 0x{addr:08x}")
    if op in _IMMEDIATE:
        return Op(addr, w, _IMMEDIATE[op], rt, (rs,), _sext16(w))
    if op in _LOGICAL_IMMEDIATE:
        return Op(addr, w, _LOGICAL_IMMEDIATE[op], rt, (rs,), w & 0xFFFF)
    if op == 0x0F:
        return Op(addr, w, "const", rt, (), ((w & 0xFFFF) << 16) & 0xFFFFFFFF)   # lui
    if op == 0x1F:
        if fn == 0x00:                                             # ext
            size = rd + 1
            return Op(addr, w, "ext", rt, (rs,), sa, 0xFFFFFFFF if size >= 32 else (1 << size) - 1)
        if fn == 0x04:                                             # ins
            size = rd - sa + 1
            if size <= 0:
                raise Unsupported(f"INS with msb below lsb at 0x{addr:08x}")
            mask = ((0xFFFFFFFF if size >= 32 else (1 << size) - 1) << sa) & 0xFFFFFFFF
            return Op(addr, w, "ins", rt, (rt, rs), sa, mask)
        if fn == 0x20 and sa in _BSHFL:
            return Op(addr, w, _BSHFL[sa], rd, (rt,))
        raise Unsupported(f"SPECIAL3 funct 0x{fn:02x} at 0x{addr:08x}")
    if op in _LOADS:
        width, signed = _LOADS[op]
        return Op(addr, w, "load", rt, (rs,), _sext16(w), signed, width)
    if op in _STORES:
        return Op(addr, w, "store", None, (rs, rt), _sext16(w), width=_STORES[op])
    if op in (0x22, 0x26):
        return Op(addr, w, "lwl" if op == 0x22 else "lwr", rt, (rt, rs), _sext16(w))
    if op in (0x2A, 0x2E):
        return Op(addr, w, "swl" if op == 0x2A else "swr", None, (rs, rt), _sext16(w), width=4)
    if op == 0x30:
        return Op(addr, w, "ll", rt, (rs,), _sext16(w), width=4)
    if op == 0x38:
        return Op(addr, w, "sc", rt, (rs, rt), _sext16(w), width=4)
    if op == 0x2F:
        if stale_detect:
            raise Unsupported(f"cache under --stale-detect at 0x{addr:08x}")
        return Op(addr, w, "nop")
    if op == 0x10:
        raise Unsupported(f"COP0 at 0x{addr:08x}")
    if op in (0x11, 0x31, 0x39):
        raise Unsupported(f"FPU at 0x{addr:08x}")
    raise Unsupported(f"opcode 0x{op:02x} (VFPU or other coprocessor) at 0x{addr:08x}")


def decode_cond(w: int) -> Cond:
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in (4, 20):
        return Cond("eq", rs, rt)
    if op in (5, 21):
        return Cond("ne", rs, rt)
    if op in (6, 22):
        return Cond("lez", rs)
    if op in (7, 23):
        return Cond("gtz", rs)
    if op == 1 and rt in (0, 2, 0x10):
        return Cond("ltz", rs)
    if op == 1 and rt in (1, 3, 0x11):
        return Cond("gez", rs)
    raise Unsupported(f"branch op 0x{op:02x}")


# ---------------------------------------------------------------------------
# Function plans
# ---------------------------------------------------------------------------

#: emit_function's GUEST_ABORT site: the one hst_profile emission site no codegen table
#: names. tools/test_codegen_llvm.py re-derives every `hst_profile and addr == ...` site
#: from codegen.py's AST and checks that hst_emission_sites covers it.
HST_GUEST_ABORT = 0x00000a1c


def hst_emission_sites(cg) -> set[int]:
    """Addresses where emit_function emits HST-profile code instead of the plain word."""
    return (set(cg._SV_SPECIAL) | set(cg.GUEST_PATCHES) | set(cg.NULL_BASE_WORD_LOADS)
            | {HST_GUEST_ABORT})


def plan_function(cg, elf, start, ranges, known, *, resume_owners=None, resumable=False,
                  profile=None, dispatch_boundaries=None) -> FunctionPlan:
    """The plan of the function at ``start``, from the running codegen module ``cg``."""
    resume_owners = resume_owners or {}
    space = cg.ADDRESS_SPACE
    if space.runtime_module:
        raise Unsupported("runtime-placed module (relocatable addresses)")
    if cg.LLE_CPU:
        raise Unsupported("--lle-cpu exception lowering is not in the prototype scope")
    if cg.SV_ENABLED:
        raise Unsupported("--static-verify assertions are emitted by the C backend only")
    insns, labels, continuations = cg.function_flow(
        elf, start, ranges, known, resume_owners=resume_owners,
        dispatch_boundaries=dispatch_boundaries)
    if not insns:
        raise Unsupported("empty body")
    if cg.ENHANCEMENT_SITES and insns & set(cg.ENHANCEMENT_SITES):
        raise Unsupported("enhancement-package rewrite site")
    if profile == "hst":
        hit = sorted(insns & hst_emission_sites(cg))
        if hit:
            raise Unsupported(f"title-profile special site 0x{hit[0]:08x}")

    def word(addr):
        return space.instruction_word(addr, cg.read32(elf, addr))

    def symbol(target):
        return space.symbol(target, resume_owners)

    # Scope first, so a function outside it fails with its first unsupported word.
    for addr in sorted(insns):
        w = word(addr)
        if w is None:
            continue
        if cg.is_control(w):
            if cg.is_eret(w):
                raise Unsupported(f"eret at 0x{addr:08x}")
            if (w >> 26) in (0x11, 0x12):
                raise Unsupported(f"coprocessor branch at 0x{addr:08x}")
        else:
            decode_op(addr, w, stale_detect=cg.STALE_DETECT)

    # A delay slot that is itself a branch target runs inline at its transfer and again
    # under its own label; fall-through past the transfer skips the labelled copy.
    dup_slot_skips = {}
    for addr in insns:
        w = cg.read32(elf, addr)
        if w is None or not cg.is_control(w):
            continue
        op, fn = w >> 26, w & 0x3F
        if op == 2 or (op == 0 and fn == 0x08) or cg.is_eret(w):
            continue
        if addr + 4 in labels and addr + 8 in insns:
            dup_slot_skips[addr] = addr + 8
    labels = set(labels) | set(dup_slot_skips.values())
    entry_label = start != min(insns)
    if entry_label:
        labels.add(start)

    plan = FunctionPlan(PLAN_VERSION, start, symbol(start), resumable, start in resume_owners,
                        entry_label)
    consumed = set()
    for addr in sorted(insns):
        if addr in consumed:
            continue
        if addr in labels:
            plan.steps.append(Label(addr))
        w = word(addr)
        if w is None:
            continue
        if not cg.is_control(w):
            plan.steps.append(decode_op(addr, w, stale_detect=cg.STALE_DETECT))
            if addr in continuations:
                plan.steps.append(FallInto(continuations[addr]))
            continue
        ds = addr + 4
        if ds not in labels:
            consumed.add(ds)
        plan.steps.append(_transfer(cg, elf, ranges, addr, w, ds, word(ds), known, resume_owners,
                                    labels, continuations, dup_slot_skips, symbol))
    for target in sorted(set(continuations.values())):
        boundary = bool(dispatch_boundaries) and target in dispatch_boundaries
        plan.exits.append(ContinuationExit(target, None if boundary else symbol(target)))
    return plan


def _slot(cg, ds, dsw):
    """codegen.delay_slot_lines without --lle-cpu: the slot is a plain word."""
    if dsw is None:
        raise Unsupported(f"unreadable delay slot at 0x{ds:08x}")
    if cg.is_control(dsw):
        raise Unsupported(f"control in delay slot at 0x{ds:08x}")
    return decode_op(ds, dsw, stale_detect=cg.STALE_DETECT)


def _transfer(cg, elf, ranges, addr, w, ds, dsw, known, resume_owners, labels, continuations,
              dup_slot_skips, symbol) -> Transfer:
    op, fn = w >> 26, w & 0x3F
    then = {"then_continuation": continuations.get(addr),
            "then_label": None if addr in continuations else dup_slot_skips.get(addr)}

    def callee(target):
        return symbol(target) if target in known or target in resume_owners else None

    if op == 3:                                                    # jal
        cg.ADDRESS_SPACE.check_jump(addr, w)
        target = cg.jump_target(addr, w)
        return Transfer(addr, w, "call", target=target, symbol=callee(target), link_reg=31,
                        slot=_slot(cg, ds, dsw), **then)
    if op == 0 and fn == 0x09:                                     # jalr rd, rs
        rd, rs = (w >> 11) & 31, (w >> 21) & 31
        if rd != 0:
            return Transfer(addr, w, "call-computed", target_reg=rs, link_reg=rd,
                            slot=_slot(cg, ds, dsw), **then)
        return Transfer(addr, w, "tail-computed", target_reg=rs, slot=_slot(cg, ds, dsw))
    ds_syscall = dsw is not None and (dsw >> 26) == 0 and (dsw & 0x3F) == 0x0C
    if op == 0 and fn == 0x08:                                     # jr rs
        rs = (w >> 21) & 31
        if rs == 31:
            if ds_syscall:
                return Transfer(addr, w, "return", target_reg=31, slot_syscall=(dsw >> 6) & 0xFFFFF)
            return Transfer(addr, w, "return", target_reg=31, slot=_slot(cg, ds, dsw))
        if ds_syscall:
            # Not reachable for import stubs (handled apart); a raw syscall, then a host return.
            return Transfer(addr, w, "return", slot_syscall=(dsw >> 6) & 0xFFFFF)
        return Transfer(addr, w, "tail-computed", target_reg=rs, slot=_slot(cg, ds, dsw))
    if op == 2:                                                    # j
        cg.ADDRESS_SPACE.check_jump(addr, w)
        target = cg.jump_target(addr, w)
        slot = _slot(cg, ds, dsw)
        if target == addr:
            return Transfer(addr, w, "jump-self", target=target, backward=True, slot=slot)
        if (target in known or target in resume_owners) and not (
                target in resume_owners and target in labels):
            return Transfer(addr, w, "jump-tail", target=target, symbol=symbol(target), slot=slot)
        if target in labels:
            return Transfer(addr, w, "jump-label", target=target, backward=target <= addr, slot=slot)
        return Transfer(addr, w, "jump-tail", target=target, slot=slot)
    # conditional branch
    target = cg.branch_target(addr, w)
    cond = decode_cond(w)
    link = 31 if cg.is_link(w) else None
    slot = _slot(cg, ds, dsw)
    if cg.is_linked_call(elf, addr, w, ranges):
        return Transfer(addr, w, "call", target=target, symbol=callee(target), link_reg=link,
                        cond=cond, slot=slot, **then)
    kind = "branch-label" if target in labels else "branch-tail"
    return Transfer(addr, w, kind, target=target, link_reg=link, cond=cond, likely=cg.is_likely(w),
                    backward=target <= addr, slot=slot, then_label=dup_slot_skips.get(addr))
