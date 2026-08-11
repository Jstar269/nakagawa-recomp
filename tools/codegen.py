# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

import struct
import re
import os
import sys
from dataclasses import dataclass

# Import the local analyzer
from analyze import analyze, Elf, in_ranges, exec_ranges
from host_stubs import HST_SIMPLE_STUBS
import entry_frame_balance


@dataclass(frozen=True)
class EntryInfo:
    """Independent roles and provenance for one emitted guest entry."""

    addr: int
    callable: bool
    resumable: bool
    owner: int | None
    provenance: frozenset[str]


# HST compatibility inventory.  These are structural entry classifications, not
# replacement behavior: every address is translated from the guest ELF.  Keep the
# profile explicit so unrelated zero-based MIPS ELFs do not inherit title addresses.
HST_MANUAL_CALLABLES = {
    0x0005A648: "address-taken-tiny-leaf",
    0x00042998: "address-taken-tiny-leaf",
    0x0003DB3C: "address-taken-tiny-leaf",
    0x000E1724: "address-taken-tiny-leaf",
    0x000E3B24: "address-taken-tiny-leaf",
    0x00014430: "address-taken-tiny-leaf",
}

HST_RESUME_OWNERS = {
    0x000310B0: 0x00030FDC,
    0x00021C78: 0x00021AC0,
    0x000B26A0: 0x000B237C,
}


def build_entry_catalog(analyzed, ranges, profile=None, elf=None):
    """Separate source-callable boundaries from externally dispatchable resumes.

    When ``elf`` is supplied, every declared resume/owner pair is additionally
    re-derived from the image by :mod:`entry_frame_balance` instead of being
    trusted as a constant.  That turns the three HST seed classifications from a
    recorded manual audit into an assertion which runs on every regeneration, so
    a wrong or stale declaration stops the build rather than silently emitting a
    resume body whose owner does not match.
    """
    catalog = {
        addr: EntryInfo(addr, True, False, None, frozenset({"analyzer"}))
        for addr in analyzed if in_ranges(addr, ranges)
    }
    if profile not in (None, "none", "hst"):
        raise ValueError(f"unknown codegen profile: {profile}")

    # Narrow the analyzer's role-mixed population only where two independent
    # facts agree: the target has the continuation stack signature and one
    # balanced analyzer callable reaches every direct-j source at the same live
    # depth.  The audit deliberately leaves loop/back-edge and ownerless cases
    # as ordinary analyzer entries.  It is image-driven; no title address is
    # embedded in this rule.
    if elf is not None:
        for candidate in entry_frame_balance.audit_direct_j_candidates(
            elf, catalog, ranges
        ):
            if candidate.classification != entry_frame_balance.CONTINUATION:
                continue
            old = catalog.get(candidate.addr)
            if old is None:
                continue
            if "hst-profile" in old.provenance or (
                profile == "hst" and candidate.addr in HST_MANUAL_CALLABLES
            ):
                raise RuntimeError(
                    f"DUAL-ROLE ENTRY DETECTED: direct-j resume "
                    f"0x{candidate.addr:08x} overlaps a profile callable"
                )
            provenance = set(old.provenance)
            provenance.update({"direct-j", "frame-balance-verified"})
            catalog[candidate.addr] = EntryInfo(
                candidate.addr,
                False,
                True,
                candidate.owners[0],
                frozenset(provenance),
            )

        # The conditional-branch slice.  Its proof model is separate from the
        # direct-`j` one -- a branch has a fall-through predecessor, its family
        # contains calls, and a backward edge is the ordinary loop idiom -- so it
        # is audited independently rather than by relaxing the rule above.  The
        # two slices overlap on addresses reached both ways; that overlap is used
        # as a cross-check, not merged: an address already carrying a resume
        # contract must be re-derived with the *same* owner, and a disagreement
        # stops the build instead of letting the later slice quietly win.
        for candidate in entry_frame_balance.audit_direct_branch_candidates(
            elf, catalog, ranges
        ):
            if candidate.classification != entry_frame_balance.CONTINUATION:
                continue
            old = catalog.get(candidate.addr)
            if old is None:
                continue
            owner = candidate.owners[0]
            if old.resumable:
                if old.owner != owner:
                    raise RuntimeError(
                        f"CONFLICTING RESUME OWNER: 0x{candidate.addr:08x} was "
                        f"derived as a continuation of 0x{old.owner:08x} and of "
                        f"0x{owner:08x}"
                    )
            elif "hst-profile" in old.provenance or (
                profile == "hst" and candidate.addr in HST_MANUAL_CALLABLES
            ):
                raise RuntimeError(
                    f"DUAL-ROLE ENTRY DETECTED: direct-branch resume "
                    f"0x{candidate.addr:08x} overlaps a profile callable"
                )
            provenance = set(old.provenance)
            provenance.update({"direct-branch", "frame-balance-verified"})
            catalog[candidate.addr] = EntryInfo(
                candidate.addr,
                False,
                True,
                owner,
                frozenset(provenance),
            )

    if profile is None or profile == "none":
        return catalog

    for addr, why in HST_MANUAL_CALLABLES.items():
        if not in_ranges(addr, ranges):
            continue
        old = catalog.get(addr)
        provenance = set(old.provenance if old else ())
        provenance.update({"hst-profile", why})
        catalog[addr] = EntryInfo(addr, True, False, None, frozenset(provenance))

    for addr, owner in HST_RESUME_OWNERS.items():
        if not in_ranges(addr, ranges):
            continue
        if addr in catalog and catalog[addr].callable:
            raise RuntimeError(
                f"DUAL-ROLE ENTRY DETECTED: 0x{addr:08x} is both callable and resumable"
            )
        if owner not in catalog or not catalog[owner].callable:
            raise RuntimeError(
                f"resume entry 0x{addr:08x} has missing callable owner 0x{owner:08x}"
            )
        provenance = {"hst-profile", "manual-seed"}
        if elf is not None:
            problems = entry_frame_balance.verify_resume_entry(
                elf, ranges, addr, owner
            )
            if problems:
                raise RuntimeError(
                    "RESUME ROLE NOT CONFIRMED BY IMAGE: " + "; ".join(problems)
                )
            callables = {a for a, info in catalog.items() if info.callable}
            # The extent claim itself -- that the owner's recovered extent
            # reaches the resume PC -- previously had no assertion anywhere.
            if not entry_frame_balance.owner_covers(
                elf, owner, addr, ranges, callables
            ):
                raise RuntimeError(
                    f"OWNER DOES NOT COVER RESUME: declared owner "
                    f"0x{owner:08x} does not reach 0x{addr:08x}"
                )
            # Wiki doc 26 section 14: a shared tail reachable from owners with
            # incompatible frame states cannot be given one host contract.
            owners = entry_frame_balance.find_frame_owners(
                elf, addr, ranges, callables
            )
            if len(set(owners.values())) > 1:
                raise RuntimeError(
                    f"INCOMPATIBLE MULTIPLE OWNERS: resume 0x{addr:08x} is "
                    "reached by balanced callables at differing stack depths "
                    + ", ".join(
                        f"0x{c:08x}(depth 0x{d:x})"
                        for c, d in sorted(owners.items())
                    )
                    + f"; declared owner 0x{owner:08x}"
                )
            provenance.add("frame-balance-verified")
        catalog[addr] = EntryInfo(addr, False, True, owner, frozenset(provenance))
    return catalog


def entry_symbol(addr, resume_owners=None):
    return f"r_{addr:08x}" if resume_owners and addr in resume_owners else f"f_{addr:08x}"


def emit_host_return(resumable, comment=None):
    """The single host-exit policy for callable and live-frame resume entries."""
    if resumable:
        return "return;"
    if comment:
        return f"s->r[29] = _sp_entry; /* {comment} */\n    return;"
    return "s->r[29] = _sp_entry; return;"


def emit_host_fallthrough(resumable):
    """Natural C fallthrough follows the same entry contract without a fake guest return."""
    if resumable:
        return "/* resume fall-through: guest owns SP */"
    return "s->r[29] = _sp_entry; /* o32 ABI: sp is callee-saved; balance on fall-through end (F3E) */"

R = lambda i: "0u" if i == 0 else f"s->r[{i}]"           # read GPR (r0 is constant 0)
F = lambda i: f"s->f[{i}]"
FI = lambda i: f"s->fi[{i}]"

def rs(w): return (w >> 21) & 0x1F
def rt(w): return (w >> 16) & 0x1F
def rd(w): return (w >> 11) & 0x1F
def sa(w): return (w >> 6) & 0x1F
def funct(w): return w & 0x3F
def simm(w): return f"0x{((w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF) & 0xFFFFFFFF:08x}u"
def zimm(w): return f"0x{w & 0xFFFF:x}u"
def s16(w): return (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF

def wr(i, expr):
    # Assignment to GPR i; writes to r0 are dropped (ARCHITECTURE section 4).
    return "(void)0;" if i == 0 else f"s->r[{i}] = {expr};"

def vreg_indices(reg, size):
    # Physical v[] indices for a VFPU vector register. size is lanes (1=single..4=quad).
    #
    # Origin: written from PPSSPP's voffset-integrated addressing
    # (MIPSVFPUUtils.cpp). That lineage is real and is retained.
    #
    # Authority: as of HQ-1 the correctness of this decode no longer rests on
    # agreement with an emulator. It was measured on a PSP-3001 (6.61-ARK,
    # 2/2 reproducible runs) and matched for all 128 scalar encodings and for
    # 14 selected wide encodings, including the two that discriminate between
    # candidate rules: triple width selects its row from bit 6 alone, and
    # transpose wraps as (row + lane) & 3 rather than saturating. See
    # fixtures/vfpu_addressing/hardware_vfpu_addr_001.json and issue #296.
    #
    # Boundary: 14 of 512 wide encodings were observed. The rest are covered
    # only by the derived cross-implementation tests in
    # tools/test_vfpu_addressing.py, which are not hardware evidence.
    # vreg_names(), mreg_index() and the packed-size choice below are NOT
    # covered by that measurement and still cite PPSSPP alone.
    mtx = (reg >> 2) & 7
    col = reg & 3
    transpose = (reg >> 5) & 1
    if size == 1:
        transpose = 0; row = (reg >> 5) & 3; length = 1
    elif size == 2:
        row = (reg >> 5) & 2; length = 2
    elif size == 3:
        row = (reg >> 6) & 1; length = 3
    else:
        row = (reg >> 5) & 2; length = 4
    out = []
    for i in range(length):
        if transpose:
            out.append(mtx * 16 + ((row + i) & 3) * 4 + col)
        else:
            out.append(mtx * 16 + col * 4 + ((row + i) & 3))
    return out

def vreg_names(reg, size):
    """VFPU scalar register names returned by PPSSPP GetVectorRegs().

    These are intentionally distinct from this runtime's packed v[] indices.  VROT's
    overlap quirk compares the encoded scalar source name against these names.
    """
    mtx=(reg>>2)&7; col=reg&3; transpose=(reg>>5)&1
    if size==1: transpose=0; row=(reg>>5)&3; length=1
    elif size==2: row=(reg>>5)&2; length=2
    elif size==3: row=(reg>>6)&1; length=3
    else: row=(reg>>5)&2; length=4
    out=[]
    for i in range(length):
        if transpose: out.append(mtx*4+((row+i)&3)+col*32)
        else: out.append(mtx*4+col+((row+i)&3)*32)
    return out

def vec_size(w):  # number of lanes from the VFPU size bits
    return (((w >> 7) & 1) | ((w >> 14) & 2)) + 1

def mreg_index(reg, side, j, i):
    # Physical v[] index of a matrix element (column j, row i), matching PPSSPP ReadMatrix.
    mtx = (reg >> 2) & 7
    col = reg & 3
    transpose = (reg >> 5) & 1
    if side == 1:
        transpose = 0; row = (reg >> 5) & 3
    elif side == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    if transpose:
        return mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3)
    return mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3)

class Unsupported(Exception):
    pass

# The HST PRX is linked/recompiled at guest address zero, while the original PSP
# process maps it above the unmapped null page. A few guest data-structure walkers
# intentionally read fields from their null terminator. Keep those reviewed data
# loads zero-valued without changing instruction dispatch or legitimate low .text
# addresses. This is relocation compatibility, not a loop cap.
NULL_BASE_WORD_LOADS = {
    0x0003E014: "command-list initial sentinel read",
    0x0003E04C: "command-list next sentinel read",
    0x0003E060: "command-list null completion callback",
    0x000705D4: "uninitialized job-queue count read",
}

# Effect of a non-control instruction -> (c_statement, store_addr_expr_or_None, store_size).
def effect(addr, w):
    op = w >> 26
    if op == 0:
        fn = funct(w)
        a, b, d, sh = rs(w), rt(w), rd(w), sa(w)
        if fn == 0x00: return wr(d, f"({R(b)} << {sh})"), None, 0           # sll
        if fn == 0x02: return wr(d, f"({R(b)} >> {sh})"), None, 0           # srl
        if fn == 0x03: return wr(d, f"((uint32_t)((int32_t){R(b)} >> {sh}))"), None, 0  # sra
        if fn == 0x04: return wr(d, f"({R(b)} << ({R(a)} & 31))"), None, 0  # sllv
        if fn == 0x06: return wr(d, f"({R(b)} >> ({R(a)} & 31))"), None, 0  # srlv
        if fn == 0x07: return wr(d, f"((uint32_t)((int32_t){R(b)} >> ({R(a)} & 31)))"), None, 0  # srav
        if fn == 0x0A: return f"if ({R(b)} == 0) {wr(d, R(a))}", None, 0    # movz
        if fn == 0x0B: return f"if ({R(b)} != 0) {wr(d, R(a))}", None, 0    # movn
        if fn == 0x0C:
            code = (w >> 6) & 0xFFFFF
            return f"sr_raw_syscall(s, {code}u, 0x{addr:08x}u); return;", None, 0        # syscall
        if fn == 0x0D:
            code = (w >> 6) & 0xFFFFF
            return f"sr_break(s, {code}u, 0x{addr:08x}u);", None, 0
        if fn == 0x10: return wr(d, "s->hi"), None, 0                       # mfhi
        if fn == 0x11: return f"s->hi = {R(a)};", None, 0                   # mthi
        if fn == 0x12: return wr(d, "s->lo"), None, 0                       # mflo
        if fn == 0x13: return f"s->lo = {R(a)};", None, 0                   # mtlo
        if fn == 0x16: return wr(d, f"({R(a)} == 0 ? 32u : (uint32_t)__builtin_clz({R(a)}))"), None, 0  # clz
        if fn == 0x17: return wr(d, f"({R(a)} == 0xFFFFFFFFu ? 32u : (uint32_t)__builtin_clz(~{R(a)}))"), None, 0  # clo
        if fn == 0x18: return f"{{ int64_t _p = (int64_t)(int32_t){R(a)} * (int64_t)(int32_t){R(b)}; s->lo = (uint32_t)_p; s->hi = (uint32_t)(_p >> 32); }}", None, 0  # mult
        if fn == 0x19: return f"{{ uint64_t _p = (uint64_t){R(a)} * (uint64_t){R(b)}; s->lo = (uint32_t)_p; s->hi = (uint32_t)(_p >> 32); }}", None, 0  # multu
        if fn == 0x1A: return ("{ int32_t _a=(int32_t)%s, _b=(int32_t)%s; if (_a==(int32_t)0x80000000 && _b==-1){s->lo=0x80000000u;s->hi=0xFFFFFFFFu;} "
                               "else if (_b!=0){s->lo=(uint32_t)(_a/_b);s->hi=(uint32_t)(_a%%_b);} else {s->lo=_a<0?1u:0xFFFFFFFFu;s->hi=(uint32_t)_a;} }") % (R(a), R(b)), None, 0  # div
        if fn == 0x1B: return ("{ uint32_t _a=%s,_b=%s; if(_b!=0){s->lo=_a/_b;s->hi=_a%%_b;} else {s->lo=_a<=0xFFFFu?0xFFFFu:0xFFFFFFFFu;s->hi=_a;} }") % (R(a), R(b)), None, 0  # divu
        # Allegrex multiply-accumulate: these are in the normal SPECIAL table (opcode 0),
        # NOT in generic MIPS32 SPECIAL2 (opcode 0x1C). Verified against PPSSPP MIPSTables.cpp
        # and the private HST ELF (0x0062001C at 0x00026BAC etc.).
        if fn == 0x1C: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; uint64_t _prod=(uint64_t)((int64_t)(int32_t){R(a)}*(int64_t)(int32_t){R(b)}); _acc+=_prod; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # madd
        if fn == 0x1D: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; _acc+=(uint64_t){R(a)}*(uint64_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # maddu
        if fn == 0x20 or fn == 0x21: return wr(d, f"({R(a)} + {R(b)})"), None, 0  # add/addu
        if fn == 0x22 or fn == 0x23: return wr(d, f"({R(a)} - {R(b)})"), None, 0  # sub/subu
        if fn == 0x24: return wr(d, f"({R(a)} & {R(b)})"), None, 0          # and
        if fn == 0x25: return wr(d, f"({R(a)} | {R(b)})"), None, 0          # or
        if fn == 0x26: return wr(d, f"({R(a)} ^ {R(b)})"), None, 0          # xor
        if fn == 0x27: return wr(d, f"(~({R(a)} | {R(b)}))"), None, 0       # nor
        if fn == 0x2A: return wr(d, f"((int32_t){R(a)} < (int32_t){R(b)} ? 1u : 0u)"), None, 0  # slt
        if fn == 0x2B: return wr(d, f"({R(a)} < {R(b)} ? 1u : 0u)"), None, 0  # sltu
        if fn == 0x2C: return wr(d, f"({{int32_t _x=(int32_t){R(a)},_y=(int32_t){R(b)}; (uint32_t)(_x>_y?_x:_y);}})"), None, 0  # max
        if fn == 0x2D: return wr(d, f"({{int32_t _x=(int32_t){R(a)},_y=(int32_t){R(b)}; (uint32_t)(_x<_y?_x:_y);}})"), None, 0  # min
        if fn == 0x2E: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; uint64_t _prod=(uint64_t)((int64_t)(int32_t){R(a)}*(int64_t)(int32_t){R(b)}); _acc-=_prod; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # msub
        if fn == 0x2F: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; _acc-=(uint64_t){R(a)}*(uint64_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # msubu
        raise Unsupported(f"SPECIAL funct 0x{fn:02x} at 0x{addr:08x}")
    if op == 0x08 or op == 0x09: return wr(rt(w), f"({R(rs(w))} + {simm(w)})"), None, 0  # addi/addiu
    if op == 0x0A: return wr(rt(w), f"((int32_t){R(rs(w))} < (int32_t){simm(w)} ? 1u : 0u)"), None, 0  # slti
    if op == 0x0B: return wr(rt(w), f"({R(rs(w))} < {simm(w)} ? 1u : 0u)"), None, 0  # sltiu
    if op == 0x0C: return wr(rt(w), f"({R(rs(w))} & {zimm(w)})"), None, 0  # andi
    if op == 0x0D: return wr(rt(w), f"({R(rs(w))} | {zimm(w)})"), None, 0  # ori
    if op == 0x0E: return wr(rt(w), f"({R(rs(w))} ^ {zimm(w)})"), None, 0  # xori
    if op == 0x0F: return wr(rt(w), f"({zimm(w)} << 16)"), None, 0          # lui
    if op == 0x1F:  # SPECIAL3
        fn = funct(w)
        if fn == 0x00:  # ext
            pos, size = sa(w), ((w >> 11) & 0x1F) + 1
            mask = 0xFFFFFFFF if size >= 32 else ((1 << size) - 1)
            return wr(rt(w), f"(({R(rs(w))} >> {pos}) & 0x{mask:x}u)"), None, 0
        if fn == 0x04:  # ins
            pos, msb = sa(w), (w >> 11) & 0x1F
            size = msb - pos + 1
            mask = ((0xFFFFFFFF if size >= 32 else ((1 << size) - 1)) << pos) & 0xFFFFFFFF
            return wr(rt(w), f"(({R(rt(w))} & ~0x{mask:x}u) | (({R(rs(w))} << {pos}) & 0x{mask:x}u))"), None, 0
        if fn == 0x20:
            sub = sa(w)
            if sub == 0x02: return wr(rd(w), f"((({R(rt(w))} & 0x00FF00FFu) << 8) | (({R(rt(w))} >> 8) & 0x00FF00FFu))"), None, 0  # wsbh
            if sub == 0x03: return wr(rd(w), f"((({R(rt(w))} & 0x000000FFu) << 24) | (({R(rt(w))} & 0x0000FF00u) << 8) | (({R(rt(w))} & 0x00FF0000u) >> 8) | (({R(rt(w))} & 0xFF000000u) >> 24))"), None, 0  # wsbw
            if sub == 0x10: return wr(rd(w), f"((uint32_t)(int32_t)(int8_t){R(rt(w))})"), None, 0   # seb
            if sub == 0x18: return wr(rd(w), f"((uint32_t)(int32_t)(int16_t){R(rt(w))})"), None, 0  # seh
            if sub == 0x14: return wr(rd(w), f"sr_bitrev({R(rt(w))})"), None, 0                      # bitrev
        raise Unsupported(f"SPECIAL3 funct 0x{fn:02x} at 0x{addr:08x}")
    # loads
    if op == 0x20: return wr(rt(w), f"((uint32_t)(int32_t)(int8_t)MEM_R8({R(rs(w))} + {simm(w)}))"), None, 0   # lb
    if op == 0x21: return wr(rt(w), f"((uint32_t)(int32_t)(int16_t)MEM_R16({R(rs(w))} + {simm(w)}))"), None, 0  # lh
    if op == 0x23:
        if addr in NULL_BASE_WORD_LOADS:
            return wr(rt(w), f"({R(rs(w))} == 0u ? 0u : MEM_R32({R(rs(w))} + {simm(w)}))"), None, 0
        return wr(rt(w), f"MEM_R32({R(rs(w))} + {simm(w)})"), None, 0   # lw
    if op == 0x24: return wr(rt(w), f"MEM_R8({R(rs(w))} + {simm(w)})"), None, 0    # lbu
    if op == 0x25: return wr(rt(w), f"MEM_R16({R(rs(w))} + {simm(w)})"), None, 0   # lhu
    # stores
    if op == 0x28: return f"MEM_W8_PC({R(rs(w))} + {simm(w)}, {R(rt(w))}, 0x{addr:08x}u);", f"({R(rs(w))} + {simm(w)})", 1   # sb
    if op == 0x29: return f"MEM_W16_PC({R(rs(w))} + {simm(w)}, {R(rt(w))}, 0x{addr:08x}u);", f"({R(rs(w))} + {simm(w)})", 2  # sh
    if op == 0x2B: return f"MEM_W32_PC({R(rs(w))} + {simm(w)}, {R(rt(w))}, 0x{addr:08x}u);", f"({R(rs(w))} + {simm(w)})", 4  # sw
    # Unaligned word access
    if op == 0x22: return wr(rt(w), f"sr_lwl({R(rt(w))}, {R(rs(w))} + {simm(w)})"), None, 0   # lwl
    if op == 0x26: return wr(rt(w), f"sr_lwr({R(rt(w))}, {R(rs(w))} + {simm(w)})"), None, 0   # lwr
    if op == 0x2A: return f"sr_swl_pc({R(rs(w))} + {simm(w)}, {R(rt(w))}, 0x{addr:08x}u);", f"(({R(rs(w))} + {simm(w)}) & ~3u)", 4  # swl
    if op == 0x2E: return f"sr_swr_pc({R(rs(w))} + {simm(w)}, {R(rt(w))}, 0x{addr:08x}u);", f"(({R(rs(w))} + {simm(w)}) & ~3u)", 4  # swr
    if op == 0x31: return f"s->fi[{rt(w)}] = MEM_R32({R(rs(w))} + {simm(w)});", None, 0  # lwc1
    if op == 0x39: return f"MEM_W32_PC({R(rs(w))} + {simm(w)}, s->fi[{rt(w)}], 0x{addr:08x}u);", f"({R(rs(w))} + {simm(w)})", 4  # swc1
    if op == 0x11: return fpu_effect(addr, w)
    if op == 0x2f: return "(void)0;", None, 0  # cache (no-op in user space static recompilation)
    if op == 0x3f: return f"if ((0x{w:08x}u & 0xFFFF0000u) != 0xFFFF0000u) {{ s->vfpuCtrl[0]=0xe4u; s->vfpuCtrl[1]=0xe4u; s->vfpuCtrl[2]=0u; }}", None, 0  # vflush
    if op in (0x35, 0x36, 0x3d, 0x3e, 0x32, 0x3a, 0x12, 0x18, 0x19, 0x1b, 0x37, 0x34, 0x3c): return vfpu_effect(addr, w)
    raise Unsupported(f"opcode 0x{op:02x} at 0x{addr:08x}")

def _arr(idx):
    return "(const uint8_t[]){" + ",".join(str(x) for x in idx) + "}"

_EAT = " s->vfpuCtrl[0]=0xe4u; s->vfpuCtrl[1]=0xe4u; s->vfpuCtrl[2]=0u;"

def _half_to_f32_bits(h):
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        if m == 0:
            return s << 31
        e2 = 127 - 15 + 1
        while not (m & 0x400):
            m <<= 1
            e2 -= 1
        return (s << 31) | (e2 << 23) | ((m & 0x3FF) << 13)
    if e == 31:
        return (s << 31) | (0xFF << 23) | (m << 13)
    return (s << 31) | ((e - 15 + 127) << 23) | (m << 13)

def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]

def _vfpu_cst():
    import math
    PI, E = math.pi, math.e
    c = [0.0] * 32
    c[1] = _f32(3.4028234663852886e38)               # FLT_MAX
    c[2] = _f32(math.sqrt(2.0))                      # sqrtf(2.0f)
    c[3] = _f32(math.sqrt(0.5))                      # sqrtf(0.5f)
    c[4] = _f32(2.0 / _f32(math.sqrt(_f32(PI))))     # 2.0f / sqrtf((float)PI)
    c[5] = _f32(2.0 / _f32(PI))
    c[6] = _f32(1.0 / _f32(PI))
    c[7] = _f32(_f32(PI) / 4)
    c[8] = _f32(_f32(PI) / 2)
    c[9] = _f32(PI)
    c[10] = _f32(E)
    c[11] = _f32(1.44269504088896340736)             # LOG2E
    c[12] = _f32(0.43429448190325182765)             # LOG10E
    c[13] = _f32(0.69314718055994530942)             # LN2
    c[14] = _f32(2.30258509299404568402)             # LN10
    c[15] = _f32(2 * _f32(PI))
    c[16] = _f32(_f32(PI) / 6)
    c[17] = _f32(math.log10(2.0))                    # log10f(2.0f)
    c[18] = _f32(_f32(math.log(10.0)) / _f32(math.log(2.0)))  # logf(10)/logf(2)
    c[19] = _f32(_f32(math.sqrt(3.0)) / 2.0)         # sqrtf(3.0f) / 2.0f
    return c

_VFPU_CST = _vfpu_cst()

def _flit(v):
    s = f"{v:.9g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"

def vfpu_effect(addr, w):
    op = w >> 26
    if op == 0x37:
        regnum = (w >> 24) & 3
        if regnum == 3:  # viim / vfim
            vt = (w >> 16) & 0x7F
            imm = w & 0xFFFF
            if (w >> 23) & 1:
                bits = _half_to_f32_bits(imm)
            else:
                iv = imm - 0x10000 if imm & 0x8000 else imm
                bits = struct.unpack("<I", struct.pack("<f", float(iv)))[0]
            i0 = vreg_indices(vt, 1)[0]
            body = (f"uint32_t _bits=0x{bits:08x}u; float _d[1]; memcpy(_d,&_bits,4); "
                    f"sr_vwrite(s,{_arr([i0])},_d,1,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        data = w & 0xFFFFF
        if regnum == 2:
            data &= 0xFFF
        return f"s->vfpuCtrl[{regnum}] = 0x{data:x}u;", None, 0
    if op == 0x34:
        vd, vs = w & 0x7F, (w >> 8) & 0x7F
        n = vec_size(w)
        jump = (w >> 21) & 0x1F
        di = vreg_indices(vd, n)
        if jump == 0x15: # vcmov
            tf = (w >> 19) & 1
            imm3 = (w >> 16) & 7
            si = vreg_indices(vs, n)
            rd_st = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                     f"sr_vread(_d,s,{_arr(di)},{n},s->vfpuCtrl[1]); ")
            if imm3 < 6:
                move = (f"if (((s->vfpuCtrl[3] >> {imm3}) & 1u) == {1 - tf}u) "
                        f"{{ for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]; }} ")
            elif imm3 == 6:
                move = (f"for(int _i=0;_i<{n};_i++) "
                        f"if (((s->vfpuCtrl[3] >> _i) & 1u) == {1 - tf}u) _d[_i]=_s[_i]; ")
            else:
                raise Unsupported(f"vcmov imm3 {imm3} at 0x{addr:08x}")
            body = rd_st + move + f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}"
            return "{ " + body + " }", None, 0
        if jump == 0x02:  # vocp
            op9 = (w >> 16) & 0x1F
            if op9 == 4:  # vocp: d = 1.0 - s
                si = vreg_indices(vs, n)
                body = (f"float _s[4],_t[4],_d[4]; "
                        f"sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]|0xF0000u); "
                        f"sr_vread(_t,s,{_arr(si)},{n},(s->vfpuCtrl[1]&~0xFFu)|0x55u|0xF000u); "
                        f"for(int _i=0;_i<{n};_i++) _d[_i]=isnan(_s[_i])?fabsf(_s[_i]):_t[_i]+_s[_i]; "
                        f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
                return "{ " + body + " }", None, 0
            raise Unsupported(f"VFPU9 op 0x{op9:02x} at 0x{addr:08x}")
        if jump == 0x03:  # vcst
            val = _flit(_VFPU_CST[(w >> 16) & 0x1F])
            body = (f"float _d[4]; for(int _i=0;_i<{n};_i++) _d[_i]={val}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if jump == 1:
            idx7 = (w >> 16) & 0x1F
            if idx7 == 27: # vs2i
                si = vreg_indices(vs, n)
                oz_n = 2
                sz_n = n
                if sz_n in (4, 3):
                    sz_n = 2
                    oz_n = 4
                elif sz_n == 2:
                    oz_n = 4
                num_src = sz_n
                lines = [
                    f"uint32_t _s[4], _d[4] = {{0}};",
                    f"sr_vread((float*)_s, s, {_arr(si[:num_src])}, {num_src}, s->vfpuCtrl[0]);",
                ]
                for i in range(num_src):
                    lines.append(f"_d[{i * 2}] = (_s[{i}] & 0xFFFFu) << 16;")
                    lines.append(f"_d[{i * 2 + 1}] = _s[{i}] & 0xFFFF0000u;")
                # Output is double-width (oz_n lanes): index the destination at oz_n,
                # not a slice of the source-sized list -- di[:oz_n] under-filled the C
                # compound literal when oz_n > n (vs2i.s/.p), making sr_vwrite read
                # past it (UB). The first n entries are unchanged by this fix.
                lines.append(f"sr_vwrite(s, {_arr(vreg_indices(vd, oz_n))}, (float*)_d, {oz_n}, s->vfpuCtrl[2]);{_EAT}")
                return "{ " + " ".join(lines) + " }", None, 0
            if idx7 in (28, 29, 30, 31): # vi2uc, vi2c, vi2us, vi2s
                case = idx7 - 28
                lines = [
                    f"int32_t _s[4]; uint32_t _d[2] = {{0}};",
                    f"sr_vread((float*)_s, s, {_arr(vreg_indices(vs, 4))}, 4, s->vfpuCtrl[0]);"
                ]
                if case == 0: # vi2uc
                    for i in range(4):
                        lines.append(f"{{ int32_t _v = _s[{i}]; if (_v < 0) _v = 0; _v >>= 23; _d[0] |= ((uint32_t)_v & 0xFFu) << {i * 8}; }}")
                    oz_n = 1
                elif case == 1: # vi2c
                    for i in range(4):
                        lines.append(f"{{ uint32_t _v = _s[{i}]; _d[0] |= ((_v >> 24) & 0xFFu) << {i * 8}; }}")
                    oz_n = 1
                elif case == 2: # vi2us
                    lines.append(f"int _elems = ({n} + 1) / 2;")
                    lines.append(f"for (int _i = 0; _i < _elems; _i++) {{")
                    lines.append(f"    int32_t _low = _s[_i * 2];")
                    lines.append(f"    int32_t _high = _s[_i * 2 + 1];")
                    lines.append(f"    if (_low < 0) _low = 0;")
                    lines.append(f"    if (_high < 0) _high = 0;")
                    lines.append(f"    _low >>= 15; _high >>= 15;")
                    lines.append(f"    _d[_i] = ((uint32_t)_low & 0xFFFFu) | (((uint32_t)_high & 0xFFFFu) << 16);")
                    lines.append(f"}}")
                    oz_n = 2 if n in (4, 3) else 1
                elif case == 3: # vi2s
                    lines.append(f"int _elems = ({n} + 1) / 2;")
                    lines.append(f"for (int _i = 0; _i < _elems; _i++) {{")
                    lines.append(f"    uint32_t _low = _s[_i * 2];")
                    lines.append(f"    uint32_t _high = _s[_i * 2 + 1];")
                    lines.append(f"    _low >>= 16; _high >>= 16;")
                    lines.append(f"    _d[_i] = (_low & 0xFFFFu) | (_high << 16);")
                    lines.append(f"}}")
                    oz_n = 2 if n in (4, 3) else 1
                # Destination is decoded at the packed size oz_n (PPSSPP GetVectorRegs
                # with oz), which differs from slicing the n-sized list for oz_n == 1
                # registers with the row bit set.
                lines.append(f"sr_vwrite(s, {_arr(vreg_indices(vd, oz_n))}, (float*)_d, {oz_n}, s->vfpuCtrl[2]);{_EAT}")
                return "{ " + " ".join(lines) + " }", None, 0
            raise Unsupported(f"VFPU7 op 0x{idx7:02x} at 0x{addr:08x}")
        if jump in (16, 17, 18, 19): # vf2i
            si = vreg_indices(vs, n)
            imm = (w >> 16) & 0x1F
            mult = float(1 << imm)
            lines = [
                f"float _s[4]; int32_t _d[4];",
                f"sr_vread(_s, s, {_arr(si)}, {n}, s->vfpuCtrl[0]);",
                f"for (int _i = 0; _i < {n}; _i++) {{",
                f"    if (isnan(_s[_i])) {{ _d[_i] = 0x7FFFFFFF; continue; }}",
                f"    double _sv = (double)_s[_i] * {mult:.9g};",
                f"    if (_sv > 2147483647.0) _d[_i] = 0x7FFFFFFF;",
                f"    else if (_sv <= -2147483648.0) _d[_i] = (int32_t)0x80000000;"
            ]
            if jump == 16: # vf2in
                lines.append(f"    else _d[_i] = (int32_t)nearbyint(_sv);")
            elif jump == 17: # vf2iz
                lines.append(f"    else _d[_i] = (int32_t)_sv;")
            elif jump == 18: # vf2iu
                lines.append(f"    else _d[_i] = (int32_t)ceil(_sv);")
            elif jump == 19: # vf2id
                lines.append(f"    else _d[_i] = (int32_t)floor(_sv);")
            lines.append(f"}}")
            lines.append(f"sr_vwrite(s, {_arr(di)}, (float*)_d, {n}, s->vfpuCtrl[2] & 0xFFFFFF00u);{_EAT}")
            return "{ " + " ".join(lines) + " }", None, 0
        if jump == 20: # vi2f
            si = vreg_indices(vs, n)
            imm = (w >> 16) & 0x1F
            mult = 1.0 / float(1 << imm)
            mult_str = f"{mult:.9g}"
            if "." not in mult_str and "e" not in mult_str:
                mult_str += ".0f"
            else:
                mult_str += "f"
            lines = [
                f"int32_t _s[4]; float _d[4];",
                f"sr_vread((float*)_s, s, {_arr(si)}, {n}, s->vfpuCtrl[0]);",
                f"for (int _i = 0; _i < {n}; _i++) _d[_i] = (float)_s[_i] * {mult_str};",
                f"sr_vwrite(s, {_arr(di)}, _d, {n}, s->vfpuCtrl[2]);{_EAT}"
            ]
            return "{ " + " ".join(lines) + " }", None, 0
        if jump != 0:
            raise Unsupported(f"VFPU4 jump 0x{jump:02x} at 0x{addr:08x}")
        optype = (w >> 16) & 0x1F
        if optype in (0, 1, 2, 4, 5):
            si = vreg_indices(vs, n)
            per = {0: "_s[_i]", 1: "fabsf(_s[_i])", 2: "-_s[_i]",
                   4: "(_s[_i]<=0.0f?0.0f:(_s[_i]>1.0f?1.0f:_s[_i]))",
                   5: "(_s[_i]<-1.0f?-1.0f:(_s[_i]>1.0f?1.0f:_s[_i]))"}[optype]
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++) _d[_i]={per}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        _TRANS = {16: "sr_vfpu_rcp(_s[_i])", 17: "sr_vfpu_rsqrt(_s[_i])",
                  18: "sr_vfpu_sin(_s[_i])", 19: "sr_vfpu_cos(_s[_i])",
                  20: "sr_vfpu_exp2(_s[_i])", 21: "sr_vfpu_log2(_s[_i])",
                  22: "sr_vfpu_sqrt(_s[_i])", 23: "sr_vfpu_asin(_s[_i])",
                  24: "-sr_vfpu_rcp(_s[_i])", 25: "-sr_vfpu_rsqrt(_s[_i])",
                  26: "-sr_vfpu_sin(_s[_i])", 27: "-sr_vfpu_cos(_s[_i])",
                  28: "-sr_vfpu_exp2(_s[_i])", 29: "-sr_vfpu_log2(_s[_i])",
                  30: "-sr_vfpu_sqrt(_s[_i])", 31: "-sr_vfpu_asin(_s[_i])"}
        if optype in _TRANS:
            si = vreg_indices(vs, n)
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++) _d[_i]={_TRANS[optype]}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if optype in (6, 7):  # vzero / vone
            val = "0.0f" if optype == 6 else "1.0f"
            body = (f"float _d[4]; for(int _i=0;_i<{n};_i++) _d[_i]={val}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if optype == 3:  # vidt
            offmask = 3 if n >= 3 else 1
            off = vd & offmask
            vals = ",".join("1.0f" if i == off else "0.0f" for i in range(n))
            body = (f"float _d[4]={{{vals}}}; sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        raise Unsupported(f"VV2Op optype {optype} at 0x{addr:08x}")
    # lvl.q/lvr.q/svl.q/svr.q: their left/right merge behavior lives in the
    # single-step interpreter so codegen cannot accidentally treat them as aligned lv.q.
    if op == 0x35 or op == 0x3d:
        base = f"({R(rs(w))} + {simm(w & 0xFFFFFFFC)})"
        # Guest code pages are not guaranteed to remain mapped in the runtime image.
        # Pass the instruction captured from the ELF directly instead of asking dispatch
        # to reread it from MEM[pc] (which can be zero after loader/data overlays).
        run = f"s->pc=0x{addr:08x}u; (void)sr_vfpu_interp(s,0x{w:08x}u);"
        return run, (base if op == 0x3d else None), (16 if op == 0x3d else 0)
    # lv.q / sv.q. Aligned accesses stay native; a dynamic alignment violation is
    # delegated to the same authoritative decoder as the explicit left/right forms.
    if op == 0x36 or op == 0x3e:
        vt = ((w >> 16) & 0x1F) | ((w & 1) << 5)
        idx = vreg_indices(vt, 4)
        base = f"({R(rs(w))} + {simm(w & 0xFFFFFFFC)})"
        if op == 0x36:  # lv.q
            # #184: the whole 16-byte span must be readable before any destination
            # lane commits. A straddling/wrapped aligned span falls back to the
            # authoritative interpreter, which rejects it all-or-nothing.
            parts = " ".join(f"s->vi[{idx[i]}] = MEM_R32(_a + {i*4});" for i in range(4))
            return (f"{{ uint32_t _a = {base}; if((_a&15u)==0 && sr_guest_span_readable(_a,16u)){{ {parts} }}else{{"
                    f"s->pc=0x{addr:08x}u; (void)sr_vfpu_interp(s,0x{w:08x}u); }} }}"), None, 0
        parts = " ".join(f"MEM_W32_PC(_a + {i*4}, s->vi[{idx[i]}], 0x{addr:08x}u);" for i in range(4))
        return (f"{{ uint32_t _a = {base}; if((_a&15u)==0 && sr_guest_span_writable(_a,16u)){{ {parts} }}else{{"
                f"s->pc=0x{addr:08x}u; (void)sr_vfpu_interp(s,0x{w:08x}u); }} }}"), base, 16  # sv.q
    # lv.s / sv.s
    if op == 0x32 or op == 0x3a:
        vt = ((w >> 16) & 0x1F) | ((w & 3) << 5)
        i0 = vreg_indices(vt, 1)[0]
        off = (w & 0xFFFC)
        off = off - 0x10000 if off & 0x8000 else off
        addr_e = f"({R(rs(w))} + {off})"
        if op == 0x32:  # lv.s
            return f"s->vi[{i0}] = MEM_R32({addr_e});", None, 0
        return f"MEM_W32_PC({addr_e}, s->vi[{i0}], 0x{addr:08x}u);", addr_e, 4  # sv.s
    # COP2 mfc2/mtc2
    if op == 0x12:
        sub = (w >> 21) & 0x1F
        imm = w & 0xFF
        if imm < 128:
            vidx = vreg_indices(imm, 1)[0]
            src, dst = f"s->vi[{vidx}]", f"s->vi[{vidx}]"
        else:
            src = dst = f"s->vfpuCtrl[{imm - 128}]"
        if sub == 3:    # mfv/mfvc
            return wr(rt(w), src), None, 0
        if sub == 7:    # mtv/mtvc
            return f"{dst} = {R(rt(w))};", None, 0
        raise Unsupported(f"cop2 sub {sub} at 0x{addr:08x}")
    # VFPU0 / VFPU1
    vd, vs, vt = w & 0x7F, (w >> 8) & 0x7F, (w >> 16) & 0x7F
    n = vec_size(w)
    sub = (w >> 23) & 7
    di, si = vreg_indices(vd, n), vreg_indices(vs, n)
    if op == 0x1b:
        ti = vreg_indices(vt, n)
        if sub == 0:  # vcmp
            cond = w & 0xF
            _C = {0: "0", 1: "_x==_y", 2: "_x<_y", 3: "_x<=_y", 4: "1", 5: "_x!=_y",
                  6: "_x>=_y", 7: "_x>_y", 8: "_x==0.0f", 9: "isnan(_x)", 10: "isinf(_x)",
                  11: "(isnan(_x)||isinf(_x))", 12: "_x!=0.0f", 13: "!isnan(_x)",
                  14: "!isinf(_x)", 15: "!(isnan(_x)||isinf(_x))"}[cond]
            body = (f"float _a[4],_b[4]; sr_vread(_a,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"sr_vread(_b,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                    f"int _cc=0,_or=0,_and=1,_aff=(1<<4)|(1<<5); "
                    f"for(int _i=0;_i<{n};_i++){{ float _x=_a[_i],_y=_b[_i]; int _c=({_C}); "
                    f"_cc|=(_c<<_i);_or|=_c;_and&=_c;_aff|=1<<_i; }} "
                    f"s->vfpuCtrl[3]=(s->vfpuCtrl[3]&~_aff)|((_cc|(_or<<4)|(_and<<5))&_aff);{_EAT}")
            return "{ " + body + " }", None, 0
        if sub in (2, 3):  # vmin / vmax
            ismin = "1" if sub == 2 else "0"
            body = (f"float _a[4],_b[4],_d[4]; sr_vread(_a,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"sr_vread(_b,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                    f"for(int _i=0;_i<{n};_i++){{ int _an=isnan(_a[_i])||isinf(_a[_i]),_bn=isnan(_b[_i])||isinf(_b[_i]); "
                    f"if(_an||_bn){{ int32_t _ai,_bi,_r; memcpy(&_ai,&_a[_i],4); memcpy(&_bi,&_b[_i],4); "
                    f"if({ismin}) _r=(_ai<0&&_bi<0)?(_bi<_ai?_ai:_bi):(_ai<_bi?_ai:_bi); "
                    f"else _r=(_ai<0&&_bi<0)?(_ai<_bi?_ai:_bi):(_bi<_ai?_ai:_bi); memcpy(&_d[_i],&_r,4); }} "
                    f"else _d[_i]={ismin}?(_a[_i]<_b[_i]?_a[_i]:_b[_i]):(_b[_i]<_a[_i]?_a[_i]:_b[_i]); }} "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if sub in (6, 7): # vcmovt / vcmovf
            tf = sub & 1
            imm3 = (w >> 16) & 7
            lines = [
                f"float _s[4], _d[4];",
                f"sr_vread(_s, s, {_arr(si)}, {n}, s->vfpuCtrl[0]);",
                f"sr_vread(_d, s, {_arr(di)}, {n}, s->vfpuCtrl[1]);"
            ]
            if imm3 < 6:
                lines.append(f"if (((s->vfpuCtrl[3] >> {imm3}) & 1u) == {1 - tf}u) {{ for (int _i = 0; _i < {n}; _i++) _d[_i] = _s[_i]; }}")
            elif imm3 == 6:
                lines.append(f"for (int _i = 0; _i < {n}; _i++) if (((s->vfpuCtrl[3] >> _i) & 1u) == {1 - tf}u) _d[_i] = _s[_i];")
            else:
                raise Unsupported(f"vcmov imm3 {imm3} at 0x{addr:08x}")
            lines.append(f"sr_vwrite(s, {_arr(di)}, _d, {n}, s->vfpuCtrl[2]);{_EAT}")
            return "{ " + " ".join(lines) + " }", None, 0
        raise Unsupported(f"VFPU3 sub {sub} at 0x{addr:08x}")
    if op == 0x3c and sub == 7 and ((w >> 21) & 0x1F) == 28:
        which = (w >> 16) & 0xF
        if which in (3, 6, 7):
            side = n
            writes = []
            for j in range(side):
                for i in range(side):
                    if which == 3:    val = "1.0f" if i == j else "0.0f"
                    elif which == 6:  val = "0.0f"
                    else:             val = "1.0f"
                    writes.append(f"s->v[{mreg_index(vd, side, j, i)}]={val};")
            return "{ " + " ".join(writes) + _EAT + " }", None, 0
        if which == 0:  # vmmov
            side = n
            writes = []
            for row in range(side - 1):
                for col in range(side):
                    writes.append(f"s->v[{mreg_index(vd, side, col, row)}] = s->v[{mreg_index(vs, side, col, row)}];")
            last_row_vs = [mreg_index(vs, side, col, side - 1) for col in range(side)]
            last_row_vd = [mreg_index(vd, side, col, side - 1) for col in range(side)]
            writes.append(f"float _s[4]; sr_vread(_s, s, {_arr(last_row_vs)}, {side}, s->vfpuCtrl[0]);")
            writes.append(f"sr_vwrite(s, {_arr(last_row_vd)}, _s, {side}, s->vfpuCtrl[2]);")
            return "{ " + " ".join(writes) + _EAT + " }", None, 0
        if which <= 7:  # vmscl
            scalar_vidx = vreg_indices(which & 7, 1)[0]
            side = n
            writes = []
            for row in range(side - 1):
                for col in range(side):
                    writes.append(f"s->v[{mreg_index(vd, side, col, row)}] = s->v[{mreg_index(vs, side, col, row)}] * s->v[{scalar_vidx}];")
            last_row_vs = [mreg_index(vs, side, col, side - 1) for col in range(side)]
            last_row_vd = [mreg_index(vd, side, col, side - 1) for col in range(side)]
            writes.append(f"float _s[4], _t[4]; sr_vread(_s, s, {_arr(last_row_vs)}, {side}, s->vfpuCtrl[0]);")
            writes.append(f"sr_vread(_t, s, (const uint8_t[]){{{scalar_vidx},{scalar_vidx},{scalar_vidx},{scalar_vidx}}}, {side}, s->vfpuCtrl[1]);")
            writes.append(f"float _d[4]; for (int _i = 0; _i < {side}; _i++) _d[_i] = _s[_i] * _t[_i];")
            writes.append(f"sr_vwrite(s, {_arr(last_row_vd)}, _d, {side}, s->vfpuCtrl[2]);")
            return "{ " + " ".join(writes) + _EAT + " }", None, 0
        raise Unsupported(f"VFPUMatrix1 which {which} at 0x{addr:08x}")
    if op == 0x3c and sub == 5:
        si, ti, di = vreg_indices(vs, n), vreg_indices(vt, n), vreg_indices(vd, n)
        if n == 4:
            body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},4,s->vfpuCtrl[0]); "
                    f"sr_vread(_t,s,{_arr(ti)},4,s->vfpuCtrl[1]); "
                    f"_d[0]=_s[0]*_t[3]+_s[1]*_t[2]-_s[2]*_t[1]+_s[3]*_t[0]; "
                    f"_d[1]=-_s[0]*_t[2]+_s[1]*_t[3]+_s[2]*_t[0]+_s[3]*_t[1]; "
                    f"_d[2]=_s[0]*_t[1]-_s[1]*_t[0]+_s[2]*_t[3]+_s[3]*_t[2]; "
                    f"_d[3]=-_s[0]*_t[0]-_s[1]*_t[1]-_s[2]*_t[2]+_s[3]*_t[3]; "
                    f"sr_vwrite(s,{_arr(di)},_d,4,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if n == 3:
            body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},3,s->vfpuCtrl[0]); "
                    f"sr_vread(_t,s,{_arr(ti)},3,s->vfpuCtrl[1]); "
                    f"_d[0]=_s[1]*_t[2]-_s[2]*_t[1]; _d[1]=_s[2]*_t[0]-_s[0]*_t[2]; _d[2]=_s[0]*_t[1]-_s[1]*_t[0]; "
                    f"sr_vwrite(s,{_arr(di)},_d,3,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        raise Unsupported(f"vcrsp/vqmul size {n} at 0x{addr:08x}")
    if op == 0x3c and sub == 7 and ((w >> 21) & 0x1F) == 29:
        di = vreg_indices(vd, n)
        ang = vreg_indices(vs, 1)[0]
        imm = (w >> 16) & 0x1F
        neg = "-" if (imm & 0x10) else ""
        sine_lane, cos_lane = (imm >> 2) & 3, imm & 3
        lines = [f"float _a=s->v[{ang}]; float _si={neg}sr_vfpu_sin(_a),_co=sr_vfpu_cos(_a); float _d[4]={{0,0,0,0}};"]
        if sine_lane == cos_lane:
            for i in range(n):
                lines.append(f"_d[{i}]=_si;")
            lines.append(f"_d[{cos_lane}]=_co;")
        else:
            lines.append(f"_d[{sine_lane}]=_si; _d[{cos_lane}]=_co;")
        if ((vd >> 2) & 7) == ((vs >> 2) & 7):
            dnames=vreg_names(vd,n)
            if vs in dnames:
                lines.append(f"_d[{cos_lane}]=sr_vfpu_cos(_d[{dnames.index(vs)}]);")
        # PSP ignores destination saturation and write-mask bits for the cosine lane.
        dmask=(3<<cos_lane)|(1<<(8+cos_lane))
        lines.append(f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]&~0x{dmask:x}u);{_EAT}")
        return "{ " + " ".join(lines) + " }", None, 0
    if (op == 0x18 and sub in (0, 1, 7)) or (op == 0x19 and sub == 0):
        ti = vreg_indices(vt, n)
        oper = {(0x18, 0): "+", (0x18, 1): "-", (0x18, 7): "/", (0x19, 0): "*"}[(op, sub)]
        body = (f"float _s[4],_t[4],_d[4]; "
                f"sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]{oper}_t[_i]; "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 1:  # vdot
        ti = vreg_indices(vt, n)
        dst = vreg_indices(vd, 1)[0]
        body = (f"float _s[4],_t[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"float _d=0.0f; for(int _i=0;_i<{n};_i++) _d+=_s[_i]*_t[_i]; "
                f"float _dd[1]={{_d}}; sr_vwrite(s,{_arr([dst])},_dd,1,s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 4:  # vhdp
        ti = vreg_indices(vt, n)
        dst = vreg_indices(vd, 1)[0]
        terms = "+".join(f"_s[{i}]*_t[{i}]" for i in range(n - 1)) + f"+1.0f*_t[{n - 1}]"
        body = (f"float _s[4],_t[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"float _d={terms}; _d=isnan(_d)?fabsf(_d):_d; "
                f"float _dd[1]={{_d}}; sr_vwrite(s,{_arr([dst])},_dd,1,s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 5:  # vcrs
        if n != 3:
            raise Unsupported(f"vcrs size {n} at 0x{addr:08x}")
        ti = vreg_indices(vt, n)
        di = vreg_indices(vd, n)
        ss, ts = (1, 2, 0, 3), (2, 0, 1, 3)
        muls = " ".join(f"_d[{i}]=_s[{ss[i]}]*_t[{ts[i]}];" for i in range(n))
        body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); {muls} "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x3c and sub == 0:  # vmmul
        side = vec_size(w)
        lines = []
        for a in range(side):
            for b in range(side):
                terms = "+".join(f"s->v[{mreg_index(vs, side, b, c)}]*s->v[{mreg_index(vt, side, a, c)}]"
                                 for c in range(side))
                lines.append(f"float _m{a}_{b}=0.0f+{terms};")
        writes = " ".join(f"s->v[{mreg_index(vd, side, a, b)}]=_m{a}_{b};"
                          for a in range(side) for b in range(side))
        return "{ " + " ".join(lines) + " " + writes + _EAT + " }", None, 0
    if op == 0x3c and sub in (1, 2, 3):  # vtfm
        ins = sub
        side = ins + 1
        tn = min(n, ins + 1)
        ti = vreg_indices(vt, side)
        di = vreg_indices(vd, side)
        lines = []
        for i in range(side):
            terms = [f"s->v[{mreg_index(vs, side, i, k)}]*s->v[{ti[k]}]" for k in range(tn)]
            if ins >= n:
                terms.append(f"s->v[{mreg_index(vs, side, i, ins)}]")
            lines.append(f"float _v{i}=0.0f+{'+'.join(terms)};")
        writes = " ".join(f"s->v[{di[i]}]=_v{i};" for i in range(side))
        return "{ " + " ".join(lines) + " " + writes + _EAT + " }", None, 0
    if op == 0x19 and sub == 2:  # vscl
        scalar = vreg_indices(vt, 1)[0]
        body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"float _sc=s->v[{scalar}]; "
                f"for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]*_sc; "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x3c and sub == 4:  # vmscl
        vt = (w >> 16) & 0x7F
        scalar_vidx = vreg_indices(vt, 1)[0]
        side = n
        writes = []
        for row in range(side - 1):
            for col in range(side):
                writes.append(f"s->v[{mreg_index(vd, side, col, row)}] = s->v[{mreg_index(vs, side, col, row)}] * s->v[{scalar_vidx}];")
        last_row_vs = [mreg_index(vs, side, col, side - 1) for col in range(side)]
        last_row_vd = [mreg_index(vd, side, col, side - 1) for col in range(side)]
        writes.append(f"float _s[4], _t[4]; sr_vread(_s, s, {_arr(last_row_vs)}, {side}, s->vfpuCtrl[0]);")
        writes.append(f"sr_vread(_t, s, (const uint8_t[]){{{scalar_vidx},{scalar_vidx},{scalar_vidx},{scalar_vidx}}}, {side}, s->vfpuCtrl[1]);")
        writes.append(f"float _d[4]; for (int _i = 0; _i < {side}; _i++) _d[_i] = _s[_i] * _t[_i];")
        writes.append(f"sr_vwrite(s, {_arr(last_row_vd)}, _d, {side}, s->vfpuCtrl[2]);")
        return "{ " + " ".join(writes) + _EAT + " }", None, 0

    raise Unsupported(f"VFPU opcode 0x{op:02x} sub 0x{sub:x} at 0x{addr:08x}")

def fpu_effect(addr, w):
    fmt = rs(w); ft = rt(w); fs = rd(w); fdv = sa(w)
    if fmt == 0x00: return wr(rt(w), f"s->fi[{fs}]"), None, 0          # mfc1
    if fmt == 0x02: return wr(rt(w), f"({fs} == 31 ? s->fcr31 : ({fs} == 0 ? 0x00003351u : 0u))"), None, 0  # cfc1
    if fmt == 0x04: return f"s->fi[{fs}] = {R(rt(w))};", None, 0       # mtc1
    if fmt == 0x06: return (f"if ({fs} == 31) s->fcr31 = {R(rt(w))};"), None, 0  # ctc1
    if fmt == 0x10:
        fn = funct(w)
        if fn == 0x00: return f"{F(fdv)} = {F(fs)} + {F(ft)};", None, 0
        if fn == 0x01: return f"{F(fdv)} = {F(fs)} - {F(ft)};", None, 0
        if fn == 0x02: return f"{{ float _a={F(fs)},_b={F(ft)}; if((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) s->fi[{fdv}]=0x7fc00000u; else {F(fdv)}=_a*_b; }}", None, 0
        if fn == 0x03: return f"{F(fdv)} = {F(fs)} / {F(ft)};", None, 0
        if fn == 0x04: return f"{F(fdv)} = sqrtf({F(fs)});", None, 0
        if fn == 0x05: return f"{F(fdv)} = fabsf({F(fs)});", None, 0
        if fn == 0x06: return f"{F(fdv)} = {F(fs)};", None, 0
        if fn == 0x07: return f"{F(fdv)} = -{F(fs)};", None, 0
        if fn in (0x0C, 0x0D, 0x0E, 0x0F, 0x24):
            return f"s->fi[{fdv}] = sr_to_w({F(fs)}, 0x{fn:02x});", None, 0
        if fn >= 0x30:
            cond = fn & 0xF
            return (f"{{ float _a={F(fs)},_b={F(ft)}; int _u=isnan(_a)||isnan(_b); int _l=!_u&&_a<_b; int _e=!_u&&_a==_b; "
                    f"s->fpcond = ((_u&&({cond}&1))||(_e&&({cond}&2))||(_l&&({cond}&4)))?1u:0u; }}"), None, 0
    if fmt == 0x14 and funct(w) == 0x20: return f"{F(fdv)} = (float)(int32_t)s->fi[{fs}];", None, 0  # cvt.s.w
    raise Unsupported(f"COP1 fmt 0x{fmt:02x} funct 0x{funct(w):02x} at 0x{addr:08x}")

def is_cond_branch(w):
    op = w >> 26
    return op in (4, 5, 6, 7, 20, 21, 22, 23) or op == 1 or (op == 0x11 and rs(w) == 8) or (op == 0x12 and rs(w) == 8)

def cond_expr(w):
    op = w >> 26
    if op == 4: return f"({R(rs(w))} == {R(rt(w))})"          # beq
    if op == 5: return f"({R(rs(w))} != {R(rt(w))})"          # bne
    if op == 6: return f"((int32_t){R(rs(w))} <= 0)"          # blez
    if op == 7: return f"((int32_t){R(rs(w))} > 0)"           # bgtz
    if op == 20: return f"({R(rs(w))} == {R(rt(w))})"         # beql
    if op == 21: return f"({R(rs(w))} != {R(rt(w))})"         # bnel
    if op == 22: return f"((int32_t){R(rs(w))} <= 0)"         # blezl
    if op == 23: return f"((int32_t){R(rs(w))} > 0)"          # bgtzl
    if op == 1:
        sub = rt(w)
        if sub in (0, 2, 0x10): return f"((int32_t){R(rs(w))} < 0)"   # bltz/bltzl/bltzal
        if sub in (1, 3, 0x11): return f"((int32_t){R(rs(w))} >= 0)"  # bgez/bgezl/bgezal
    if op == 0x11 and rs(w) == 8:
        tf = (w >> 16) & 1
        return f"(s->fpcond {'!=' if tf else '=='} 0)"        # bc1t/bc1f
    if op == 0x12 and rs(w) == 8:
        tf = (w >> 16) & 1
        cc = (w >> 18) & 7
        return f"(((s->vfpuCtrl[3] >> {cc}) & 1u) {'!=' if tf else '=='} 0u)"  # bc2t/bc2f
    raise Unsupported(f"branch op 0x{op:02x}")

def is_likely(w):
    op = w >> 26
    return op in (20, 21, 22, 23) or (op == 1 and rt(w) in (2, 3)) or (op == 0x11 and rs(w) == 8 and ((w >> 17) & 1)) or (op == 0x12 and rs(w) == 8 and ((w >> 17) & 1))

def is_link(w):  # branch that also writes $ra
    return (w >> 26) == 1 and rt(w) in (0x10, 0x11)

def branch_target(addr, w):
    return (addr + 4 + (s16(w) << 2)) & 0xFFFFFFFF

def jump_target(addr, w):
    return ((addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) & 0xFFFFFFFF

def read32(elf, addr):
    b = elf.read_at_vaddr(addr, 4)
    return int.from_bytes(b, 'little') if b and len(b) >= 4 else None

def is_control(w):
    op = w >> 26
    fn = w & 0x3F
    return op in (2, 3) or (op == 0 and fn in (0x08, 0x09)) or is_cond_branch(w)

def function_flow(elf, start, ranges, known, resume_owners=None):
    insns, labels, seen = set(), set(), set()
    resume_owners = resume_owners or {}
    # A translated entry may linearly run into another address-taken entry.  This is
    # common for switch cases (and extremely common in the hand-written libc code),
    # but it also marks real adjacent-function boundaries such as f_0000d518 ->
    # f_0000d530.  Keep the bodies disjoint and represent the fall-through explicitly
    # instead of either swallowing the next function or truncating execution there.
    #
    # Keyed by the call/non-control instruction that owns the fall-through edge;
    # there can be several such exits in one control-flow graph.  Conditional branch
    # successors (both taken and not taken) remain native labels in this body.  Splitting
    # branch-connected regions creates continuation cycles and would recursively consume
    # the host stack for perfectly ordinary guest loops; straight-line/call boundaries in
    # this image form an acyclic graph (maximum observed depth 34).
    continuations = {}

    def is_entry_boundary(next_pc):
        if next_pc == start:
            return False
        if next_pc in known:
            return True
        owner = resume_owners.get(next_pc)
        # A resume label belongs natively to its owner and to its own alternate
        # host entry.  It remains an external entry boundary for every other body.
        return owner is not None and start not in (owner, next_pc)

    def is_jump_boundary(target_pc):
        # Preserve the existing callable/self-jump contract.  Only a resume label
        # owned by this body (or the resume body itself) is a native jump target.
        if target_pc in known:
            return True
        owner = resume_owners.get(target_pc)
        return owner is not None and start not in (owner, target_pc)

    def stop_at_continuation(owner, next_pc):
        if is_entry_boundary(next_pc):
            previous = continuations.setdefault(owner, next_pc)
            if previous != next_pc:
                raise AssertionError(
                    f"conflicting continuation at 0x{owner:08x}: "
                    f"0x{previous:08x} vs 0x{next_pc:08x}"
                )
            return True
        return False

    stack = [start]
    while stack:
        pc = stack.pop()
        while in_ranges(pc, ranges) and pc not in seen:
            seen.add(pc)
            insns.add(pc)
            w = read32(elf, pc)
            if w is None:
                break
            op, fn = w >> 26, w & 0x3F
            if op == 3 or (op == 0 and fn == 0x09):  # jal / jalr: call, returns
                insns.add(pc + 4)
                seen.add(pc + 4)
                next_pc = pc + 8
                if stop_at_continuation(pc, next_pc):
                    break
                pc = next_pc
                continue
            if op == 2:  # j
                t = jump_target(pc, w)
                insns.add(pc + 4)
                seen.add(pc + 4)
                if not is_jump_boundary(t) and in_ranges(t, ranges):  # intra goto
                    labels.add(t)
                    stack.append(t)
                break
            if op == 0 and fn == 0x08:  # jr: return / computed / tail
                insns.add(pc + 4)
                seen.add(pc + 4)
                break
            if op == 0 and fn == 0x0C:  # syscall: HLE boundary
                break
            if is_cond_branch(w):
                t = branch_target(pc, w)
                # A conditional target is a native label only when it remains
                # inside this translated body.  Cross-module/foreign entry
                # targets must dispatch just like an indirect branch; adding
                # them to ``labels`` without visiting them leaves an undefined
                # C goto in the generated translation unit.
                if in_ranges(t, ranges):
                    labels.add(t)
                    stack.append(t)
                insns.add(pc + 4)
                seen.add(pc + 4)
                # Both successors belong to one native control-flow region.  In
                # particular, do not turn a loop's not-taken edge into a host call.
                pc += 8
                continue
            next_pc = pc + 4
            if stop_at_continuation(pc, next_pc):
                break
            pc = next_pc
    return insns, labels, continuations

def normal_line(addr, w):
    try:
        eff, saddr, ssize = effect(addr, w)
    except Unsupported:
        # Keep the owning function translatable when the static emitter does not know a
        # VFPU form. Invoke the single-step interpreter with the ELF opcode literal;
        # runtime guest code pages may be unmapped or overlaid and cannot be reread by PC.
        if (w >> 26) not in (0x12,0x18,0x19,0x1b,0x32,0x34,0x35,0x36,0x37,0x3a,0x3c,0x3d,0x3e,0x3f):
            raise
        eff = f"s->pc=0x{addr:08x}u; (void)sr_vfpu_interp(s,0x{w:08x}u);"
        saddr, ssize = None, 0
    return f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {eff} sr_end(s, {saddr if saddr else '0u'}, {ssize});"

# ---------------------------------------------------------------------------
# Offline static verification trace simulation (--static-verify).
#
# With no external oracle traces available, verification assertions are derived
# at compile time instead: each maximal straight-line run of instructions inside
# a basic block is abstractly interpreted over a Known/Unknown register lattice
# (all registers Unknown at every block head, r0 = 0). Any register holding a
# compile-time constant at the end of a run -- constant materialization
# (lui/addiu/ori chains) and pure ALU folds over already-known values -- becomes
# an sr_sv_check assertion attached to the run's final instruction. At runtime
# the recompiled code must reproduce exactly those values, so the assertions
# detect lifter/register-allocation regressions locally, per block, without any
# oracle input. The model is sound-by-construction: anything not modeled
# bit-exactly degrades to Unknown (never asserted), and any opcode with unclear
# side effects resets the whole lattice.
#
# Default builds are unchanged: nothing is emitted unless codegen runs with
# --static-verify (keeps the -O0 -w chunk-size budget intact).

SV_ENABLED = False
SV_MAX_PER_FLUSH = 3     # checks attached to one flush point
SV_MAX_PER_FUNC = 16     # checks per function (bounds chunk growth)
SV_STATS = {"funcs": 0, "checks": 0}

# Diagnostic probe emission. When False (default/production), the temporary
# TOKENSCAN_DIAG and F3G_ENTRY fprintf probes are completely omitted from the
# generated C. Set True only for debug runs.
EMIT_DIAG_PROBES = False

# Functions carrying hand-injected code (fastpaths, probes, condition
# overrides) execute semantics the simulator does not model; exclude them.
_SV_SPECIAL = {0x0006e9bc, 0x0006ea1c, 0x00108630, 0x0010433c}

# GUEST_PATCHES -- reviewed, traceable per-guest instruction overrides.
# Each entry carries a rationale + issue id so every hand-patch is auditable.
# kind:
#   "cond"   : force the branch condition to `value` (always/never taken) and
#              optionally inject `stmt`.
#   "inject" : unconditionally inject `stmt` into the branch block.
#   "probe"  : emit a one-shot fprintf probe at function entry (no semantic
#              change; diagnostic only).
_GUEST_PATCH_KEYS = {0x00010950, 0x00048320, 0x0004cdc8}
GUEST_PATCHES = {
    0x00010950: {"kind": "cond", "value": "0 /* bypass loop 0x10950 */",
                 "stmt": " s->r[16] = s->r[3];",
                 "why": "bypass spin loop at 0x10950 (worker frame init)",
                 "issue": "#5.1"},
    0x00048320: {"kind": "inject", "stmt": " _c = 1u;",
                 "why": "force single-iteration pass at 0x48320",
                 "issue": "#5.1"},
    0x0004cdc8: {"kind": "cond", "value": "1u",
                 "why": "route host0 asset reads through sceIoOpen so HLE can serve extracted XB files; the guest-only host filesystem is unavailable",
                 "issue": "P0 table root 0x0034a84c"},
}
# Static-verify exclusions: the four stand-alone specials plus every patched addr.
_SV_SPECIAL |= _GUEST_PATCH_KEYS

# Opcodes that write no GPR at all (FPU/VFPU compute + loads/stores to
# coprocessors, plain stores, cache/pref).
_SV_NOGPR_OPS = {0x18, 0x19, 0x1B, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F,
                 0x31, 0x32, 0x33, 0x34, 0x37, 0x39, 0x3A, 0x3C,
                 0x35, 0x36, 0x3D, 0x3E}
# Opcodes whose only GPR effect is writing rt with a runtime-dependent value.
_SV_RT_UNKNOWN_OPS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x30}

def _sv_s32(v):
    return v - 0x100000000 if v & 0x80000000 else v

def _sv_step(w, regs, written):
    """Advance the Known/Unknown lattice by one non-control instruction.

    Mirrors effect()'s emitted C bit-for-bit for every modeled opcode; any
    imprecision is one-directional (Known -> Unknown)."""
    op = w >> 26
    a, b, d, sh, fn = rs(w), rt(w), rd(w), sa(w), funct(w)

    def W(i, v):
        if i != 0:
            regs[i] = (v & 0xFFFFFFFF) if v is not None else None
            written.add(i)

    def bin2(i, x, y, f):
        W(i, f(x, y) if x is not None and y is not None else None)

    if op == 0:
        if fn in (0x0C, 0x0D):  # syscall / break: HLE may mutate anything
            for i in range(1, 32):
                regs[i] = None
            return
        if fn == 0x00: W(d, regs[b] << sh if regs[b] is not None else None); return           # sll
        if fn == 0x02: W(d, regs[b] >> sh if regs[b] is not None else None); return           # srl
        if fn == 0x03: W(d, (_sv_s32(regs[b]) >> sh) if regs[b] is not None else None); return  # sra
        if fn == 0x04: bin2(d, regs[b], regs[a], lambda x, y: x << (y & 31)); return          # sllv
        if fn == 0x06: bin2(d, regs[b], regs[a], lambda x, y: x >> (y & 31)); return          # srlv
        if fn == 0x07: bin2(d, regs[b], regs[a], lambda x, y: _sv_s32(x) >> (y & 31)); return # srav
        if fn == 0x0A:  # movz
            if regs[b] == 0: W(d, regs[a])
            elif regs[b] is None: W(d, None)
            return
        if fn == 0x0B:  # movn
            if regs[b] is not None and regs[b] != 0: W(d, regs[a])
            elif regs[b] is None: W(d, None)
            return
        if fn in (0x10, 0x12): W(d, None); return       # mfhi / mflo (hi/lo untracked)
        if fn in (0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D): return  # mthi/mtlo/mult/div family: no GPR dest
        if fn == 0x16: W(d, (32 if regs[a] == 0 else 32 - regs[a].bit_length()) if regs[a] is not None else None); return  # clz
        if fn == 0x17: W(d, (32 if regs[a] == 0xFFFFFFFF else 32 - ((~regs[a]) & 0xFFFFFFFF).bit_length()) if regs[a] is not None else None); return  # clo
        if fn in (0x20, 0x21): bin2(d, regs[a], regs[b], lambda x, y: x + y); return          # add/addu
        if fn in (0x22, 0x23): bin2(d, regs[a], regs[b], lambda x, y: x - y); return          # sub/subu
        if fn == 0x24: bin2(d, regs[a], regs[b], lambda x, y: x & y); return
        if fn == 0x25: bin2(d, regs[a], regs[b], lambda x, y: x | y); return
        if fn == 0x26: bin2(d, regs[a], regs[b], lambda x, y: x ^ y); return
        if fn == 0x27: bin2(d, regs[a], regs[b], lambda x, y: ~(x | y)); return
        if fn == 0x2A: bin2(d, regs[a], regs[b], lambda x, y: 1 if _sv_s32(x) < _sv_s32(y) else 0); return  # slt
        if fn == 0x2B: bin2(d, regs[a], regs[b], lambda x, y: 1 if x < y else 0); return      # sltu
    elif op in (0x08, 0x09):   # addi/addiu
        W(b, regs[a] + s16(w) if regs[a] is not None else None); return
    elif op == 0x0A:           # slti
        W(b, (1 if _sv_s32(regs[a]) < s16(w) else 0) if regs[a] is not None else None); return
    elif op == 0x0B:           # sltiu (unsigned compare against sign-extended imm)
        W(b, (1 if regs[a] < (s16(w) & 0xFFFFFFFF) else 0) if regs[a] is not None else None); return
    elif op == 0x0C: W(b, regs[a] & (w & 0xFFFF) if regs[a] is not None else None); return    # andi
    elif op == 0x0D: W(b, regs[a] | (w & 0xFFFF) if regs[a] is not None else None); return    # ori
    elif op == 0x0E: W(b, regs[a] ^ (w & 0xFFFF) if regs[a] is not None else None); return    # xori
    elif op == 0x0F: W(b, (w & 0xFFFF) << 16); return                                         # lui
    elif op in _SV_RT_UNKNOWN_OPS: W(b, None); return   # loads: runtime-dependent
    elif op in (0x11, 0x12):   # cop1/cop2 move group: mfc/cfc write rt, mtc/ctc none
        W(b, None); return     # (marking rt Unknown for mtc too is sound)
    elif op in (0x1C, 0x1F):   # Allegrex special2/special3 (max/min, ext/ins/seb/seh/wsbw)
        W(d, None); W(b, None); return
    elif op in _SV_NOGPR_OPS:
        return
    else:                      # anything unrecognized: nuke the lattice (sound)
        for i in range(1, 32):
            regs[i] = None

def sv_plan(elf, insns, labels):
    """Pre-compute {flush_addr: [(reg, expected_value), ...]} for one function."""
    if not (insns and SV_ENABLED) or (insns & _SV_SPECIAL):
        return {}
    points, total = {}, 0
    regs: list[int | None] = [None] * 32
    regs[0] = 0
    written, run_len, last_plain = set(), 0, None

    def reset():
        nonlocal written, run_len, last_plain
        for i in range(1, 32):
            regs[i] = None
        written, run_len, last_plain = set(), 0, None

    def flush():
        nonlocal total
        if last_plain is None or run_len < 2 or total >= SV_MAX_PER_FUNC:
            return
        chk = [(r, regs[r]) for r in sorted(written) if regs[r] is not None][:SV_MAX_PER_FLUSH]
        if chk:
            points[last_plain] = chk
            total += len(chk)

    prev, in_delay = None, False
    for addr in sorted(insns):
        if prev is not None and addr != prev + 4:
            flush(); reset(); in_delay = False
        prev = addr
        if addr in labels:
            flush(); reset(); in_delay = False
        if in_delay:
            # Delay slot: executes on both branch paths; the post-branch state is
            # a join across successors, so restart the lattice after it.
            in_delay = False
            reset()
            continue
        w = read32(elf, addr)
        if w is None:
            flush(); reset(); continue
        if is_control(w):
            flush(); reset()
            in_delay = True
            continue
        _sv_step(w, regs, written)
        run_len += 1
        last_plain = addr
    flush()
    if points:
        SV_STATS["funcs"] += 1
        SV_STATS["checks"] += total
    return points

SR_SV_CHECK = """/* --static-verify support: compile-time-predicted register assertions. */
static void sr_sv_check(CpuState *s, uint32_t pc, int reg, uint32_t expect) {
    if (s->r[reg] != expect) {
        static int s_sv_reports = 0;
        if (s_sv_reports < 50) {
            s_sv_reports++;
            fprintf(stderr, "SV_MISMATCH pc=0x%08x r%d=0x%08x expected=0x%08x\\n",
                    pc, reg, s->r[reg], expect);
            fflush(stderr);
        }
    }
}"""

def emit_function(elf, start, ranges, known, resume_owners=None, resumable=False):
    resume_owners = resume_owners or {}
    host_entries = set(known) | set(resume_owners)
    insns, labels, continuations = function_flow(
        elf, start, ranges, known, resume_owners=resume_owners)
    sv_points = sv_plan(elf, insns, labels)
    # A delay slot that is itself a branch target is emitted twice: inline at its
    # owning control instruction (where it must execute as the slot) and again
    # under its own label (so branches can land on it).  On fall-through past the
    # owning instruction, control must NOT run the labelled duplicate: that would
    # execute the slot twice for non-likely branches and jal/jalr, and would run
    # an annulled slot for not-taken likely branches.  Plan a skip target at
    # slot+4 for every such site and jump over the duplicate explicitly.
    # (Observed in this image at f_00022d7c's tail byte-fill: the loop count in
    # the shared slot was decremented twice per entry, underfilling by one byte
    # or, for a tail count of exactly 1, wrapping to ~4GiB of writes.)
    dup_slot_skips = {}
    for _a in insns:
        _w = read32(elf, _a)
        if _w is None or not is_control(_w):
            continue
        _op, _fn = _w >> 26, _w & 0x3F
        if _op == 2 or (_op == 0 and _fn == 0x08):
            continue  # j / jr never fall through past their slot
        _ds = _a + 4
        if _ds in labels and _ds + 4 in insns:
            dup_slot_skips[_a] = _ds + 4
    if dup_slot_skips:
        labels = set(labels) | set(dup_slot_skips.values())
    out = []
    out.append(f"void {entry_symbol(start, resume_owners)}(CpuState *s) {{")
    if start in {0x0003D828, 0x0003DFD0, 0x000705B0, 0x001026B8, 0x001039D8}:
        out.append(f"    sr_boot_probe(s, 0x{start:08x}u);")
    out.append(f"    SR_YIELD(s, 0x{start:08x}u);")   # preemption point (no-op unless scheduler active)
    # Callable entries own an o32 frame contract. Resume entries begin with an
    # already-live owner frame, so the guest instructions alone own SP changes.
    if not resumable:
        out.append("    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;")
    if insns and start != min(insns):
        labels = set(labels)
        labels.add(start)
        out.append(f"    goto L_{start:08x};")
    consumed = set()
    for addr in sorted(insns):
        if addr in consumed:
            continue
        if addr in labels:
            out.append(f"  L_{addr:08x}: ;")
        w = read32(elf, addr)
        if w is None:
            continue
        op, fn = w >> 26, w & 0x3F

        # LOOP_CAPS retired (F4, 2026-07-11): the table is empty and the emission block
        # is gone. Every remaining capped loop was statically proven terminating and
        # verified never-firing across the retained log corpus (docs/audit/F4_LOOPCAPS.md):
        #   SCAN_CAP    0x000147e0  f_00014788: word-at-a-time strlen (0xfefefeff/0x80808080
        #               stopword scan). Induction variable a0 increases by 4 every
        #               iteration; MEM_R32 is bounds-checked, so the loop either finds a
        #               NUL or traps -- no same-PC spin is possible. The 1024-iteration
        #               cap silently corrupted strlen for any string >= 4 KiB.
        #   RSRC_CAP    0x0001b688  f_0001b584: counted loop (s3++ vs callee-saved s0).
        #               The guarded retry-starvation spin was an allocator-corruption
        #               symptom. The structural fix is one retail newlib allocator
        #               backed by its untouched UserSbrk partition block.
        #   HASHFN_CAP  0x0001b73c  f_0001b73c: pure NUL-terminated string hash
        #               (monotonic pointer walk) + divu by table size; terminates for
        #               any terminated string. The cap counted *calls*, not iterations,
        #               and forged r2=1 (phantom hash) on fire. Never fired.
        #   RSRC2-6     0x0019357c/0x001935d0/0x00193620/0x00193678/0x001936cc
        #               f_001934c8: five identical counted loops (s2++ vs u32 count
        #               reloaded from a descriptor the body never writes). Bounded
        #               unless callee-save discipline is broken -- which is exactly the
        #               P0 recompiler chain a cap must not mask. Never fired.
        #   SENT_CAP    0x001b9570  f_001b94f8: doubly-linked-list unlink-until-sentinel.
        #               Terminates for any well-formed list; the known list-corruption
        #               sources (WALKER_CAP truncation, LoadLayout runaway) are gone.
        #               Never fired.
        #   SCENELOOP   0x0006dc2c  f_0006dab0: begin/end iterator, r17 += 0x70 until
        #               r17 == r16 (compiler-emitted `it != end` idiom, always aligned).
        #               Never fired.
        #   RESINIT_CAP 0x0005a608  f_0005a500: element-init loop, a2++ vs count in a
        #               freshly-allocated, correctly-sized disjoint buffer. Bounded
        #               unless the allocator aliases the descriptor (prevented by the
        #               unified retail newlib/UserSbrk path). Never fired.
        #   TABLEWALK/LOOKUP/STRLIST sentinels (0x00102908/0x00102734/0x0019668c): the
        #               "dedicated blocks" the sentinels pointed at were never
        #               implemented; the entries were dead configuration.
        # Cooperative scheduling does not depend on any of this: codegen emits SR_YIELD
        # on every backward branch and function entry, and the SR_YIELD macro yields on
        # timeslice expiry, so even a genuinely stuck loop cannot starve other fibers --
        # it is watchdog-diagnosable by its unique back-edge PC in the thread dump.
        # If a spin reappears, root-cause it; do not re-cap.
        #
        # WALKER_CAP (0x0000095c, f_000008d8) REMOVED in F3: it was an absolute
        # 2048-hit permanent sentinel on a bounded "apply callback to N elements"
        # iterator, not a spin detector. A single legitimate table-init call
        # (RSRC init, hst_recomp_0.c:147346-147360) needs r8=0xc005=49157 iterations
        # in one call, so the cap tripped mid-init and permanently no-op'd the
        # iterator, leaving dispatch tables half-populated. The historical allocator
        # corruption it guarded is fixed at the retail newlib/UserSbrk boundary. See
        # docs/audit/F3_LOOPCAPS.md.
        # ARRAY_CAP (0x00100e98) and LISTWALK_CAP (0x0001038c) REMOVED in F3 (second
        # round, 2026-07-09): both still fired on every boot after WALKER_CAP's removal.
        # ARRAY_CAP force-broke a loop inside the 0x100xxx menu-layout/registration code
        # region after an absolute 20000 same-PC iterations — the same "bounded init
        # mistaken for a spin" failure mode as WALKER_CAP, and a direct candidate for
        # why dispatch tables (0x3070c0 +0xc/+0x10) remain half-populated. LISTWALK_CAP
        # broke f_0001034c's next-pointer chain walk on the theory the list could be
        # circular/corrupt; the known corruption sources upstream (WALKER_CAP truncation,
        # the LoadLayout runaway) are gone, so the guard's premise is stale. If either
        # loop genuinely spins again, the watchdog thread-dump PC identifies it uniquely
        # (0x00100e98 vs 0x0001038c) — root-cause that spin; do not re-cap (see
        # docs/audit/F3_LOOPCAPS.md and docs/DESIGN_FONT_HLE.md F3 re-scope).
        # The strtol parser can expose an unterminated asset token only after thousands
        # of characters. Keep this read-only probe compiled in; sr_boot_probe is a
        # production no-op unless SR_BOOT_DIAG is explicitly enabled.
        if addr == 0x000160e8:
            out.append("    sr_boot_probe(s, 0x000160e8u);")
        # Job-queue drain instrumentation. The boot watchdog often interrupts
        # inside the queue's sync callbacks, obscuring the outer queue state.
        # This read-only probe records count/index progress at the actual loop.
        if addr == 0x000705e4:
            out.append("    sr_boot_probe(s, 0x000705e4u);")
        # TOKENSCAN_DIAG: instrumentation ONLY (not a fix) for the post-1.6 blocker. The
        # worker's PC sampler pins on f_001041f4's character-scan loop (top at L_0010433c):
        # it dispatches f_0006517c (a vtable "get char / element" call) once per iteration
        # until a 0x23 ('#') byte, so the trip bound depends on runtime container contents
        # that cannot be resolved by static reading. Dump the container pointer (r21+0x1c)
        # plus the two index/count field-pairs (r21+0x2c/0x30 and r21+0x38/0x3c) and the
        # running per-scan byte counter (sp+0xc8) at the loop top. A single run then
        # distinguishes "many short parses" (struct changes between prints, counts sane =
        # bounded-slow, same class as the memset/arrshift fixes) from "one stuck/huge scan"
        # (struct unchanging while the counter climbs without ever hitting the count bound =
        # runaway / correctness bug upstream). Prints the first 24 hits, then every ~1M-th,
        # so it can neither miss the opening nor flood a long run. REMOVE once characterised.
        if EMIT_DIAG_PROBES and addr == 0x0010433c:
            out.append(
                "    { static unsigned long long _ts_it = 0; static int _ts_pr = 0;\n"
                "      unsigned long long _ts_n = ++_ts_it;\n"
                "      if (_ts_pr < 24 || (_ts_n & 0xfffffull) == 0) { if (_ts_pr < 1000000) _ts_pr++;\n"
                "        fprintf(stderr, \"TOKENSCAN_DIAG it=%llu struct=0x%08x buf=0x%08x idxA=0x%08x cntA=0x%08x idxB=0x%08x cntB=0x%08x bcnt=0x%08x\\n\",\n"
                "          _ts_n, s->r[21], MEM_R32(s->r[21] + 0x1cu), MEM_R32(s->r[21] + 0x2cu), MEM_R32(s->r[21] + 0x30u), MEM_R32(s->r[21] + 0x38u), MEM_R32(s->r[21] + 0x3cu), MEM_R32(s->r[29] + 0xc8u));\n"
                "        fflush(stderr); } }"
            )



        # F3G ENTRY PROBE (temporary): dump f_0006e9bc's incoming object (r4) and count (r5)
        # to resolve the static-vs-runtime disconnect — statically r5 should be 96 and r4 a
        # valid heap object, but at runtime the memset reads garbage. Prints once. Remove after.
        if EMIT_DIAG_PROBES and addr == 0x0006e9bc:
            out.append(
                "    { static int _e9_n = 0; if (_e9_n++ < 4) fprintf(stderr, "
                "\"F3G_ENTRY f_0006e9bc: r4(obj)=0x%08x r5(count)=0x%08x ra=0x%08x sp=0x%08x\\n\", "
                "s->r[4], s->r[5], s->r[31], s->r[29]); }"
            )

        # MEMSET_FASTPATH: f_0006e9bc's post-alloc zero-fill loop (body 0x6ea28-0x6ea44,
        # count/base reloaded at 0x6ea1c/0x6ea28). Confirmed by static trace of the lifted
        # instructions: base (MEM[r16+8]) and count (MEM[r16+0]) are both read-only for the
        # entire loop body -- nothing between 0x6ea1c and 0x6ea44 writes r16+0 or r16+8 --
        # so `for (i=0;i<count;i++) base[i]=0` is a provably exact rewrite of the interpreted
        # per-byte loop as one native pass. This function is called ~100x from the resource-
        # table init loop at 0x6da44; under one-MIPS-
        # instruction-per-dispatch interpretation the accumulated SR_YIELD/dispatch overhead
        # per byte stalls the worker thread past the 1200-vblank watchdog before a second
        # frame ever presents. Dead-code checked: r3/r4 are never read again after the loop
        # (L_0006ea48's epilogue only restores r31/r17/r16 from the stack) and r2 (the
        # function's implicit return value, set by the alloc call at 0x6ea18) is untouched
        # by the loop, so skipping the interpreted instructions is transparent to the caller.
        # MEM_W8/MEM_R32 are already bounds-checked (src/rt/recomp.h sr_inrange).  The
        # fast path now rejects a non-contiguous/overflowing span before any host pointer
        # is formed; the >1MB warning exists to flag malformed counts without attempting
        # an unbounded scalar fallback.
        if addr == 0x0006ea1c:
            out.append(f"    sr_begin(s, 0x0006ea1cu, 0x{w:08x}u); s->r[3] = MEM_R32(s->r[16] + 0x00000000u); sr_end(s, 0u, 0);")
            out.append(
                "    { uint32_t _mf_cnt = s->r[3]; uint32_t _mf_base = MEM_R32(s->r[16] + 0x00000008u);\n"
                "      if (_mf_cnt > 0x00100000u) fprintf(stderr, \"MEMSET_FASTPATH: large count=0x%x base=0x%x r16=0x%08x MEM[r16+0]=0x%08x MEM[r16+4]=0x%08x MEM[r16+8]=0x%08x ra=0x%08x\\n\", _mf_cnt, _mf_base, s->r[16], (s->r[16]<0x0c000000u?MEM_R32(s->r[16]):0xDEADu), (s->r[16]<0x0c000000u?MEM_R32(s->r[16]+4u):0xDEADu), (s->r[16]<0x0c000000u?MEM_R32(s->r[16]+8u):0xDEADu), s->r[31]);\n"
                "      if (_mf_cnt > 0 && _mf_cnt < 0x04000000u && sr_guest_span_writable(_mf_base, _mf_cnt)) {\n"
                "          if (g_sr_heap_watch) sr_heap_note_bulk_write(_mf_base, _mf_cnt, 0x0006ea1cu);\n"
                "          memset(SR_HOST(_mf_base), 0, _mf_cnt);\n"
                "      } else {\n"
                "          if (_mf_cnt) sr_oor(_mf_base, 0u, 1);\n"
                "      }\n"
                "      s->r[4] = _mf_cnt; s->r[3] = 0u; }"
            )
            for _skip in (0x0006ea20, 0x0006ea24, 0x0006ea28, 0x0006ea2c, 0x0006ea30,
                          0x0006ea34, 0x0006ea38, 0x0006ea3c, 0x0006ea40, 0x0006ea44):
                consumed.add(_skip)
            continue

        # ARRSHIFT_FASTPATH: f_001084b8's element-shift loop (setup 0x108630-0x108654,
        # body 0x108658-0x108690). Confirmed by static trace of the lifted instructions:
        # this is a reverse, element-by-element 8-byte block move -- the source pointer
        # (r6) and dest pointer (r4) both start high and decrement by exactly 8 every
        # iteration in lockstep, terminating when r6 reaches r5 (the fixed source base
        # loaded once at 0x108634 and never written again in the loop). The trip count is
        # exactly n = MEM[r19+4] (read once, at 0x108630, before anything in the loop body
        # can touch r19+4). This is the classic "grow a packed array, shift n existing
        # elements into the new buffer" pattern: the code immediately after the loop
        # (0x108694-0x1086c8) swaps the struct's base/count/capacity fields at r19+0/+4/+8
        # with the new ones staged on the stack and calls f_00108bf4 (frees the old
        # buffer) -- realloc-and-shift. A reverse per-element copy over
        # [src,src+n*8) <-> [dst-n*8,dst) is exactly what memmove() computes (memmove is
        # correct for either overlap direction, a superset of what the hand-unrolled
        # reverse loop covers for its one intended direction). Under one-MIPS-instruction-
        # per-dispatch interpretation this loop was a historical bring-up blocker: the
        # worker cycles between pc=0x0006517c (an unrelated inlined "r2=r5<<1" leaf called
        # from elsewhere, just this loop's most recent PCSAMPLE neighbour) and this loop's
        # own SR_YIELD checkpoint at pc=0x0010868c for the entire watchdog window without
        # the loop ever finishing. Historical details are retained in the local archive.
        # Two stack slots track the same trip progress in different units and both must
        # land on the same final value as the interpreted loop would leave them: MEM[sp+
        # 0x5c] (decrements by 1/iter) and MEM[sp+0x50] (increments by 1/iter, consumed by
        # the caller as the new element count at 0x1086c0-0x1086c4). Registers r1-r6's
        # loop-final values are dead: everything from 0x108694 onward reloads r2/r3 fresh
        # from memory and overwrites r4/r5/r6 before any further read (confirmed by
        # reading through to the f_00108bf4 call at 0x1086c8), so only the copied bytes
        # and the two stack slots need to be reproduced. The r4==0 mid-loop edge case
        # (dest pointer hits exactly address 0) is not special-cased in the fast path --
        # it would require the dest range to span across address 0, which cannot happen
        # for a legitimate guest-RAM pointer -- but the bounds-checked fallback below
        # still reproduces it exactly if it ever did.
        if addr == 0x00108630:
            out.append(f"    sr_begin(s, 0x00108630u, 0x{w:08x}u); s->r[4] = MEM_R32(s->r[19] + 0x00000004u); sr_end(s, 0u, 0);")
            out.append(
                "    { uint32_t _as_n = s->r[4];\n"
                "      uint32_t _as_src_base = MEM_R32(s->r[19] + 0x00000000u);\n"
                "      uint32_t _as_sp5c0 = MEM_R32(s->r[29] + 0x0000005cu);\n"
                "      uint32_t _as_sp500 = MEM_R32(s->r[29] + 0x00000050u);\n"
                "      uint32_t _as_dst_base = MEM_R32(s->r[29] + 0x0000004cu);\n"
                "      uint32_t _as_dst_hi = _as_dst_base + (_as_sp5c0 << 3);\n"
                "      if (_as_n > 0x00100000u) fprintf(stderr, \"ARRSHIFT_FASTPATH: large n=0x%x src=0x%x dst_hi=0x%x at f_001084b8 (0x108630)\\n\", _as_n, _as_src_base, _as_dst_hi);\n"
                "      if (_as_n > 0u) {\n"
                "          uint32_t _as_dst_lo = _as_dst_hi - (_as_n << 3);\n"
                "          uint64_t _as_nbytes = (uint64_t)_as_n << 3;\n"
                "          if (_as_dst_lo < _as_dst_hi && _as_src_base < 0x0c000000u && _as_dst_lo < 0x0c000000u &&\n"
                "              (uint64_t)_as_src_base + _as_nbytes <= 0x0c000000u && (uint64_t)_as_dst_lo + _as_nbytes <= 0x0c000000u) {\n"
                "              if (g_sr_heap_watch) sr_heap_note_bulk_write(_as_dst_lo, (uint32_t)_as_nbytes, 0x00108630u);\n"
                "              memmove(SR_HOST(_as_dst_lo), SR_HOST(_as_src_base), (size_t)_as_nbytes);\n"
                "          } else {\n"
                "              for (uint32_t _as_i = 0; _as_i < _as_n; _as_i++) {\n"
                "                  uint32_t _as_d = _as_dst_hi - 8u * (_as_i + 1u);\n"
                "                  uint32_t _as_s = _as_src_base + (_as_n - 1u - _as_i) * 8u;\n"
                "                  if (_as_d != 0u) {\n"
                "                      MEM_W32(_as_d + 0u, MEM_R32(_as_s + 0u));\n"
                "                      MEM_W32(_as_d + 4u, MEM_R32(_as_s + 4u));\n"
                "                  }\n"
                "              }\n"
                "          }\n"
                "      }\n"
                "      MEM_W32(s->r[29] + 0x0000005cu, _as_sp5c0 - _as_n);\n"
                "      MEM_W32(s->r[29] + 0x00000050u, _as_sp500 + _as_n);\n"
                "      s->r[1] = 0u; s->r[5] = _as_src_base; s->r[6] = _as_src_base;\n"
                "    }"
            )
            for _skip in (0x00108634, 0x00108638, 0x0010863c, 0x00108640, 0x00108644, 0x00108648,
                          0x0010864c, 0x00108650, 0x00108654, 0x00108658, 0x0010865c, 0x00108660,
                          0x00108664, 0x00108668, 0x0010866c, 0x00108670, 0x00108674, 0x00108678,
                          0x0010867c, 0x00108680, 0x00108684, 0x00108688, 0x0010868c, 0x00108690):
                consumed.add(_skip)
            continue

        # GUEST_ABORT: f_00000a1c is the game's own compiled abort() path -- its body
        # (jal 0x000015ec, a diagnostic/handler-table walk, immediately followed by an
        # unconditional self-branch at L_00000a24, with NO guard of any kind) is only
        # ever reached indirectly: a raw jal/j word scan of the whole image confirms
        # there is no static call site targeting this address, so it can only be
        # entered via a function pointer / dispatch() (i.e. it is the registered
        # abort/terminate handler). NOTE: this is address 0x00000a1c, not the adjacent
        # 0x00000a14 -- analyze()'s static function-boundary detection recognizes BOTH
        # as independent function starts (confirmed: both are in the `known` set with
        # zero static jal/j callers), but they are different functions. f_00000a14 is a
        # harmless stub whose very first instruction (`beq r0,r0`) unconditionally
        # branches around this exact jal+loop body to a plain epilogue/return --
        # verified to fire routinely and harmlessly during ordinary boot (regression-
        # tested: a probe placed at 0x00000a14 false-positived on a known-good run that
        # only reaches mode-select). f_00000a1c has no such guard: every entry
        # unconditionally calls the diagnostic walk and then spins forever. Left alone,
        # reaching it means the emulator silently spins forever on the guest's own
        # infinite loop, presenting nothing, until an external timeout kills the
        # process -- observed via both heap exhaustion and a missing character/stage
        # asset validation failure, minutes apart in wall-clock terms. Fail fast
        # instead, mirroring the existing unimplemented-HLE-NID convention in hle.c
        # (search "_Exit(7)" there) with a distinct, reserved exit code so the two
        # fatal classes are distinguishable from the process exit status alone.
        if addr == 0x00000a1c:
            out.append(
                "    fprintf(stderr, \"GUEST_ABORT: game called abort() (f_00000a1c), "
                "called from ra=0x%08x arg0=0x%08x -- terminating instead of silent infinite hang\\n\", "
                "s->r[31], s->r[4]); fflush(stderr); _Exit(9);"
            )

        if not is_control(w):
            out.append(normal_line(addr, w))
            if addr in sv_points:
                for _sv_r, _sv_v in sv_points[addr]:
                    out.append(f"    sr_sv_check(s, 0x{addr:08x}u, {_sv_r}, 0x{_sv_v:08x}u);")
            if addr in continuations:
                out.append(f"    goto _sr_cont_{continuations[addr]:08x};")
            continue

        ds = addr + 4
        dsw = read32(elf, ds)
        if ds not in labels:
            consumed.add(ds)
        ds_is_syscall = dsw is not None and (dsw >> 26) == 0 and (dsw & 0x3F) == 0x0C

        if op == 3:  # jal
            target = jump_target(addr, w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); s->r[31] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            if target in host_entries:
                out.append(f"    {entry_symbol(target, resume_owners)}(s);")
            else:
                out.append(f"    dispatch(s, 0x{target:08x}u);")
            if addr in continuations:
                out.append(f"    goto _sr_cont_{continuations[addr]:08x};")
            elif addr in dup_slot_skips:
                out.append(f"    goto L_{dup_slot_skips[addr]:08x}; /* slot already ran inline; skip its labelled duplicate */")
            continue
        if op == 0 and fn == 0x09:  # jalr rd, rs
            d, a = rd(w), rs(w)
            link = f"s->r[{d}] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; " if d != 0 else ""
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {link}sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            out.append(f"    {{ uint32_t _t = {R(a)}; dispatch(s, _t); }}")
            if addr in continuations:
                out.append(f"    goto _sr_cont_{continuations[addr]:08x};")
            elif addr in dup_slot_skips:
                out.append(f"    goto L_{dup_slot_skips[addr]:08x}; /* slot already ran inline; skip its labelled duplicate */")
            continue
        if op == 0 and fn == 0x08:  # jr rs
            a = rs(w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); sr_end(s, 0u, 0);")
            if ds_is_syscall and dsw is not None:
                # JR with syscall in delay slot. This is not reachable for current eboot stubs (handled by is_stub),
                # but if reached in general code, route it via the correct raw-syscall mechanism with PC.
                out.append(f"    sr_raw_syscall(s, 0x{(dsw >> 6) & 0xFFFFF:x}u, 0x{ds:08x}u); {emit_host_return(resumable)}")
            else:
                out.append(normal_line(ds, dsw))
                if a == 31:
                    out.append(f"    {emit_host_return(resumable)}")
                else:
                    out.append(f"    {{ uint32_t _t = {R(a)}; dispatch(s, _t); {emit_host_return(resumable)} }}")
            continue
        if op == 2:  # j
            target = jump_target(addr, w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            if target in host_entries and not (target in resume_owners and target in labels):
                out.append(f"    {entry_symbol(target, resume_owners)}(s); {emit_host_return(resumable)}")
            elif target in labels:
                y = f"SR_YIELD(s, 0x{addr:08x}u); " if target <= addr else ""   # backward j: loop edge
                out.append(f"    {y}goto L_{target:08x};")
            else:
                out.append(f"    {{ uint32_t _t = 0x{target:08x}u; dispatch(s, _t); {emit_host_return(resumable)} }}")
            continue
        # conditional branch
        target = branch_target(addr, w)
        y = f"SR_YIELD(s, 0x{addr:08x}u); " if target <= addr else ""
        link = f"s->r[31] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; " if is_link(w) else ""
        cond = cond_expr(w)
        inject_stmt = ""
        if addr in GUEST_PATCHES:
            p = GUEST_PATCHES[addr]
            if p["kind"] == "cond":
                cond = p["value"]
                if p.get("stmt"):
                    inject_stmt += p["stmt"]
            elif p["kind"] == "inject":
                inject_stmt += p["stmt"]

        out.append(f"    {{ uint32_t _c = {cond};{inject_stmt}")
        out.append(f"      sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {link}sr_end(s, 0u, 0);")
        if target in labels:
            if is_likely(w):
                out.append(f"      if (_c) {{ {normal_line(ds, dsw).strip()} {y}goto L_{target:08x}; }} }}")
            else:
                out.append(f"   {normal_line(ds, dsw)}")
                out.append(f"      if (_c) {{ {y}goto L_{target:08x}; }} }}")
        else:
            if is_likely(w):
                out.append(f"      if (_c) {{ {normal_line(ds, dsw).strip()} {{ s->pc = 0x{target:08x}u; dispatch(s, s->pc); {emit_host_return(resumable)} }} }} }}")
            else:
                out.append(f"   {normal_line(ds, dsw)}")
                out.append(f"      if (_c) {{ {{ s->pc = 0x{target:08x}u; dispatch(s, s->pc); {emit_host_return(resumable)} }} }} }}")
        if addr in dup_slot_skips:
            out.append(f"    goto L_{dup_slot_skips[addr]:08x}; /* slot already ran inline (or was annulled); skip its labelled duplicate */")
    if continuations:
        # Do not let an unrelated natural end path fall into the first continuation
        # block merely because the blocks live after the sorted instruction bodies.
        out.append("    goto _sr_fallthrough_return;")
        for target in sorted(set(continuations.values())):
            out.append(f"  _sr_cont_{target:08x}: ;")
            out.append(f"    {entry_symbol(target, resume_owners)}(s);")
            out.append(f"    {emit_host_return(resumable, 'synthetic boundary: restore the owning entry frame')}")
        out.append("  _sr_fallthrough_return: ;")
    out.append(f"    {emit_host_fallthrough(resumable)}")
    out.append("}")
    out.append("")
    return out

SR_TO_W = """static uint32_t sr_to_w(float x, uint32_t fn) {
    if (isnan(x) || isinf(x)) return (isinf(x) && x < 0.0f) ? 0x80000000u : 0x7FFFFFFFu;
    int32_t r;
    switch (fn) {
        case 0x0C: r = (int32_t)floorf(x + 0.5f); break;
        case 0x0D: if (x >= 0.0f) { r = (int32_t)floorf(x); if (r == (int32_t)0x80000000) r = 0x7FFFFFFF; } else r = (int32_t)ceilf(x); break;
        case 0x0E: r = (int32_t)ceilf(x); break;
        case 0x0F: r = (int32_t)floorf(x); break;
        default:   r = (int32_t)nearbyintf(x); break;
    }
    return (uint32_t)r;
}"""


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    if len(args) < 2:
        sys.stderr.write("usage: codegen.py <elf> <out.c> [--base=HEX] [--profile=NAME] [--funcs-per-chunk=N] [--extra-elf=ELF@BASE]...\n")
        return 2
    base = None
    profile = None
    funcs_per_chunk = 2000
    extra_elfs = []  # list of (elf_path, base_addr)
    for o in opts:
        if o.startswith("--base="):
            base = int(o.split("=", 1)[1], 16)
        elif o == "--static-verify":
            global SV_ENABLED
            SV_ENABLED = True
        elif o.startswith("--profile="):
            profile = o.split("=", 1)[1]
        elif o.startswith("--funcs-per-chunk="):
            try:
                funcs_per_chunk = int(o.split("=", 1)[1], 10)
            except ValueError:
                sys.stderr.write(f"invalid --funcs-per-chunk value: {o}\n")
                return 2
            if funcs_per_chunk < 1:
                sys.stderr.write("--funcs-per-chunk must be at least 1\n")
                return 2
        elif o.startswith("--extra-elf="):
            spec = o.split("=", 1)[1]
            if "@" in spec:
                elf_path, base_str = spec.split("@", 1)
                extra_elfs.append((elf_path, int(base_str, 16)))
            else:
                sys.stderr.write(f"invalid --extra-elf format: {o}\n")
                return 2
    elf = Elf(args[0], base=base)
    analyzed, ranges = analyze(elf)
    catalog = build_entry_catalog(analyzed, ranges, profile=profile, elf=elf)
    known = {addr for addr, info in catalog.items() if info.callable}
    resume_owners = {
        addr: info.owner for addr, info in catalog.items() if info.resumable
    }

    impmap = {}
    if elf.reloc is not None:
        try:
            from imports import parse_imports
            impmap = parse_imports(elf)
        except Exception as e:
            sys.stderr.write(f"warning: import table parse failed: {e}\n")

    stub = elf.sec(".sceStub.text")
    def is_stub(a):
        if a in impmap:
            return True
        return stub is not None and stub["addr"] <= a < stub["addr"] + stub["size"]

    emitted = []
    stubbed = []
    func_texts = []

    # Semantic name overrides for known HST functions. Single source of truth is
    # host_stubs.HST_SIMPLE_STUBS: addr -> (name, retflag). retflag=1 means the
    # stub returns a meaningful value (r2=1); retflag=0 means it returns success
    # (r2=0) and otherwise does nothing.
    SIMPLE_STUBS = HST_SIMPLE_STUBS if profile == "hst" else {}

    for a in sorted(catalog):
        # --- CUSTOM STUBS START ---
        if a == 0x000011b0:
            text = """void f_000011b0(CpuState *s) {  /* custom stub: __register_frame_info bypass */
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x0000260c:
            text = """void f_0000260c(CpuState *s) {  /* custom stub: exception helper bypass */
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x0000fe3c:
            text = """void f_0000fe3c(CpuState *s) {  /* custom stub: _getmodreent / FileIO_GetState */
    if (s->r[31] == 0x000104c4u || s->r[31] == 0x000104f4u) {
        /* Allocator calls always use the main thread's global impure reent */
        s->r[2] = 0x002cf338u;
    } else {
        extern uint32_t sr_thread_k0(void);
        uint32_t k0 = s->r[26];
        if (k0 < 0x08000000u || k0 >= 0x10000000u) k0 = sr_thread_k0();
        uint32_t ptr = k0 ? MEM_R32(k0 + 4) : 0;
        if (!ptr) ptr = MEM_R32(0x002cf6b8u);
        if (!ptr) ptr = 0x002cf338u;
        s->r[2] = ptr;
    }
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x00010738:
            text = """void f_00010738(CpuState *s) {  /* custom stub: _malloc_r(reent, size) -> sr_newlib_malloc bridge
     * This runtime owns the arena metadata. Retail _memalign_r/_realloc_r edit dlmalloc
     * headers directly, where bit 0 means PREV_INUSE; the host header uses bit 0 for the
     * current block's allocation state. Keep the whole metadata-manipulating API on one ABI.
     * malloc()'s retail wrapper saves its own caller RA at sp+4 before calling _malloc_r.
     * operator new then adds a 0x20-byte frame and saves its caller at +0xc, which is
     * sp+0x1c from here. Unwrap only those exact verified return PCs; direct _malloc_r
     * callers retain their immediate RA, so tracing gains provenance without changing
     * guest state. */
    uint32_t owner_ra = s->r[31] == 0x000104d0u
        ? MEM_R32(s->r[29] + 0x00000004u) : s->r[31];
    if (owner_ra == 0x00000bf4u || owner_ra == 0x00000c5cu)
        owner_ra = MEM_R32(s->r[29] + 0x0000001cu);
    s->r[2] = sr_newlib_malloc(s->r[5], owner_ra);
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x0000f538:
            text = """void f_0000f538(CpuState *s) {  /* custom stub: _free_r(reent, ptr) -> sr_newlib_free bridge
     * Reinstated 2026-07-16 alongside f_00010738 -- see that stub's comment and ISSUES.md P0. */
    uint32_t owner_ra = s->r[31] == 0x00010500u
        ? MEM_R32(s->r[29] + 0x00000004u) : s->r[31];
    if (owner_ra == 0x00000a14u)
        owner_ra = MEM_R32(s->r[29] + 0x0000001cu);
    sr_newlib_free(s->r[5], owner_ra);
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x000101c4:
            text = """void f_000101c4(CpuState *s) {  /* custom stub: _memalign_r(reent, alignment, size)
     * The translated retail body carves dlmalloc chunks and writes PREV_INUSE into the
     * following header. That bit is incompatible with the host arena's current-block flag. */
    s->r[2] = sr_newlib_memalign(s->r[5], s->r[6], s->r[31]);
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x00013524:
            text = """void f_00013524(CpuState *s) {  /* custom stub: _realloc_r(reent, ptr, size)
     * Retail realloc also walks, unlinks, and rewrites dlmalloc headers, so it must use
     * the same host-owned metadata ABI as malloc/free/memalign. */
    s->r[2] = sr_newlib_realloc(s->r[5], s->r[6], s->r[31]);
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x00015ea0:
            text = """void f_00015ea0(CpuState *s) {
    /* Host-side tokenizer matching FUN_00015ea0 (EBOOT.BIN.dec.c:22965).
     * str is the CSV buffer pointer; delim is the delimiter string. First
     * call (str != 0) builds a cached token table by walking the buffer
     * once. Subsequent calls return one token pointer per yield. */
    uint32_t str = s->r[4];
    uint32_t delim = s->r[5];
    static uint32_t s_tokens[16384];
    static uint32_t s_ntokens = 0;
    static uint32_t s_tok_idx = 0;
    if (str != 0) {
        s_ntokens = 0;
        s_tok_idx = 0;
        uint32_t p = str;
        uint32_t d = delim;
        while (MEM_R8(p) != 0 && s_ntokens < 16384) {
            while (MEM_R8(p) != 0) {
                int is_delim = 0;
                for (uint32_t dd = d; MEM_R8(dd) != 0; dd++) {
                    if (MEM_R8(p) == MEM_R8(dd)) { is_delim = 1; break; }
                }
                if (!is_delim) break;
                p++;
            }
            if (MEM_R8(p) == 0) break;
            s_tokens[s_ntokens++] = p;
            while (MEM_R8(p) != 0) {
                int is_delim = 0;
                for (uint32_t dd = d; MEM_R8(dd) != 0; dd++) {
                    if (MEM_R8(p) == MEM_R8(dd)) { is_delim = 1; break; }
                }
                if (is_delim) { MEM_W8(p, 0); p++; break; }
                p++;
            }
        }
    }
    s->r[2] = (s_tok_idx < s_ntokens) ? s_tokens[s_tok_idx++] : 0u;
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x000143b0:
            text = r'''void f_000143b0(CpuState *s) {  /* custom stub: guest sprintf */
    sr_guest_sprintf(s);
}'''
            func_texts.append(text); emitted.append(a); continue

        if a == 0x00046d14:
            text = """void f_00046d14(CpuState *s) {  /* game loop entry trace */
    fprintf(stderr, "GAMELOOP: entered L_00046d14 pc=0x%08x\\n", s->pc);
    fflush(stderr);
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x0001034c:
            text = """void f_0001034c(CpuState *s) {  /* custom stub: skip corrupted heap-statistics walk */
    /* This routine only accumulates mallinfo-style counters. The guest free-list can be
     * incomplete during bring-up; walking it must not block game initialization. */
    s->r[2] = 0u;
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue



        if a in SIMPLE_STUBS:
            name, ret_val = SIMPLE_STUBS[a]
            text = f"""void f_{a:08x}(CpuState *s) {{  /* custom stub: {name} - return static val */
    s->r[2] = {ret_val};
    s->pc = s->r[31];
}}"""
            func_texts.append(text); emitted.append(a); continue

        # 0x0001a5f8 and 0x0001c008 had constant-return custom stubs here until
        # 2026-07-18. Ghidra-assisted review (ISSUES.md) showed both shadow real,
        # fully-translatable code: 0x1c008 is `jr ra; sw a1,0x4028(a0)` (the stub
        # dropped the delay-slot store) and 0x1a5f8 is a computed-goto resume
        # point reached via .data pointer tables. Both are discovered entries and
        # translate faithfully; tools/test_codegen_no_shadow_stubs.py keeps them so.

        if a == 0x000468c8:
            func_texts.append("\n".join(emit_function(
                elf, a, ranges, known, resume_owners=resume_owners,
                resumable=catalog[a].resumable)))
            func_texts[-1] = func_texts[-1].replace("void f_000468c8(CpuState *s)", "void f_000468c8_real(CpuState *s)")
            text = """void f_000468c8(CpuState *s) {  /* custom stub: main_RunGameLoop - infinite frame loop */
    for (;;) {
        SR_YIELD(s, 0x000468c8u);
        f_000468c8_real(s);
    }
}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x001d9eb0:
            # This retail chooser supplies the NN in both menu/095_titleNN.xb and
            # data/menu/title/title_cNN.gim.  Every shipped title archive is numbered
            # from 01 upward; 00 does not exist.  Preserve the original selection
            # routine, but recover from its impossible zero result.  Without this
            # guard the failed c00 load leaves an unrelated text surface in the title
            # background buffer.  Archived live traces selected c01/c02 normally, so
            # this is intentionally a postcondition guard rather than a replacement
            # chooser or a VFS alias that would conceal bad resource paths globally.
            func_texts.append("\n".join(emit_function(
                elf, a, ranges, known, resume_owners=resume_owners,
                resumable=catalog[a].resumable)))
            func_texts[-1] = func_texts[-1].replace(
                "void f_001d9eb0(CpuState *s)", "void f_001d9eb0_real(CpuState *s)")
            text = """void f_001d9eb0(CpuState *s) {  /* title backdrop selector postcondition */
    f_001d9eb0_real(s);
    if (s->r[2] == 0u) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "TITLE_SELECT: retail chooser returned invalid id 0; using shipped backdrop 01\\n");
            fflush(stderr);
            warned = 1;
        }
        s->r[2] = 1u;
    }
}"""
            func_texts.append(text); emitted.append(a); continue

        if a in (0x00011090, 0x000110dc):
            text = f"""void f_{a:08x}(CpuState *s) {{  /* custom stub: memcpy native */
    uint32_t dest = s->r[4], src = s->r[5], size = s->r[6];
    if (size > 0 && size < 0x04000000u &&
        sr_guest_span_writable(dest, size) && sr_guest_span_readable(src, size)) {{
            if (g_sr_heap_watch) sr_heap_note_bulk_write(dest, size, 0x{a:08x}u);
            memmove(SR_HOST(dest), SR_HOST(src), size);
    }} else if (size) {{
        sr_oor(dest, 0u, 1); sr_oor(src, 0u, 0);
    }}
    s->r[2] = dest;
    s->pc = s->r[31];
}}"""
            func_texts.append(text); emitted.append(a); continue

        if a in (0x000114c0, 0x000114a8):
            text = f"""void f_{a:08x}(CpuState *s) {{  /* custom stub: sceKernelMemset native */
    uint32_t dest = s->r[4], size = s->r[6];
    uint8_t val = (uint8_t)(s->r[5] & 0xFF);
    if (size > 0 && size < 0x04000000u && sr_guest_span_writable(dest, size)) {{
            if (g_sr_heap_watch) sr_heap_note_bulk_write(dest, size, 0x{a:08x}u);
            memset(SR_HOST(dest), val, size);
    }} else if (size) {{
        sr_oor(dest, val, 1);
    }}
    s->r[2] = dest;
    s->pc = s->r[31];
}}"""
            func_texts.append(text); emitted.append(a); continue

        if a == 0x000149a8:
            text = """void f_000149a8(CpuState *s) {  /* custom stub: strcpy native */
    /* Boot's file-open wrapper copies its source path here before adding the
     * host0: prefix.  Treating this routine as strcmp left the destination
     * uninitialized and corrupted every resource path during startup. */
    uint32_t dst0 = s->r[4], dst = dst0, src = s->r[5];
    if (dst != 0u && src != 0u) {
        for (uint32_t n = 0; n < 0x00100000u; n++) {
            uint8_t ch = MEM_R8(src++);
            MEM_W8(dst++, ch);
            if (ch == 0u) break;
        }
    }
    s->r[2] = dst0;
    s->pc = s->r[31];
}"""
            func_texts.append(text); emitted.append(a); continue

        # --- CUSTOM STUBS END ---

        if is_stub(a):
            lib_nid = impmap.get(a)
            if lib_nid is not None:
                lib, nid = lib_nid
                text = f"void f_{a:08x}(CpuState *s) {{  /* import: {lib} nid 0x{nid:08x} */\n"
                text += f"    sr_syscall(s, 0x{nid:08x}u);\n"
                text += f"    sr_end(s, 0u, 0);\n}}"
            else:
                text = f"void f_{a:08x}(CpuState *s) {{  /* import stub without NID mapping */\n"
                text += f"    sr_unimplemented(0x{a:08x}u, \"import stub without NID mapping\");\n}}"
            func_texts.append(text)
            emitted.append(a)
            continue
        try:
            func_texts.append("\n".join(emit_function(
                elf, a, ranges, known, resume_owners=resume_owners,
                resumable=catalog[a].resumable)))
            emitted.append(a)
        except Unsupported as e:
            reason = str(e).replace('"', "'")
            text = f"void {entry_symbol(a, resume_owners)}(CpuState *s) {{  /* untranslatable: {reason} */\n"
            text += f'    sr_unimplemented(0x{a:08x}u, "{reason}");\n}}'
            func_texts.append(text)
            emitted.append(a)
            stubbed.append((a, reason))
            sys.stderr.write(f"skip 0x{a:08x}: {e}\n")

    # Process extra ELF files (PRX modules)
    for extra_elf_path, extra_base in extra_elfs:
        sys.stderr.write(f"Processing extra ELF: {extra_elf_path} @ 0x{extra_base:08x}\n")
        extra_elf = Elf(extra_elf_path, base=extra_base)
        extra_ranges = exec_ranges(extra_elf)
        extra_known, _ = analyze(extra_elf)
        extra_known = set(a for a in extra_known if in_ranges(a, extra_ranges))

        extra_impmap = {}
        if extra_elf.reloc is not None:
            try:
                from imports import parse_imports
                extra_impmap = parse_imports(extra_elf)
            except Exception as e:
                sys.stderr.write(f"warning: import table parse failed for {extra_elf_path}: {e}\n")

        extra_stub = extra_elf.sec(".sceStub.text")
        def is_extra_stub(a):
            if a in extra_impmap:
                return True
            return extra_stub is not None and extra_stub["addr"] <= a < extra_stub["addr"] + extra_stub["size"]

        for a in sorted(extra_known):
            if is_extra_stub(a):
                lib_nid = extra_impmap.get(a)
                if lib_nid is not None:
                    lib, nid = lib_nid
                    text = f"void f_{a:08x}(CpuState *s) {{  /* import: {lib} nid 0x{nid:08x} */\n"
                    text += f"    sr_syscall(s, 0x{nid:08x}u);\n"
                    text += f"    sr_end(s, 0u, 0);\n}}"
                else:
                    text = f"void f_{a:08x}(CpuState *s) {{  /* import stub -> HLE boundary */\n"
                    text += f"    sr_hle_call(s, 0u);\n}}"
                func_texts.append(text)
                emitted.append(a)
                continue
            try:
                func_texts.append("\n".join(emit_function(extra_elf, a, extra_ranges, extra_known)))
                emitted.append(a)
            except Unsupported as e:
                reason = str(e).replace('"', "'")
                text = f"void f_{a:08x}(CpuState *s) {{  /* untranslatable: {reason} */\n"
                text += f'    sr_unimplemented(0x{a:08x}u, "{reason}");\n}}'
                func_texts.append(text)
                emitted.append(a)
                stubbed.append((a, reason))
                sys.stderr.write(f"skip 0x{a:08x}: {e}\n")

    num_files = (len(func_texts) + funcs_per_chunk - 1) // funcs_per_chunk
    base_name = os.path.splitext(args[1])[0]

    # Write the codegen gap report
    with open(f"{base_name}_stubs.txt", "w", encoding="ascii", newline="\n") as f:
        for a, r in sorted(stubbed):
            f.write(f"0x{a:08x} {r}\n")

    # Write the shared functions header
    funcs_h_path = f"{base_name}_funcs.h"
    with open(funcs_h_path, "w", encoding="ascii", newline="\n") as f:
        f.write("#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n")
        f.write('#include "recomp.h"\n\n')
        for a in emitted:
            f.write(f"void {entry_symbol(a, resume_owners)}(CpuState *s);\n")
        f.write("#endif\n")

    # Main recomp.c
    main_out = [
        "/* Generated by tools/codegen.py. Do not edit by hand. */",
        f'#include "{os.path.basename(base_name)}_funcs.h"',
        "#include <math.h>",
        "#include <string.h>",
        "#include <stdio.h>",
        "",
        SR_TO_W,
        ""
    ]
    for i in range(num_files):
        main_out.append(f"void sr_register_chunk_{i}(void);")
    main_out.append("\nvoid sr_register_all(void) {")
    main_out.append(f'    fprintf(stderr, "sr_register_all: starting {len(emitted)} registrations\\n");')
    for i in range(num_files):
        main_out.append(f"    sr_register_chunk_{i}();")
    main_out.append('    fprintf(stderr, "sr_register_all: completed\\n");\n}')

    with open(args[1], "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(main_out))

    for i in range(num_files):
        chunk = func_texts[i * funcs_per_chunk : (i + 1) * funcs_per_chunk]
        with open(f"{base_name}_{i}.c", "w", encoding="ascii", newline="\n") as f:
            f.write("/* Generated by tools/codegen.py. Do not edit by hand. */\n")
            f.write(f'#include "{os.path.basename(base_name)}_funcs.h"\n#include <math.h>\n#include <string.h>\n#include <stdio.h>\n\n')
            f.write(SR_TO_W + "\n\n")
            if SV_ENABLED:
                f.write(SR_SV_CHECK + "\n\n")
            f.write("\n\n".join(chunk) + "\n\n")
            f.write(f"void sr_register_chunk_{i}(void) {{\n")
            for ft in chunk:
                m = re.search(r'void ([fr])_([0-9a-fA-F]+)\(', ft)
                if m:
                    prefix = m.group(1)
                    a_val = int(m.group(2), 16)
                    f.write(f"    sr_register(0x{a_val:08x}u, {prefix}_{a_val:08x});\n")
            f.write("}\n")

    # Delete stale chunk files from previous runs
    j = num_files
    while True:
        stale_c = f"{base_name}_{j}.c"
        stale_o = f"{base_name}_{j}.o"
        if os.path.exists(stale_c) or os.path.exists(stale_o):
            if os.path.exists(stale_c): os.remove(stale_c)
            if os.path.exists(stale_o): os.remove(stale_o)
            j += 1
        else:
            break

    resume_count = sum(1 for a in emitted if a in resume_owners)
    print(f"wrote {args[1]} and {num_files} chunks ({funcs_per_chunk} functions/chunk); "
          f"{len(emitted) - resume_count} callable functions + {resume_count} resume entries, "
          f"{len(stubbed)} fallbacks. "
          f"Stub list: {base_name}_stubs.txt")
    if SV_ENABLED:
        print(f"static-verify: {SV_STATS['checks']} register assertions "
              f"across {SV_STATS['funcs']} functions")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
