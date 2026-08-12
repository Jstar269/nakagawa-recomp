# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

# Function-boundary analyzer for PSP ELF/PRX modules (Phase 2).
#
# Discovers function entry points without using the symbol table, by combining:
#   - the ELF entry point and the module's start/stop exports,
#   - direct-call targets (jal) found by sweeping the executable sections,
#   - constructor/destructor pointer arrays (.ctors/.dtors),
#   - function-pointer tables in read-only/data sections (pointers into code),
#   - recursive descent from each entry to bound the function and find more calls.
#
# When the module has a symbol table (the homebrew does), it reports recall against the
# STT_FUNC symbols. It also emits a TOML function inventory.
#
# Usage: analyze.py <elf> [--toml out.toml] [--quiet]

import os
import struct
import sys

import tomllib

from elf_bounds import validate_elf32_envelope

# Input classes supported by the Prx rebase/relocate path. Only genuinely
# relocatable images are routed there: PSP PRX-format ELFs (ET_SCE_PRX),
# relocatable ELFs (ET_REL), and executables that still carry relocation
# sections (SHT_RELA/SHT_REL, or the PSP types SHT_PSP_RELA/SHT_PSP_REL --
# e.g. the -Wl,-q PSPDEV ET_EXEC form, whose link-time addresses are
# 0-based and must be rebased). Ordinary ET_EXEC/ET_DYN ELFs without
# relocation sections at a nonzero base are handled as-is: their sections
# already carry concrete addresses.
ET_REL = 1
ET_EXEC = 2
ET_SCE_PRX = 0xFFA0
SHT_RELA = 4
SHT_REL = 9
SHT_PSP_RELA = 0x700000A0
SHT_PSP_REL = 0x700000A1


class Elf:
    def __init__(self, path, base=None):
        with open(path, "rb") as source:
            self.data = source.read()
        self.base = base
        d = self.data
        self.reloc = None
        envelope = validate_elf32_envelope(d, path)
        self.entry = envelope["entry"]
        self.phoff = envelope["phoff"]
        self.shoff = envelope["shoff"]
        self.phentsize = envelope["phentsize"]
        self.phnum = envelope["phnum"]
        self.shentsize = envelope["shentsize"]
        self.shnum = envelope["shnum"]
        self.shstrndx = envelope["shstrndx"]
        self.segments = [
            dict(
                type=p["type"],
                off=p["off"],
                vaddr=p["vaddr"],
                filesz=p["filesz"],
                memsz=p["memsz"],
                flags=p["flags"],
                idx=p["idx"],
            )
            for p in envelope["phdrs"]
        ]

        self.sections = []
        if self.shnum > 0 and self.shentsize > 0:
            for section in envelope["shdrs"]:
                name, typ, flags, addr, off, size, link, info, align, entsz = (
                    section["name"], section["typ"], section["flags"], section["addr"],
                    section["off"], section["size"], section["link"], section["info"],
                    section["align"], section["entsz"],
                )
                self.sections.append(dict(name=name, typ=typ, flags=flags, addr=addr,
                                          off=off, size=size, link=link, info=info, entsz=entsz))
            shstr = self.sections[self.shstrndx]
            for s in self.sections:
                e = shstr["off"] + s["name"]
                s["nm"] = d[e:d.find(b"\x00", e)].decode("ascii", "replace")
        else:
            # Reconstruct sections for stripped PRX
            for seg in self.segments:
                if seg["type"] == 1 and seg["filesz"] > 0:  # PT_LOAD
                    if seg["flags"] & 1:  # Executable
                        self.sections.append(dict(name=0, typ=1, flags=seg["flags"], addr=seg["vaddr"],
                                                  off=seg["off"], size=seg["filesz"], link=0, info=0, entsz=0, nm=".text"))
                    else:  # Data
                        self.sections.append(dict(name=0, typ=1, flags=seg["flags"], addr=seg["vaddr"],
                                                  off=seg["off"], size=seg["filesz"], link=0, info=0, entsz=0, nm=".data"))
            # Scan Segment 0 for PspModuleInfo
            code_seg = next((s for s in self.segments if s["type"] == 1 and (s["flags"] & 1)), None)
            if code_seg:
                seg_data = d[code_seg["off"] : code_seg["off"] + code_seg["filesz"]]
                module_info_offset = -1
                for o in range(0, len(seg_data) - 52, 4):
                    attr, ver = struct.unpack("<HH", seg_data[o:o+4])
                    name_bytes = seg_data[o+4:o+32]
                    if name_bytes[0] == 0:
                        continue
                    try:
                        name_len = name_bytes.index(0)
                        name = name_bytes[:name_len].decode("ascii")
                        if not all(32 <= ord(c) < 127 for c in name):
                            continue
                    except Exception:
                        continue
                    gp, ent, entend, stub, stubend = struct.unpack("<5I", seg_data[o+32:o+52])
                    max_size = len(d) + 0x1000000
                    if ent < max_size and entend < max_size and stub < max_size and stubend < max_size:
                        if entend >= ent and stubend >= stub:
                            if len(name) < 4 or gp == 0 or (ent % 4) != 0 or (stub % 4) != 0:
                                continue
                            module_info_offset = code_seg["vaddr"] + o
                            self.sections.append(dict(name=0, typ=1, flags=2, addr=module_info_offset,
                                                      off=code_seg["off"] + o, size=52, link=0, info=0, entsz=0, nm=".rodata.sceModuleInfo"))
                            if stubend > stub:
                                stub_off = -1
                                for s in self.segments:
                                    if s["vaddr"] <= stub < s["vaddr"] + s["memsz"]:
                                        stub_off = s["off"] + (stub - s["vaddr"])
                                        break
                                if stub_off != -1:
                                    self.sections.append(dict(name=0, typ=1, flags=6, addr=stub,
                                                              off=stub_off, size=stubend - stub, link=0, info=0, entsz=0, nm=".lib.stub"))
                            if entend > ent:
                                ent_off = -1
                                for s in self.segments:
                                    if s["vaddr"] <= ent < s["vaddr"] + s["memsz"]:
                                        ent_off = s["off"] + (ent - s["vaddr"])
                                        break
                                if ent_off != -1:
                                    self.sections.append(dict(name=0, typ=1, flags=2, addr=ent,
                                                              off=ent_off, size=entend - ent, link=0, info=0, entsz=0, nm=".lib.ent"))

                            meta_start = module_info_offset
                            if entend > ent and code_seg["vaddr"] <= ent < code_seg["vaddr"] + code_seg["memsz"]:
                                meta_start = min(meta_start, ent)
                            if stubend > stub and code_seg["vaddr"] <= stub < code_seg["vaddr"] + code_seg["memsz"]:
                                meta_start = min(meta_start, stub)
                            text_sec = next((s for s in self.sections if s.get("nm") == ".text"), None)
                            if text_sec:
                                text_sec["size"] = meta_start - text_sec["addr"]
                            break

        # PRX (ET_SCE_PRX = 0xFFA0), relocatable (ET_REL), and relocation-bearing
        # ELFs (e.g. the -Wl,-q PSPDEV ET_EXEC form): rebase to `base` and apply
        # the PSP relocations, so the code has concrete addresses. Non-PRX
        # classes are only rebased at a nonzero base (a zero base carries no
        # rebase work and matches the legacy HST-style input handling).
        # Ordinary ET_EXEC/ET_DYN ELFs without relocation sections keep their
        # as-is addressing. After this, read_at_vaddr serves the relocated
        # image and section/segment addresses are in the rebased space.
        e_type = struct.unpack("<H", d[16:18])[0]
        has_reloc_sections = any(
            s["typ"] in (SHT_RELA, SHT_REL, SHT_PSP_RELA, SHT_PSP_REL)
            for s in self.sections
        )
        if base is not None and (
            e_type == ET_SCE_PRX or (base != 0 and (e_type == ET_REL or has_reloc_sections))
        ):
            from prxload import Prx
            prx = Prx(path, base)
            prx.relocate()
            self.reloc = prx
            self.entry = (self.entry + base) & 0xFFFFFFFF
            for s in self.sections:
                s["addr"] += base
            for s in self.segments:
                s["vaddr"] += base

    def sec(self, name):
        return next((s for s in self.sections if s.get("nm") == name), None)

    def read_at_vaddr(self, vaddr, n):
        # For a relocated PRX, serve the rebased+relocated image directly.
        if self.reloc is not None:
            if self.reloc.lo <= vaddr < self.reloc.lo + len(self.reloc.mem):
                o = vaddr - self.reloc.lo
                return bytes(self.reloc.mem[o:o + n])
            return None
        # Otherwise read from the loaded image via PT_LOAD program headers. Sections are not
        # used here because non-code allocated sections (.reginfo, .MIPS.abiflags) can share a
        # vaddr with .text and would otherwise shadow the real instruction bytes.
        for seg in self.segments:
            if seg["type"] == 1 and seg["vaddr"] <= vaddr < seg["vaddr"] + seg["filesz"]:
                off = seg["off"] + (vaddr - seg["vaddr"])
                return self.data[off:off + n]
        return None

    def func_symbols(self):
        symtab, strtab = self.sec(".symtab"), self.sec(".strtab")
        if not symtab or not strtab:
            return None
        d = self.data
        out = set()
        for i in range(symtab["size"] // symtab["entsz"]):
            o = symtab["off"] + i * symtab["entsz"]
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack("<IIIBBH", d[o:o + 16])
            if (st_info & 0xF) == 2 and st_value != 0:  # STT_FUNC
                out.add(st_value)
        return out


EXEC_SECTIONS = (".text", ".init", ".fini", ".sceStub.text", ".data", ".rodata.sceResident")


def section_bytes(elf, s):
    # For a relocated PRX, read the section from the rebased+relocated image so that any
    # R_MIPS_32 function pointers it holds are already concrete. Otherwise read the raw file.
    if elf.reloc is not None:
        b = elf.read_at_vaddr(s["addr"], s["size"])
        return b if b is not None else b""
    return elf.data[s["off"]:s["off"] + s["size"]]


EXTRA_SPAN_ENV = "HST_EXTRA_SPANS"
UINT32_END_MAX = 0x100000000


def parse_extra_spans(text, source=EXTRA_SPAN_ENV):
    """Parse one explicit ``"lo,hi"`` executable span into ``[(lo, hi)]``.

    An empty/whitespace-only string means *no* extra span. `source` only names the
    origin in error messages (an environment variable or a command-line option), so
    a malformed value fails closed with an actionable message instead of silently
    analyzing the wrong address range.
    """
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    parts = [part.strip() for part in text.split(",")]
    if len(parts) != 2:
        raise RuntimeError("%s must look like 'lo,hi' (got %r)" % (source, text))
    try:
        lo, hi = (int(part, 0) for part in parts)
    except ValueError as exc:
        raise RuntimeError(
            "%s must contain numeric addresses (got %r)" % (source, text)
        ) from exc
    if lo < 0 or hi < 0:
        raise RuntimeError("%s must not contain negative addresses (got %r)" % (source, text))
    if hi <= lo:
        raise RuntimeError("%s requires hi > lo (got %r)" % (source, text))
    if hi > UINT32_END_MAX:
        raise RuntimeError("%s exceeds the 32-bit guest address space (got %r)" % (source, text))
    return [(lo, hi)]


def analyzer_span_from_env(environ=None):
    """Return the explicit extra executable span the caller put in the environment.

    This is the *only* place a span may enter from ambient process state, and it is
    called exclusively from CLI entry points for the primary image. Library callers
    (`exec_ranges`, `analyze`) never consult the environment, so an inherited value
    cannot leak into a rebased extra guest module analyzed in the same process.
    """
    if environ is None:
        environ = os.environ
    return parse_extra_spans(environ.get(EXTRA_SPAN_ENV), EXTRA_SPAN_ENV)


def resolve_extra_spans(cli_text, environ=None, source="--extra-span"):
    """Resolve the effective span from an explicit option plus the environment.

    An explicit option wins, but a *conflicting* environment value fails closed
    rather than letting the two disagree silently about what was analyzed.
    """
    from_cli = parse_extra_spans(cli_text, source)
    from_env = analyzer_span_from_env(environ)
    if from_cli is None:
        return from_env
    if from_env is not None and from_env != from_cli:
        raise RuntimeError(
            "%s=%r conflicts with %s=%r" % (source, cli_text, EXTRA_SPAN_ENV, from_env)
        )
    return from_cli


def exec_ranges(elf, extra_spans=None):
    # Build the executable address ranges from the section table, restricted to actual code
    # sections. PSP PRX ELFs declare almost every section with SHF_EXECINSTR (the loader
    # maps the whole module flat with WAX), so a naive flag check still treats .data/.rodata
    # tables as code -- the analyzer then sees fake "functions" at every data word and the
    # codegen emits thousands of bogus stubs in .data. Use the section *name* as the source
    # of truth: only .text and its siblings are recompiled. .lib.ent / .lib.stub are linker
    # bookkeeping (import tables / module stubs), filled in by the loader at runtime -- never
    # to be recompiled. .rodata.* / .data are data sections; their word content merely looks
    # like MIPS to a naive decoder.
    spans = []
    for s in elf.sections:
        if s["typ"] != 1:  # only PROGBITS
            continue
        name = s.get("nm")
        if name is None:
            if elf.shnum > 0 and elf.shentsize > 0:
                shstr = elf.sections[elf.shstrndx]
                name = elf.data[shstr["off"] + s["name"]: elf.data.find(b"\x00", shstr["off"] + s["name"])].decode("ascii", "replace")
            else:
                continue
        # Real code is .text and PSP-specific .sceStub.text sections. Everything else
        # (.lib.*, .rodata.*, .data, etc.) is data/linker bookkeeping.
        if name != ".text" and name != ".sceStub.text":
            continue
        if s["addr"] is None or s["size"] is None or s["size"] <= 0:
            continue
        spans.append((s["addr"], s["addr"] + s["size"]))
    # Extra code spans that live outside the section table are *caller-supplied title
    # configuration*, never a built-in default: a generic base-zero image must not
    # silently inherit some other title's executable span. The span is only meaningful
    # for an unrebased image -- a rebased guest lives at different addresses -- so
    # applying one to a rebased module fails closed instead of analyzing garbage.
    if extra_spans:
        effective_base = 0 if elf.base is None else elf.base
        if effective_base != 0:
            raise RuntimeError(
                "explicit extra executable spans cannot be applied when "
                "GAME_BASE != 0 (got GAME_BASE=0x%x)" % effective_base
            )
        spans.extend((lo, hi) for lo, hi in extra_spans)
    if not spans:
        return [(0, 0)]
    spans.sort()
    merged = [spans[0]]
    for lo, hi in spans[1:]:
        if lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def in_ranges(addr, ranges):
    return any(lo <= addr < hi for lo, hi in ranges)


def trace_function(elf, start, ranges, covered, calls, hc):
    # Recursive descent over one function's intra-procedural control flow from `start`.
    # Adds every instruction address reached to `covered`, and every direct-call (jal) target
    # to `calls`. Calls return, so execution continues after the delay slot; jr and j and an
    # unconditional b end a path (no fall-through). Conditional branches fork: follow the
    # target and continue past the delay slot.
    stack = [start]
    local = set()
    while stack:
        pc = stack.pop()
        # Stop the linear scan when it reaches a DIFFERENT high-confidence function entry
        # (a jal target / address-taken code pointer / export) -- but only when that entry
        # is a genuine function boundary. hc is built from jal targets, la-materialized
        # code pointers, and exports (internal block labels live in `noisy`), so it can
        # also contain internal switch-case/jump-table landing pads that happen to be
        # address-taken without ever being a *direct* call target. Those pads never carry
        # an o32 prologue (`addiu sp, sp, -X`) of their own; treating them as hard barriers
        # truncates the owning function's switch body. Signature-aware check: a foreign hc
        # hit only truncates when it looks like a real entry (has the sp-alloc prologue) or
        # is a known direct-call target (`calls`, populated by jal in this same pass and
        # prior functions). Anything else falls through and is traced as this function's
        # own code, exactly like the case that recovers f_0000d518 -> f_0000d530 (that
        # split target has neither trait working against it: it IS a real jal target).
        # Without this guard entirely, a function that ends by falling through into the
        # next function -- e.g. one whose last act is `jal <noreturn>` with no epilogue of
        # its own -- swallows that adjacent function and adopts ITS `jr $ra` as its own
        # terminator, dropping its real return path. The mis-split then makes every caller
        # reload s0-s7/ra from off-by-frame stack slots (callee-save corruption; see
        # docs/audit/F3E_CALLEESAVE.md, the f_0005a648 bug class). The recursive jr-ra-fold
        # below (for hoisted trailing epilogues) still runs for pc+8 addresses that are not
        # treated as boundaries here.
        while in_ranges(pc, ranges) and pc not in local:
            if pc != start and pc in hc:
                insn_bytes = elf.read_at_vaddr(pc, 4)
                stop = True
                if insn_bytes and len(insn_bytes) == 4:
                    insn = int.from_bytes(insn_bytes, 'little')
                    is_prologue = (insn >> 16) == 0x27BD and (insn & 0x8000)
                    if not is_prologue and pc not in calls:
                        # Internal jump-table/switch landing pad, not a real
                        # function entry: keep tracing it as part of this function.
                        stop = False
                if stop:
                    break
            local.add(pc)
            covered.add(pc)
            wb = elf.read_at_vaddr(pc, 4)
            if wb is None or len(wb) < 4:
                break
            word = int.from_bytes(wb, 'little')
            op = word >> 26
            funct = word & 0x3F
            if op == 3:  # jal: direct call, returns -> continue past delay slot
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    calls.add(target)
                covered.add(pc + 4)
                pc += 8
                continue
            if op == 2:  # j: tail call or local jump
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                covered.add(pc + 4)
                if in_ranges(target, ranges):
                    stub = elf.sec(".sceStub.text")
                    is_stub_target = stub and stub["addr"] <= target < stub["addr"] + stub["size"]
                    tb = elf.read_at_vaddr(target, 4)
                    is_prologue = False
                    if tb and len(tb) == 4:
                        tw = int.from_bytes(tb, 'little')
                        if (tw >> 16) == 0x27BD and (tw & 0x8000):
                            is_prologue = True
                    if target in hc or is_stub_target or is_prologue:
                        calls.add(target)
                        break
                    else:
                        stack.append(target)
                        break
            if op == 0 and funct == 0x08:  # jr (return / computed): end this path
                covered.add(pc + 4)
                # If the instruction after the delay slot is also jr $ra, the
                # function has multiple return paths (e.g. a tiny comparator
                # stub merged into the same address range). Continue tracing
                # so those instructions are covered by this function rather
                # than detected as a standalone thin stub.
                nx = elf.read_at_vaddr(pc + 8, 4)
                if nx and len(nx) == 4 and int.from_bytes(nx, 'little') == 0x03e00008:
                    pc += 8
                    continue
                break
            # Branches: REGIMM (1), beq/bne/blez/bgtz (4-7) and their likely forms (20-23),
            # and FPU bc1 (cop1 with rs=8). Target is intra-function; fork and continue.
            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0x11 and ((word >> 21) & 0x1F) == 8)
            if is_branch:
                off = word & 0xFFFF
                off = off - 0x10000 if off & 0x8000 else off
                target = pc + 4 + (off << 2)
                if in_ranges(target, ranges):
                    stack.append(target)
                covered.add(pc + 4)
                # Unconditional b (beq $zero,$zero): no fall-through after the delay slot.
                if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
                    break
                pc += 8
                continue
            pc += 4


def _is_hard_terminator(word):
    # True for instructions after which control never falls through to the next
    # slot-pair: `j`, any `jr`, or the unconditional `b` idiom (beq $0,$0).
    # `jal`/`jalr`/`bal` return, so they are NOT terminators here.
    op = word >> 26
    return (op == 2 or (op == 0 and (word & 0x3F) == 0x08)
            or (op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0))


#: Public alias. The entry-role audit needs exactly this fall-through rule to
#: decide whether an address can be entered by falling into it, and must not
#: carry a second, drifting copy of it.
is_hard_terminator = _is_hard_terminator


def _is_trailing_epilogue(elf, x):
    # A `jr $ra` at x is a hoisted/outlined return-epilogue (not a real standalone
    # function) when it sits 8 bytes after another `jr $ra` (the owning function's
    # real return), separated by that return's delay slot. Such stubs (e.g.
    # `return a0`) must be folded into the preceding function, not emitted as their
    # own function -- otherwise trace diffs misalign on the mis-split boundary.
    if x < 8:
        return False
    a = elf.read_at_vaddr(x, 4)
    b = elf.read_at_vaddr(x - 8, 4)
    if a is None or b is None or len(a) < 4 or len(b) < 4:
        return False
    return int.from_bytes(a, 'little') == 0x03e00008 and int.from_bytes(b, 'little') == 0x03e00008


def direct_j_edges(elf, ranges, targets=None):
    """Return unconditional direct-``j`` edges in executable ranges.

    The analyzer already treats ``j`` as a weak boundary signal because the
    instruction is used for both tail transfers and intra-procedural control
    flow.  Keep the edge census separate from that boundary decision: callers
    need the actual source PCs to distinguish a one-way shared-tail transfer
    from a backwards loop edge.  ``targets`` optionally limits the result to a
    known entry set without changing the scan itself.

    The returned mapping is ``target -> tuple(sorted(source_pcs))``.  Only
    in-range targets are reported; data words outside executable ranges are
    never interpreted as edges.
    """
    wanted = None if targets is None else set(targets)
    edges = {}
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            # Small synthetic images used by analyzer/codegen regressions often
            # expose only the scalar read surface.  Keep the production path
            # bulk-read based, but make the evidence helper honor that same
            # read contract without requiring an ELF container.
            words = (
                (source, elf.read_at_vaddr(source, 4))
                for source in range(lo, hi, 4)
            )
        else:
            words = (
                (lo + off, blob[off:off + 4])
                for off in range(0, len(blob) - 3, 4)
            )
        for source, raw in words:
            if raw is None or len(raw) < 4:
                continue
            word = int.from_bytes(raw, "little")
            if word >> 26 != 2:  # unconditional direct jump
                continue
            target = (source & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if not in_ranges(target, ranges):
                continue
            if wanted is not None and target not in wanted:
                continue
            edges.setdefault(target, []).append(source)
    return {target: tuple(sorted(sources)) for target, sources in edges.items()}


#: REGIMM ``rt`` selectors that actually encode a PC-relative branch:
#: ``bltz``/``bgez`` (0/1), their likely forms (2/3), and the linking
#: ``bltzal``/``bgezal``/``bltzall``/``bgezall`` (0x10-0x13).  Every other
#: REGIMM ``rt`` is a trap-immediate (``tgei``, ``tlti``, ``teqi``, ...) or
#: ``synci``, whose immediate field is a trap code -- decoding it as a branch
#: displacement manufactures an edge that does not exist.  ``trace_function``
#: above is deliberately left alone: widening or narrowing its notion of a
#: branch would move recovered function extents, which is a separate change.
#: The evidence census below has no such coupling and is therefore exact.
REGIMM_BRANCH_RT = frozenset({0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13})

#: The subset of :data:`REGIMM_BRANCH_RT` that writes ``$ra``.  ``bal`` is
#: ``bgezal $zero``; these are *calls* wearing a branch encoding, so their
#: target is a callable boundary and never continuation evidence.
REGIMM_LINK_RT = frozenset({0x10, 0x11, 0x12, 0x13})

#: Branch edge kinds reported by :func:`direct_branch_edges`.
BRANCH_COND = "cond"          # an ordinary two-way conditional branch
BRANCH_UNCOND = "uncond"      # the ``b`` idiom (``beq $zero, $zero``): no fall-through
BRANCH_LINK = "link"          # ``bal``/``bgezal``/...: a call, not a branch edge


def _iter_code_words(elf, ranges):
    """Yield ``(vaddr, word)`` over executable ranges.

    Bulk-read first, falling back to the scalar ``read_at_vaddr`` contract that
    the owned synthetic regressions expose, so the same census runs host-neutral
    without an ELF container.
    """
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            for addr in range(lo, hi, 4):
                raw = elf.read_at_vaddr(addr, 4)
                if raw is not None and len(raw) >= 4:
                    yield addr, int.from_bytes(raw[:4], "little")
        else:
            for off in range(0, len(blob) - 3, 4):
                yield lo + off, int.from_bytes(blob[off:off + 4], "little")


def branch_target(source, word):
    """PC-relative branch target of ``word`` at ``source``, or ``None``.

    ``None`` means the word is not a direct branch at all.  The returned kind is
    supplied separately by :func:`direct_branch_edges`; this helper exists so the
    decode lives in exactly one place.
    """
    op = word >> 26
    if op == 1:
        if ((word >> 16) & 0x1F) not in REGIMM_BRANCH_RT:
            return None
    elif op in (4, 5, 6, 7, 20, 21, 22, 23):
        pass
    elif op == 0x11 and ((word >> 21) & 0x1F) == 8:
        pass
    else:
        return None
    offset = word & 0xFFFF
    offset -= 0x10000 if offset & 0x8000 else 0
    return (source + 4 + (offset << 2)) & 0xFFFFFFFF


def branch_kind(word):
    """Classify a direct branch as conditional, unconditional, or linking."""
    op = word >> 26
    if op == 1 and ((word >> 16) & 0x1F) in REGIMM_LINK_RT:
        return BRANCH_LINK
    if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
        return BRANCH_UNCOND
    return BRANCH_COND


def direct_branch_edges(elf, ranges, targets=None):
    """Return direct PC-relative branch edges in executable ranges.

    The companion of :func:`direct_j_edges` for the conditional-branch slice of
    issue #51.  A conditional branch differs from ``j`` in ways the caller must
    see rather than infer, so the mapping is ``target -> ((source, kind), ...)``
    with the kind preserved:

    * an ordinary conditional branch leaves a **fall-through** predecessor
      behind it, so its target is not entered by that edge alone;
    * the ``b`` idiom has no fall-through, exactly like ``j``;
    * a **linking** REGIMM branch is a call and makes its target a callable
      boundary -- keeping it in the census, rather than dropping it, is what
      lets the audit veto instead of silently missing the evidence.

    ``targets`` optionally limits the result to a known entry set.  Only in-range
    targets are reported.
    """
    wanted = None if targets is None else set(targets)
    edges = {}
    for source, word in _iter_code_words(elf, ranges):
        target = branch_target(source, word)
        if target is None or not in_ranges(target, ranges):
            continue
        if wanted is not None and target not in wanted:
            continue
        edges.setdefault(target, []).append((source, branch_kind(word)))
    return {target: tuple(sorted(srcs)) for target, srcs in edges.items()}


def code_pointer_evidence(elf, ranges):
    """Census of every way an in-range address is reachable other than by falling into it.

    Returns ``{kind: frozenset(addresses)}`` over four independent kinds:

    ``jal``
        a direct call target.
    ``branch-link``
        a ``bal``/``bgezal``-family target -- also a call.
    ``immediate``
        an address materialized as a constant in code (``lui`` + ``addiu``/``ori``),
        i.e. taken for an indirect call, a callback, or a stored pointer.
    ``data``
        an in-range word found in a non-code section: function-pointer tables,
        vtables, jump tables, export/import tables.

    :func:`analyze` folds these signals into its start set and then discards
    *which* signal fired.  The entry-role audit needs them kept apart, because
    each one contradicts a continuation classification for a different reason:
    the first two prove a fresh-call contract outright, while the last two prove
    only that the address is independently dispatchable with an incoming
    contract this analysis cannot see.
    """
    evidence = {kind: set() for kind in ("jal", "branch-link", "immediate", "data")}

    hireg = {}
    clear_next = False
    for addr, word in _iter_code_words(elf, ranges):
        if clear_next:
            hireg.clear()
            clear_next = False
        op = word >> 26
        if op == 3:  # jal
            target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            if in_ranges(target, ranges):
                evidence["jal"].add(target)
        elif op == 1 and ((word >> 16) & 0x1F) in REGIMM_LINK_RT:
            target = branch_target(addr, word)
            if target is not None and in_ranges(target, ranges):
                evidence["branch-link"].add(target)

        if op == 0x0F:  # lui rt, imm
            hireg[(word >> 16) & 0x1F] = (word & 0xFFFF) << 16
        elif op in (0x09, 0x0D):  # addiu / ori: the low half of an `la`
            base = hireg.get((word >> 21) & 0x1F)
            if isinstance(base, int):
                if op == 0x09:
                    low = (word & 0xFFFF) - (0x10000 if word & 0x8000 else 0)
                    value = (base + low) & 0xFFFFFFFF
                else:
                    value = base | (word & 0xFFFF)
                if in_ranges(value, ranges) and (value & 3) == 0:
                    evidence["immediate"].add(value)
        if branch_target(addr, word) is not None or op in (2, 3) or (
            op == 0 and (word & 0x3F) in (0x08, 0x09)
        ):
            clear_next = True

    # Non-code sections. `sections` is absent on the owned synthetic word images
    # used by the regressions, which carry no data segment at all; treat that as
    # "no data evidence" rather than requiring the fixtures to fake a container.
    for section in getattr(elf, "sections", ()):
        if section.get("typ") != 1 or not section.get("size"):
            continue
        if section.get("nm") in (".text", ".sceStub.text"):
            continue
        blob = section_bytes(elf, section)
        for off in range(0, len(blob) - 3, 4):
            value = int.from_bytes(blob[off:off + 4], "little")
            if in_ranges(value, ranges) and (value & 3) == 0:
                evidence["data"].add(value)

    return {kind: frozenset(values) for kind, values in evidence.items()}


def analyze(elf, extra_spans=None):
    ranges = exec_ranges(elf, extra_spans=extra_spans)
    text = elf.sec(".text")

    def in_text(a):
        return in_ranges(a, ranges) and (a & 3) == 0

    # High-confidence function starts: addresses that are genuinely entered as a function,
    # not internal blocks. These seed the extent tracing below.
    hc = set()
    noisy = set()
    jtails = set()  # every in-range `j` target; candidates for tail-call promotion below
    if in_ranges(elf.entry, ranges):
        hc.add(elf.entry)

    # Build a file-offset -> vaddr lookup from PT_LOAD segments (needed because
    # the raw file's byte offset differs from the guest virtual address for PRX/rebased ELFs).
    # If no PT_LOAD segment covers a given file offset, the address is skipped.
    loads = [s for s in elf.segments if s["type"] == 1]
    def file_to_vaddr(fo):
        for s in loads:
            if s["off"] <= fo < s["off"] + s["filesz"]:
                return s["vaddr"] + (fo - s["off"])
        return None

    # NEW: Add all potential prologues and post-terminator addresses as seeds
    data = elf.data
    for i in range(0, len(data) - 4, 4):
        w = int.from_bytes(data[i:i+4], 'little')
        va = file_to_vaddr(i)
        if va is None:
            continue
        # addiu sp, sp, -X (0x27bdXXXX where XXXX is negative)
        if (w >> 16) == 0x27BD and (w & 0x8000) and (va & 3) == 0:
            noisy.add(va)
        # jr $ra / unconditional b terminators. The instruction after the delay slot
        # is very likely the start of a new function.
        # We explicitly omit 'j' (op == 2) because PSP pointers start with 0x08, making
        # every pointer look like a 'j' instruction, which pollutes the function list
        # with data/string addresses.
        is_uncond = (w == 0x03e00008) or ((w >> 16) == 0x1000)
        if is_uncond and in_ranges(va + 8, ranges) and ((va + 8) & 3) == 0:
            noisy.add(va + 8)

    # Module export pointers and constructor/destructor arrays point at real functions.
    # NOTE: .lib.ent is parsed separately below (structures with func_table), don't scan raw.
    for nm in (".rodata.sceModuleInfo", ".rodata.sceResident",
               ".ctors", ".dtors", ".init_array", ".fini_array"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = int.from_bytes(blob[o:o + 4], 'little')
            if in_ranges(val, ranges) and (val & 3) == 0:
                hc.add(val)
    # Reconstruct exports from .lib.ent
    libent = elf.sec(".lib.ent")
    if libent:
        blob = section_bytes(elf, libent)
        idx = 0
        while idx < len(blob) - 19:
            name_ptr, ver, flags, size, num_vars, num_funcs, func_table, var_table = struct.unpack(
                "<IHHBBHII", blob[idx:idx + 20])
            entry_size_bytes = size * 4
            if entry_size_bytes < 20:
                entry_size_bytes = 20
            if num_funcs > 0 and func_table != 0:
                for f_idx in range(num_funcs):
                    fptr_bytes = elf.read_at_vaddr(func_table + f_idx * 4, 4)
                    if fptr_bytes:
                        fptr = int.from_bytes(fptr_bytes, 'little')
                        if in_ranges(fptr, ranges) and (fptr & 3) == 0:
                            hc.add(fptr)
            if num_vars > 0 and var_table != 0:
                for v_idx in range(num_vars):
                    vptr_bytes = elf.read_at_vaddr(var_table + v_idx * 4, 4)
                    if vptr_bytes:
                        vptr = int.from_bytes(vptr_bytes, 'little')
                        if in_ranges(vptr, ranges) and (vptr & 3) == 0:
                            hc.add(vptr)
            idx += entry_size_bytes


    # Reconstruct stubs from .sceStub.text or imports
    stubs_sec = elf.sec(".sceStub.text")
    if stubs_sec:
        if elf.reloc is not None:
            try:
                from imports import parse_imports
                impmap = parse_imports(elf)
                for a in impmap.keys():
                    hc.add(a)
            except Exception as e:
                sys.stderr.write(f"warning: failed to parse imports in analyzer: {e}\n")
        else:
            for a in range(stubs_sec["addr"], stubs_sec["addr"] + stubs_sec["size"], 8):
                hc.add(a)


    # Function-pointer tables in read-only/data sections (callbacks reached via jalr).
    for nm in (".rodata", ".data", ".sdata", ".rodata.sceResident"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = int.from_bytes(blob[o:o + 4], 'little')
            if in_ranges(val, ranges) and (val & 3) == 0:
                hc.add(val)

    # Sweep executable sections: jal targets are calls (high-confidence functions); la-style
    # address materialization into code is an indirect-call target. j targets, prologues, and
    # the instruction after an unconditional terminator are weaker "block boundary" signals
    # kept separately and used only to fill gaps the call graph does not cover.
    sys.stderr.write(f"SCANNING RANGES: {ranges}\n")
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            continue
        hireg = {}
        clear_next = False
        for off in range(0, len(blob) - 3, 4):
            if clear_next:
                hireg.clear()
                clear_next = False
            word = int.from_bytes(blob[off:off + 4], 'little')
            addr = lo + off
            op = word >> 26
            if op == 3:  # jal: direct call
                target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    hc.add(target)
            elif op == 2:  # j: tail call or intra-function goto -> weak
                target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    noisy.add(target)
                    jtails.add(target)
            if (word >> 16) == 0x27BD and (word & 0x8000):  # addiu $sp,$sp,-N prologue
                noisy.add(addr)
            is_uncond = (op == 2 or (op == 0 and (word & 0x3F) == 0x08)
                         or (op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0))
            if is_uncond and in_ranges(addr + 8, ranges):
                tb = elf.read_at_vaddr(addr + 8, 4)
                is_jr_ra = tb is not None and len(tb) == 4 and int.from_bytes(tb, 'little') == 0x03e00008
                if is_jr_ra:
                    if op == 0 and (word & 0x3F) == 0x08:
                        # Trailing return-epilogue: the function returns at `addr`, and the
                        # `jr $ra` immediately after its delay slot (addr+8) is a hoisted /
                        # outlined epilogue stub (e.g. `return a0`), not a separate function.
                        # trace_function already folds addr+8 into the preceding function's
                        # covered set, so keep it only as a weak signal; gap-fill skips it
                        # because it is already covered by that function.
                        noisy.add(addr + 8)
                    else:
                        hc.add(addr + 8)
                else:
                    noisy.add(addr + 8)

            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0x11 and ((word >> 21) & 0x1F) == 8)
            is_jump_or_call = op in (2, 3) or (op == 0 and (word & 0x3F) in (0x08, 0x09))
            if is_branch or is_jump_or_call:
                clear_next = True

            if op == 0x0F:  # lui rt, imm
                hireg[(word >> 16) & 0x1F] = (word & 0xFFFF) << 16
            elif op == 0x09:  # addiu rt, rs, simm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg and isinstance(hireg[rs], int):
                    val = (hireg[rs] + ((word & 0xFFFF) - (0x10000 if word & 0x8000 else 0))) & 0xFFFFFFFF
                    if in_text(val):
                        tb = elf.read_at_vaddr(val, 4)
                        is_jr_ra = tb is not None and len(tb) == 4 and int.from_bytes(tb, 'little') == 0x03e00008
                        if is_jr_ra:
                            if _is_trailing_epilogue(elf, val):
                                noisy.add(val)
                            else:
                                hc.add(val)
                        else:
                            noisy.add(val)
            elif op == 0x0D:  # ori rt, rs, imm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg and isinstance(hireg[rs], int):
                    val = hireg[rs] | (word & 0xFFFF)
                    if in_text(val):
                        tb = elf.read_at_vaddr(val, 4)
                        is_jr_ra = tb is not None and len(tb) == 4 and int.from_bytes(tb, 'little') == 0x03e00008
                        if is_jr_ra:
                            if _is_trailing_epilogue(elf, val):
                                noisy.add(val)
                            else:
                                hc.add(val)
                        else:
                            noisy.add(val)
            elif op == 0 and (word & 0x3F) == 0x21: # addu rd, rs, rt
                rs_reg, rt_reg, rd_reg = (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], int):
                    hireg[rd_reg] = hireg[rs_reg]
                elif rt_reg in hireg and isinstance(hireg[rt_reg], int):
                    hireg[rd_reg] = hireg[rt_reg]
            elif op == 0x23:  # lw rt, simm(rs)
                rs_reg = (word >> 21) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], int):
                    table_addr = (hireg[rs_reg] + ((word & 0xFFFF) - (0x10000 if word & 0x8000 else 0))) & 0xFFFFFFFF
                    hireg[(word >> 16) & 0x1F] = ("table", table_addr)
            elif op == 0x2B:  # sw rt, simm(rs)
                rt_reg = (word >> 16) & 0x1F
                if rt_reg in hireg and isinstance(hireg[rt_reg], int):
                    ptr = hireg[rt_reg]
                    # Heuristic: if we store a pointer that itself points to code, it's likely a vtable.
                    # This catches C++ constructors setting up the vtable pointer.
                    tb = elf.read_at_vaddr(ptr, 4)
                    if tb and len(tb) == 4:
                        val = int.from_bytes(tb, 'little')
                        if in_text(val):
                            for j in range(512):
                                tb = elf.read_at_vaddr(ptr + j*4, 4)
                                if not tb or len(tb) < 4: break
                                val = int.from_bytes(tb, 'little')
                                if in_text(val): hc.add(val)
                                else: break
            elif op == 0 and (word & 0x3F) in (0x08, 0x09):  # jr rs / jalr rd, rs
                rs_reg = (word >> 21) & 0x1F
                if rs_reg in hireg and isinstance(hireg[rs_reg], tuple) and hireg[rs_reg][0] == "table":
                    table_addr = hireg[rs_reg][1]
                    for j in range(512): # Scan up to 512 entries
                        tb = elf.read_at_vaddr(table_addr + j*4, 4)
                        if not tb or len(tb) < 4: break
                        val = int.from_bytes(tb, 'little')
                        if in_text(val): hc.add(val)
                        else: break

    # Trace each function's extent from the high-confidence seeds, following discovered calls.
    # `covered` ends up holding every instruction that belongs to some known function, so the
    # weak signals that land inside a function body (internal blocks) can be discarded.
    covered = set()
    calls = set()
    functions = set(hc)
    work = list(hc)
    while work:
        s = work.pop()
        if in_ranges(s, ranges):
            trace_function(elf, s, ranges, covered, calls, hc)
        for t in list(calls):
            if t not in functions and in_ranges(t, ranges):
                functions.add(t)
                work.append(t)

    # Tail-call promotion: a `j` target that no fallthrough can reach (the word 8
    # bytes before it -- the previous slot's terminator position -- is `jr`, `j`,
    # or an unconditional `b`) is a function entered only by jumps: the shared-
    # return / compiler-split cold-path / trampoline idiom. Left unpromoted,
    # whichever function linearly covers the target absorbs its body, and every
    # OTHER function's tail `j` has no entry to dispatch to at runtime -- a
    # silent NONPLT_MISS of the 0x000e1724 class (ISSUES.md 2026-07-18; found by
    # tools/ghidra_crosscheck.py, 16 live cases). Promotion is safe: codegen's
    # continuation machinery stops the covering function's extent at the new
    # entry and emits a continuation call, so both owners stay correct.
    for t in sorted(jtails):
        if t in functions or not in_ranges(t, ranges):
            continue
        wb = elf.read_at_vaddr(t - 8, 4)
        if wb is None or len(wb) < 4:
            continue
        if not _is_hard_terminator(int.from_bytes(wb, 'little')):
            continue
        functions.add(t)
        trace_function(elf, t, ranges, covered, calls, hc)
        for c in list(calls):
            if c not in functions and in_ranges(c, ranges):
                functions.add(c)
                trace_function(elf, c, ranges, covered, calls, hc)

    # Gap fill: a weak-signal address that no known function covers is an indirect-only
    # function (reached through a register the call graph could not resolve). Add it and trace
    # it, which may reveal further calls. Iterate until stable.
    changed = True
    while changed:
        changed = False
        for c in sorted(noisy):
            if in_ranges(c, ranges) and c not in covered and c not in functions:
                # Safety net: if a candidate address immediately follows a
                # covered instruction AND starts with jr $ra, it is a thin
                # return stub that belongs to the preceding function (e.g. a
                # comparator callback embedded in a function's address range).
                # Skip it rather than creating a spurious standalone function.
                if (c - 4) in covered or (c - 8) in covered:
                    wb = elf.read_at_vaddr(c, 4)
                    if wb and len(wb) == 4 and int.from_bytes(wb, 'little') == 0x03e00008:
                        covered.add(c)
                        covered.add(c + 4)
                        continue
                # Reverse safety net: if a candidate address is immediately
                # BEFORE a covered instruction AND starts with jr $ra, it is
                # a tail stub that belongs to the preceding function. Skip it
                # rather than creating a spurious standalone function.
                if (c + 4) in covered or (c + 8) in covered:
                    wb = elf.read_at_vaddr(c, 4)
                    if wb and len(wb) == 4 and int.from_bytes(wb, 'little') == 0x03e00008:
                        covered.add(c)
                        covered.add(c + 4)
                        continue
                functions.add(c)
                trace_function(elf, c, ranges, covered, calls, hc)
                for t in list(calls):
                    if t not in functions and in_ranges(t, ranges):
                        functions.add(t)
                        trace_function(elf, t, ranges, covered, calls, hc)
                changed = True

    return functions, ranges


# ---- TOML model: a function inventory the codegen reads and a human can correct ----------

def build_model(elf, starts):
    text = elf.sec(".text")
    stub = elf.sec(".sceStub.text")

    def kind(a):
        if stub and stub["addr"] <= a < stub["addr"] + stub["size"]:
            return "import_stub"
        return "function"

    return {
        "module": (elf.sec(".rodata.sceModuleInfo") is not None) and "prx" or "elf",
        "entry": int(elf.entry) & 0xFFFFFFFF,
        "functions": [{"addr": int(a) & 0xFFFFFFFF, "kind": kind(a)} for a in sorted(starts)],
    }


# Per-function fields are emitted in this order; unknown extra fields are appended so a
# hand-corrected TOML round-trips without loss.
_FUNC_ORDER = ["addr", "kind", "name", "skip", "note"]


def _fmt_value(key, val):
    if key == "addr" or (key == "entry"):
        # Mask to a guest 32-bit address before formatting. Unrelocated PRX entries
        # come in as 0x1..XXXXXXXX (the kernel module base bit, 0x100000000, leaks
        # through); without the mask the TOML prints >32-bit hex and the loader/roundtrip
        # comparisons treat the value as 0x1XXXXXXXX on the next pass.
        return f"0x{int(val) & 0xFFFFFFFF:08x}"
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, int):
        return str(val)
    return '"' + str(val).replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit_toml(model, path):
    lines = [
        "# Function inventory emitted by tools/analyze.py. Machine-generated, human-editable.",
        "# Edit names/skip/note or add functions; the loader round-trips edits without loss.",
        f'module = {_fmt_value("module", model["module"])}',
        f'entry = {_fmt_value("entry", model["entry"])}',
        "",
    ]
    for fn in model["functions"]:
        lines.append("[[function]]")
        keys = [k for k in _FUNC_ORDER if k in fn] + [k for k in fn if k not in _FUNC_ORDER]
        for k in keys:
            lines.append(f"{k} = {_fmt_value(k, fn[k])}")
        lines.append("")
    import os
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


def load_toml(path):
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    return {
        "module": doc.get("module", "elf"),
        "entry": doc.get("entry", 0),
        "functions": doc.get("function", []),
    }


def roundtrip_check(model, workdir):
    # Emit the model, simulate a hand correction (rename one function, skip another, add a
    # note), then load -> re-emit -> load and require the corrections and the full function
    # set to survive byte-for-byte.
    import os
    os.makedirs(workdir, exist_ok=True)
    t1 = os.path.join(workdir, "functions.toml")
    emit_toml(model, t1)

    corrected = load_toml(t1)
    if len(corrected["functions"]) >= 2:
        corrected["functions"][0]["name"] = "module_entry"
        corrected["functions"][1]["skip"] = True
        corrected["functions"][1]["note"] = "hand-corrected: handled by HLE"
    tc = os.path.join(workdir, "functions.corrected.toml")
    emit_toml(corrected, tc)

    # Idempotence: loading and re-emitting the corrected TOML must be byte-identical.
    reloaded = load_toml(tc)
    tc2 = os.path.join(workdir, "functions.corrected.2.toml")
    emit_toml(reloaded, tc2)
    a = open(tc, "rb").read()
    b = open(tc2, "rb").read()
    ok = a == b
    # And the corrections must still be present after the round-trip.
    fn0, fn1 = reloaded["functions"][0], reloaded["functions"][1]
    preserved = fn0.get("name") == "module_entry" and fn1.get("skip") is True and "note" in fn1
    same_count = len(reloaded["functions"]) == len(model["functions"])
    return ok and preserved and same_count, t1


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    if not args:
        sys.stderr.write("usage: analyze.py <elf> [--base=HEX] [--extra-span=LO,HI] [--toml=out.toml] [--check=workdir] [--quiet]\n")
        return 2
    base = None
    extra_span_arg = None
    for o in opts:
        if o.startswith("--base="):
            base = int(o.split("=", 1)[1], 16)
        elif o.startswith("--extra-span="):
            extra_span_arg = o.split("=", 1)[1]
    elf = Elf(args[0], base=base)
    starts, ranges = analyze(elf, extra_spans=resolve_extra_spans(extra_span_arg))
    model = build_model(elf, starts)

    quiet = "--quiet" in opts
    rc = 0
    truth = elf.func_symbols()
    if truth is not None:
        truth_in = set(a for a in truth if in_ranges(a, ranges))
        found = truth_in & starts
        recall = len(found) / len(truth_in) if truth_in else 1.0
        missed = sorted(truth_in - starts)
        if not quiet:
            print(f"ground-truth functions (in exec ranges): {len(truth_in)}")
            print(f"discovered entry points: {len(starts)}")
            print(f"recovered: {len(found)}  recall: {recall*100:.2f}%")
            if missed:
                print(f"missed {len(missed)} (first 20): " + ", ".join("0x%08x" % a for a in missed[:20]))
        print(f"RECALL {recall*100:.2f}% ({len(found)}/{len(truth_in)})")
        if recall < 0.95:
            rc = 1

    for o in opts:
        if o.startswith("--toml="):
            emit_toml(model, o.split("=", 1)[1])
            print("wrote TOML:", o.split("=", 1)[1])
        if o.startswith("--check="):
            ok, t1 = roundtrip_check(model, o.split("=", 1)[1])
            print(f"TOML round-trip (hand-corrected, lossless): {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
