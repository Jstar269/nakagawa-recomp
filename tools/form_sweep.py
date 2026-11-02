# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Census-driven sweep of every scalar instruction form ``tools/codegen.py`` emits (#1844).

Translation bugs live in aliasing: a temporary read after the destination was written, a
sign extension done on the wrong copy. Fixed cases (the cosim cells, ``gen_microtest.py``,
``fp_convert_selftest.c``) do not enumerate it. This tool does, in four steps:

``census``     asks ``codegen.effect`` itself which instruction words it translates. Nothing
               here lists forms by hand: a word is probed, its emitted C is normalised
               (register indices and numerals blanked) and words with the same normalised C
               under the same decode selector form one *form*. A form added to codegen
               appears as a new census row and is swept without any edit here.
``instances``  expands every form into encodings: each aliasing pattern of its register
               operands (``rd == rs``, ``rd == rt``, ``rs == rt``, ``$zero`` operands, in
               the first-appearance renaming of ``alias_patterns``) crossed with each
               immediate bucket (``imm_bucket``), sampled down to a cap per form when the
               product is large, and reported as sampled.
``generate``   writes a synthetic PSP module holding one tiny guest function per instance,
               and a plan file, into the ignored build tree. The ordinary pipeline
               (``prxload.py`` -> ``codegen.py``) translates it, so the sweep runs the real
               code generator.
``report``     reads the verdict lines ``src/rt/form_sweep.c`` prints after it ran every
               instance from the same seeded states on the translated body and on the
               reference interpreter (``src/ref``), and prints one line per form.

Public mode (this file) builds only synthetic encodings; it contains and reads no game data.
Private mode (a title's own emitted words as the census) is a different input, never
committed (AGENTS.md section 5).
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import itertools
import json
import random
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen  # noqa: E402  (tools/ is the import root for tool modules)

# ---------------------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------------------

# Field name -> (shift, width). ``imm`` is the 16-bit immediate of an I-type word and
# overlaps rd, sa and funct; R-type words use rd, sa and funct instead.
FIELDS = {"rs": (21, 5), "rt": (16, 5), "rd": (11, 5), "sa": (6, 5), "funct": (0, 6),
          "imm": (0, 16)}

#: Opcodes whose decode continues in the low bits (MIPS I: SPECIAL, COP1; Allegrex SPECIAL3).
R_GROUPS = (0x00, 0x11, 0x1F)

#: Nominal field values for probing: distinct and non-zero, so a register index in the
#: emitted C can be traced back to the field that produced it.
NOMINAL = {"rs": 5, "rt": 7, "rd": 9, "sa": 11, "funct": 0, "imm": 0x1234}

_REG_INDEX = re.compile(r"s->(?:r|f|fi)\[(\d+)\]")
_NUMERAL = re.compile(r"\b(?:0x[0-9a-fA-F]+|\d+)u?\b")
_ZERO_READ = re.compile(r"\b0u\b")


def word_of(op: int, **fields: int) -> int:
    value = (op & 0x3F) << 26
    for name, v in fields.items():
        shift, width = FIELDS[name]
        value |= (v & ((1 << width) - 1)) << shift
    return value & 0xFFFFFFFF


def field_of(word: int, name: str) -> int:
    shift, width = FIELDS[name]
    return (word >> shift) & ((1 << width) - 1)


# ---------------------------------------------------------------------------------------
# Probing codegen
# ---------------------------------------------------------------------------------------

PROBE_ADDR = 0x08C00000


class _RoutedToVfpu(Exception):
    """Raised in place of ``codegen.vfpu_effect`` while probing, to learn which words route there."""


@dataclass(frozen=True)
class Probe:
    text: str | None          # emitted C for the instruction, None when codegen refuses it
    vfpu: bool = False        # codegen routes the word to the VFPU emitter


@functools.lru_cache(maxsize=None)
def probe(word: int) -> Probe:
    """What ``codegen.effect`` does with ``word`` on the default (non-LLE, non-profile) path."""
    original = codegen.vfpu_effect

    def routed(*args, **kwargs):
        raise _RoutedToVfpu()

    codegen.vfpu_effect = routed
    try:
        text = codegen.effect(PROBE_ADDR, word)[0]
    except _RoutedToVfpu:
        return Probe(None, vfpu=True)
    except Exception:  # noqa: BLE001  (codegen raises Unsupported and arithmetic errors alike)
        return Probe(None)
    finally:
        codegen.vfpu_effect = original
    return Probe(text)


def blank_registers(text: str) -> str:
    """Text with every register index and every read of ``$zero`` replaced by ``REG``."""
    return _ZERO_READ.sub("REG", _REG_INDEX.sub("REG", text))


@functools.lru_cache(maxsize=None)
def shape(text: str) -> str:
    """Structure of an emission: register operands and every numeral blanked to ``N``."""
    return _NUMERAL.sub("N", _ZERO_READ.sub("N", _REG_INDEX.sub("N", text)))


def register_file(text: str, value: int) -> str | None:
    """``gpr`` or ``fpr`` when ``text`` indexes a register file with ``value``, else None."""
    hit = {m.group(0).split("[")[0] for m in _REG_INDEX.finditer(text)
           if int(m.group(1)) == value}
    if not hit:
        return None
    return "gpr" if hit == {"s->r"} else "fpr"


# ---------------------------------------------------------------------------------------
# Immediate buckets and aliasing patterns (what test_form_sweep pins)
# ---------------------------------------------------------------------------------------

EXACT_IMMEDIATES = (0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 31, 32)


def imm_bucket(value: int, width: int) -> str:
    """Bucket of an immediate field: exact for 0..8, 16, 24, 31, 32; else a named range.

    A field of up to six bits is an unsigned count, selector or register number: the rest
    split into ``low`` and ``high`` halves. A 16-bit field is an immediate: ``small``
    (< 0x100), ``large``, ``max`` (0x7FFF), ``min`` (0x8000), and the negative ranges
    ``neg-small`` (within 0x100 of -1), ``neg-large`` and ``-1`` itself. The value passed is
    always the raw unsigned field.
    """
    if value in EXACT_IMMEDIATES:
        return f"={value}"
    if width <= 6:
        return "low" if value < (1 << width) // 2 else "high"
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    if value == mask:
        return "-1"
    if value == top - 1:
        return "max"
    if value == top:
        return "min"
    if value > top:
        return "neg-small" if mask - value < 0x100 else "neg-large"
    return "small" if value < 0x100 else "large"


def alias_patterns(slots: int, zero_allowed: tuple[bool, ...] | None = None) -> list[tuple]:
    """Every distinct way ``slots`` register operands can name registers.

    A pattern is the operand list in first-appearance renaming: ``0, 1, 0`` is ``rs == rd``
    with ``rt`` different, and ``Z`` is the ``$zero`` register. Two operands that are both
    ``$zero`` are the same register, so ``Z`` may repeat while every other name is a fresh
    register only on first appearance. ``zero_allowed[i]`` False forbids ``Z`` for operand
    ``i`` (a memory base register, whose address would fall outside RAM).
    """
    if zero_allowed is None:
        zero_allowed = (True,) * slots
    out: list[tuple] = []

    def walk(prefix: tuple, used: int) -> None:
        i = len(prefix)
        if i == slots:
            out.append(prefix)
            return
        choices: list = list(range(used + 1))
        if zero_allowed[i]:
            choices.append("Z")
        for c in choices:
            walk(prefix + (c,), used + 1 if c == used else used)

    walk((), 0)
    return out


# ---------------------------------------------------------------------------------------
# Census
# ---------------------------------------------------------------------------------------

# Names for display only. Census membership never depends on this table; a form it does
# not name is reported by its encoding.
_SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
            0x0A: "movz", 0x0B: "movn", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
            0x16: "clz", 0x17: "clo", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
            0x1C: "madd", 0x1D: "maddu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
            0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
            0x2C: "max", 0x2D: "min", 0x2E: "msub", 0x2F: "msubu", 0x0C: "syscall",
            0x0D: "break", 0x0F: "sync"}
_OPS = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
        0x0E: "xori", 0x0F: "lui", 0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
        0x25: "lhu", 0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
        0x2F: "cache", 0x30: "ll", 0x31: "lwc1", 0x38: "sc", 0x39: "swc1", 0x3F: "vflush"}
_COP1_MOVES = {0x00: "mfc1", 0x02: "cfc1", 0x04: "mtc1", 0x06: "ctc1"}
_COP1_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s",
           0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s", 0x0C: "round.w.s", 0x0D: "trunc.w.s",
           0x0E: "ceil.w.s", 0x0F: "floor.w.s", 0x24: "cvt.w.s"}
_COND = ("f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle", "seq", "ngl", "lt",
         "nge", "le", "ngt")
_SPECIAL3 = {0x02: "wsbh", 0x03: "wsbw", 0x10: "seb", 0x18: "seh", 0x14: "bitrev"}


def mnemonic(word: int) -> str:
    """Display name of ``word``'s form, or its encoding when this table does not know it."""
    op = word >> 26
    funct = word & 0x3F
    if op == 0:
        if funct == 0x02 and field_of(word, "rs") == 1:
            return "rotr"
        if funct == 0x06 and field_of(word, "sa") == 1:
            return "rotrv"
        return _SPECIAL.get(funct, f"special.{funct:02x}")
    if op == 0x1F:
        if funct == 0x00:
            return "ext"
        if funct == 0x04:
            return "ins"
        if funct == 0x20:
            return _SPECIAL3.get(field_of(word, "sa"), f"bshfl.{field_of(word, 'sa'):02x}")
        return f"special3.{funct:02x}"
    if op == 0x11:
        fmt = field_of(word, "rs")
        if fmt in _COP1_MOVES:
            return _COP1_MOVES[fmt]
        if fmt == 0x10:
            if funct >= 0x30:
                return f"c.{_COND[funct & 0xF]}.s"
            return _COP1_S.get(funct, f"cop1.s.{funct:02x}")
        if fmt == 0x14 and funct == 0x20:
            return "cvt.s.w"
        return f"cop1.{fmt:02x}.{funct:02x}"
    return _OPS.get(op, f"op.{op:02x}")


@dataclass
class Form:
    """One instruction form: a decode selector plus operand roles, derived from codegen."""

    key: str                       # stable id, e.g. "00.21" or "00.02.1"
    name: str
    op: int
    fixed: dict[str, int]          # fields whose value selects the form (funct, fmt, ...)
    regs: dict[str, str]           # register-role fields -> "gpr" | "fpr"
    imms: tuple[str, ...]          # immediate-role fields, in encoding order
    members: list[tuple]           # valid value tuples of ``imms``
    memory: bool                   # touches guest memory (rs is the base register)
    delay_differs: bool            # the delay-slot emission differs from the plain one
    sample_word: int
    sample_text: str

    def describe(self) -> str:
        regs = ",".join(f"{n}:{f}" for n, f in sorted(self.regs.items()))
        return f"{self.key} {self.name} regs[{regs}] imms{list(self.imms)}"


@dataclass
class Census:
    forms: list[Form] = field(default_factory=list)
    excluded: dict[str, list[str]] = field(default_factory=dict)   # reason -> names
    refused: list[str] = field(default_factory=list)               # unsupported (op, funct) ids


_EXCLUDE_MARKERS = (
    ("sr_raw_syscall", "stops the run (covered by the snapshot cases, #1842)"),
    ("sr_break(", "stops the run (covered by the snapshot cases, #1842)"),
    ("vfpuCtrl", "VFPU control state (#326)"),
)

# Opcodes handled outside the scalar sweep. COP0 is the system coprocessor; VFPU words are
# detected by probing (``Probe.vfpu``), not listed.
_SYSTEM_OPS = {0x10: "COP0 system instructions (#1727 LLE gates cover them)"}


def _sweep(op: int, base: dict[str, int], name: str, values: range, funct_fixed: dict) -> list[Probe]:
    out = []
    for v in values:
        fields = dict(base)
        fields[name] = v
        out.append(probe(word_of(op, **fields)))
    return out


def _classify(op: int, base: dict[str, int], names: tuple[str, ...]) -> dict[str, str]:
    """Role of each field around ``base``: ``unused``, ``register`` or ``variant``.

    ``register``: varying the field changes only register indices (and reads of ``$zero``).
    ``variant``: it changes anything else (an immediate, or a selector of the form).
    Value 0 is left out of the sweep: a register operand of ``$zero`` is an aliasing
    pattern of the form (a dropped write, a constant read), not a different form.
    """
    roles = {}
    for name in names:
        width = FIELDS[name][1]
        values = (1, 2, 0x7FFF, 0x8000, 0xFFFF) if name == "imm" else range(1, 1 << width)
        texts = []
        for v in values:
            fields = dict(base)
            fields[name] = v
            texts.append(probe(word_of(op, **fields)).text)
        live = [t for t in texts if t is not None]
        if not live:
            roles[name] = "unused"
        elif len(live) != len(texts) or len({shape(t) for t in live}) > 1:
            roles[name] = "variant"
        elif len(set(live)) == 1:
            roles[name] = "unused"
        elif len({blank_registers(t) for t in live}) == 1:
            roles[name] = "register"
        else:
            roles[name] = "variant"
    return roles


def _bases(op: int, outer: dict[str, int], names: tuple[str, ...]) -> list[dict[str, int]]:
    """Starting points for role discovery: the nominal word and a few shape-changing anchors."""
    anchors = [dict(NOMINAL)]
    for rs_v, sa_v in ((0, 0), (1, 0), (0, 1), (0, 3), (1, 1)):
        a = dict(NOMINAL)
        a["rs"], a["sa"] = rs_v, sa_v
        anchors.append(a)
    # Last: with every register $zero a form's writes are dropped and its shape degenerates,
    # so this base is only for words that need all-zero fields (sync).
    anchors.append({"rs": 0, "rt": 0, "rd": 0, "sa": 0, "imm": 0})
    out = []
    for a in anchors:
        b = {n: a[n] for n in names}
        b.update(outer)
        out.append(b)
    return out


def is_r_like(op: int) -> bool:
    """True when the low six bits of ``op`` select the form (SPECIAL, COP1, SPECIAL3, ...).

    The known groups are listed in ``R_GROUPS``. Any other opcode is tested the same way: if
    the six low bits, read as an immediate, change which words codegen translates or what
    shape it emits, they are a function code and the opcode decodes like SPECIAL. A new
    SPECIAL2-style group added to codegen is therefore swept without an edit here.
    """
    if op in R_GROUPS:
        return True
    shapes = set()
    live = 0
    for funct in range(64):
        p = probe(word_of(op, rs=NOMINAL["rs"], rt=NOMINAL["rt"], rd=NOMINAL["rd"],
                          sa=NOMINAL["sa"], funct=funct))
        if p.text is not None:
            live += 1
            shapes.add(shape(p.text))
    return 0 < live < 64 or len(shapes) > 1


def _group_forms(op: int, outer: dict[str, int], census: Census) -> None:
    """Forms of one (opcode, outer selector) group."""
    r_type = is_r_like(op)
    names = ("rs", "rt", "rd", "sa") if r_type else ("rs", "rt", "imm")
    free = tuple(n for n in names if n not in outer)
    # Pick the first base that translates, then classify every free field around it.
    chosen = None
    for base in _bases(op, outer, names):
        if probe(word_of(op, **base)).text is not None:
            chosen = base
            break
    if chosen is None:
        # No anchor translates; the selector may sit on a variant field (srl needs rs 0/1).
        # Sweep rs and sa jointly before giving up.
        for rs_v, sa_v in itertools.product(range(32), repeat=2) if r_type else ((0, 0),):
            base = {n: NOMINAL[n] for n in names}
            base.update(outer)
            if r_type:
                base["rs"], base["sa"] = (rs_v, sa_v) if "rs" not in outer else (base["rs"], sa_v)
            if probe(word_of(op, **base)).text is not None:
                chosen = base
                break
    if chosen is None:
        census.refused.append(f"{op:02x}:" + ",".join(f"{k}={v:02x}" for k, v in sorted(outer.items())))
        return
    roles = _classify(op, chosen, free)
    variant = [n for n in free if roles[n] == "variant"]
    registers = [n for n in free if roles[n] == "register"]

    # Enumerate the variant fields jointly; registers and unused fields stay nominal.
    def ranges(name: str):
        if len(variant) > 2 and FIELDS[name][1] <= 6:
            # Three or more free fields is a code field (syscall, break): bucket
            # representatives suffice, the full product is 2^20 words.
            return [v for v in EXACT_IMMEDIATES if v < (1 << FIELDS[name][1])]
        return range(1 << FIELDS[name][1]) if FIELDS[name][1] <= 6 else (
            sorted({0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 31, 32, 0x1234, 0x7FFF, 0x8000,
                    0xFFF0, 0xFFFF, 0x0123, 0xC000}))

    clusters: dict[str, list[tuple]] = {}
    sample: dict[str, tuple[int, str]] = {}
    for values in itertools.product(*(ranges(n) for n in variant)):
        fields = dict(chosen)
        fields.update(zip(variant, values, strict=True))
        word = word_of(op, **fields)
        p = probe(word)
        if p.vfpu:
            census.excluded.setdefault("VFPU (#326, vfpu_fuzz)", []).append(f"op {op:02x}")
            return
        if p.text is None:
            continue
        sh = shape(p.text)
        clusters.setdefault(sh, []).append(values)
        sample.setdefault(sh, (word, p.text))
    if not clusters:
        census.refused.append(f"{op:02x}:" + ",".join(f"{k}={v:02x}" for k, v in sorted(outer.items())))
        return

    for index, (sh, members) in enumerate(clusters.items()):
        word, text = sample[sh]
        reason = _SYSTEM_OPS.get(op)
        for marker, why in _EXCLUDE_MARKERS:
            if marker in text:
                reason = why
        name = mnemonic(word)
        if reason:
            census.excluded.setdefault(reason, []).append(name)
            continue
        # A variant field that takes one value across the cluster selects the form; the
        # others are immediates whose values come from ``members``.
        fixed = dict(outer)
        imms = []
        for i, n in enumerate(variant):
            seen = {m[i] for m in members}
            if len(seen) == 1:
                fixed[n] = next(iter(seen))
            else:
                imms.append(n)
        imm_index = [variant.index(n) for n in imms]
        member_tuples = sorted({tuple(m[i] for i in imm_index) for m in members})
        regs = {}
        for n in registers:
            f = register_file(text, chosen[n])
            if f is None:
                # Register-role field that the nominal word did not index (read of $zero).
                f = "gpr"
            regs[n] = f
        key = ".".join(f"{x:02x}" for x in (op, *(outer[n] for n in ("rs", "funct") if n in outer)))
        if len(clusters) > 1:
            key += f".{index}"
        census.forms.append(Form(
            key=key, name=name, op=op, fixed=fixed, regs=regs, imms=tuple(imms),
            members=member_tuples,
            memory=any(t in text for t in ("MEM_R", "MEM_W", "sr_lw", "sr_sw")),
            delay_differs=_delay_differs(word),
            sample_word=word, sample_text=text))


def _delay_differs(word: int) -> bool:
    """True when codegen emits something other than the plain line for ``word`` in a delay slot."""
    try:
        slot = codegen.delay_slot_lines(PROBE_ADDR + 4, word, PROBE_ADDR, indent="")
        plain = codegen.normal_line(PROBE_ADDR + 4, word)
    except Exception:  # noqa: BLE001
        return True
    return [line.strip() for line in slot] != [plain.strip()]


def census() -> Census:
    """Every form ``codegen.effect`` translates on the default path, derived by probing."""
    result = Census()
    for op in range(64):
        if is_r_like(op) and op != 0x11:
            for funct in range(64):
                _group_forms(op, {"funct": funct}, result)
        elif op == 0x11:
            for fmt in range(32):
                # The function field selects a form only for formats where it changes the
                # emission; for the register moves it is a reserved field and one group does.
                texts = {probe(word_of(op, rs=fmt, rt=NOMINAL["rt"], rd=NOMINAL["rd"],
                                       sa=NOMINAL["sa"], funct=f)).text for f in range(64)}
                if len(texts) <= 1:
                    _group_forms(op, {"rs": fmt}, result)
                else:
                    for funct in range(64):
                        _group_forms(op, {"rs": fmt, "funct": funct}, result)
        else:
            _group_forms(op, {}, result)
    # COP1 forms keyed by fmt with funct as a variant field: split by funct where it selects.
    result.forms.sort(key=lambda f: (f.op, f.key))
    return result

# ---------------------------------------------------------------------------------------
# Instances
# ---------------------------------------------------------------------------------------

#: Scratch registers per pool. A form's aliasing pattern names slot 0, 1, 2 ...; the pool
#: maps those names to registers, so the register numbers vary across forms without adding
#: instances. $ra (31) is never used: the function returns through it.
GPR_POOLS = ((8, 9, 10, 11), (16, 17, 18, 19), (24, 25, 2, 3), (12, 13, 14, 15))
FPR_POOLS = ((0, 1, 2, 3), (4, 5, 6, 7), (28, 29, 30, 31), (12, 13, 14, 15))

#: The order register operands are named in a pattern: destination first.
SLOT_ORDER = ("rd", "rt", "rs", "sa")

#: Scratch window the sweep's loads and stores address (guest RAM, above the module).
SCRATCH = 0x09800000
SCRATCH_SIZE = 0x100
MODULE_BASE = 0x08C00000


@dataclass
class Instance:
    index: int
    form: Form
    word: int
    pattern: str            # human label, e.g. "rd=a rt=b rs=a"
    slots: dict[str, str]   # field -> register name
    regs: dict[str, int]    # field -> register number (0 = $zero)
    imm: tuple
    delay: bool             # placed in the jr $ra delay slot
    base_reg: int           # memory base register, -1 when the form does not touch memory
    simm: int               # memory offset, sign-extended
    gmask: int
    fmask: int
    addr: int = 0

    @property
    def name(self) -> str:
        return mnemonic(self.word)


def _form_slots(form: Form) -> tuple[list[str], list[str]]:
    gpr = [n for n in SLOT_ORDER if form.regs.get(n) == "gpr"]
    fpr = [n for n in SLOT_ORDER if form.regs.get(n) == "fpr"]
    return gpr, fpr


def immediate_reps(form: Form) -> list[tuple]:
    """One member of the form's immediate-field values per bucket combination."""
    if not form.imms:
        return [()]
    seen: dict[tuple, tuple] = {}
    for m in form.members:
        key = tuple(imm_bucket(v, FIELDS[n][1]) for n, v in zip(form.imms, m, strict=True))
        seen.setdefault(key, m)
    return list(seen.values())


def form_instances(form: Form, seed: int, cap: int | None, start: int = 0) -> tuple[list[Instance], int]:
    """Encodings of ``form``: aliasing patterns x immediate buckets, sampled to ``cap``.

    Returns the instances and the size of the full product. Every pattern and every
    immediate bucket appears at least once before random pairs fill the rest, so a
    sampled form still covers each aliasing pattern and each bucket.
    """
    gpr, fpr = _form_slots(form)
    gpats = alias_patterns(len(gpr), tuple(not (form.memory and n == "rs") for n in gpr))
    fpats = [p for p in alias_patterns(len(fpr), (False,) * len(fpr))]
    patterns = [(g, f) for g in gpats for f in fpats]
    reps = immediate_reps(form)
    total = len(patterns) * len(reps)
    pairs = [(patterns[k % len(patterns)], reps[k % len(reps)])
             for k in range(max(len(patterns), len(reps)))]
    chosen = set(pairs)
    rest = [(p, r) for p in patterns for r in reps if (p, r) not in chosen]
    if cap is not None and total > max(cap, len(pairs)):
        random.Random(f"{seed}:{form.key}").shuffle(rest)
        pairs += rest[: max(0, cap - len(pairs))]
    else:
        pairs += rest
    out = []
    for n, ((g, f), imm) in enumerate(pairs):
        pick = int(hashlib.sha256(f"{seed}:{form.key}:{n}".encode()).hexdigest(), 16)
        gpool = GPR_POOLS[pick % len(GPR_POOLS)]
        fpool = FPR_POOLS[(pick >> 8) % len(FPR_POOLS)]
        fields = dict(form.fixed)
        slots: dict[str, str] = {}
        regs: dict[str, int] = {}
        label = []
        gmask = fmask = 0
        for name, c in zip(gpr, g, strict=True):
            reg = 0 if c == "Z" else gpool[c]
            fields[name] = reg
            regs[name] = reg
            slots[name] = "$zero" if c == "Z" else f"${reg}"
            label.append(f"{name}={'Z' if c == 'Z' else chr(97 + c)}")
            if reg:
                gmask |= 1 << reg
        for name, c in zip(fpr, f, strict=True):
            reg = fpool[c]
            fields[name] = reg
            regs[name] = reg
            slots[name] = f"$f{reg}"
            label.append(f"{name}={chr(97 + c)}")
            fmask |= 1 << reg
        for name, v in zip(form.imms, imm, strict=True):
            fields[name] = v
            label.append(f"{name}={v:#x}")
        word = word_of(form.op, **fields)
        raw = field_of(word, "imm")
        simm = raw - 0x10000 if raw & 0x8000 else raw
        out.append(Instance(start + n, form, word, " ".join(label), slots, regs, imm, False,
                            fields.get("rs", 0) if form.memory else -1,
                            simm if form.memory else 0, gmask, fmask))
    if form.delay_differs:
        # Codegen emits a delay-slot instruction differently from a plain one for this form,
        # so each instance is also run in the delay slot of the closing jr $ra.
        for inst in list(out):
            out.append(Instance(start + len(out), form, inst.word, inst.pattern + " (delay)",
                                inst.slots, inst.regs, inst.imm, True, inst.base_reg, inst.simm,
                                inst.gmask, inst.fmask))
    return out, total


def all_instances(c: Census, seed: int, cap: int | None,
                  only: set[str] | None = None) -> tuple[list[Instance], dict[str, int]]:
    instances: list[Instance] = []
    totals: dict[str, int] = {}
    for form in c.forms:
        if only and form.key not in only:
            continue
        got, total = form_instances(form, seed, cap, start=len(instances))
        instances += got
        totals[form.key] = total
    return instances, totals


# ---------------------------------------------------------------------------------------
# Module and plan
# ---------------------------------------------------------------------------------------

TEXT_FILE_OFFSET = 0x100
DATA_FILE_SIZE = 0x40
BSS_SIZE = 0x10
DATA_MEMORY_SIZE = DATA_FILE_SIZE + BSS_SIZE
SHT_PRX_RELOC = 0x700000A0
R_MIPS_26 = 4
JR_RA = (31 << 21) | 0x08
NOP = 0
FUNCTION_STRIDE = 16


def _jal(offset: int) -> int:
    return (3 << 26) | ((offset >> 2) & 0x03FFFFFF)


def layout(instances: list[Instance]) -> tuple[list[int], int]:
    """Text words (one ``jal`` per instance so the analyzer claims each function start, then
    one 16-byte slot per instance) and the data segment offset."""
    entry_bytes = (len(instances) * 2 + 2) * 4
    first = (entry_bytes + 15) & ~15
    text = [NOP] * ((first + len(instances) * FUNCTION_STRIDE) // 4)
    for i, inst in enumerate(instances):
        offset = first + i * FUNCTION_STRIDE
        inst.addr = MODULE_BASE + offset
        text[i * 2] = _jal(offset)
        body = [JR_RA, inst.word] if inst.delay else [inst.word, JR_RA, NOP]
        text[offset // 4 : offset // 4 + len(body)] = body
    text[len(instances) * 2] = JR_RA
    data_offset = (len(text) * 4 + 0xFFF) & ~0xFFF
    return text, data_offset


def build_prx(instances: list[Instance]) -> bytes:
    """A minimal PSP module (no imports) whose text holds every instance's function."""
    words, data_offset = layout(instances)
    text = struct.pack(f"<{len(words)}I", *words)
    data_file_offset = TEXT_FILE_OFFSET + data_offset + 0x100
    data = bytearray(DATA_FILE_SIZE)
    name = b"formsweep"
    data[0:52] = struct.pack("<HH28s5I", 0, 0x0100, name + b"\0" * (28 - len(name)), 0, 0, 0, 0, 0)
    relocs = b"".join(struct.pack("<II", off, R_MIPS_26)
                      for off in range(0, len(words) * 4, 4) if (words[off // 4] >> 26) in (2, 3))
    relocation_offset = data_file_offset + DATA_FILE_SIZE
    names = b"\0.text\0.rodata.sceModuleInfo\0.data\0.reloc.sceModuleInfo\0.bss\0.shstrtab\0"
    idx = {n: names.index(n.encode()) for n in
           (".text", ".rodata.sceModuleInfo", ".data", ".reloc.sceModuleInfo", ".bss", ".shstrtab")}
    shstr_offset = relocation_offset + len(relocs)
    table_offset = (shstr_offset + len(names) + 3) & ~3
    count = 7
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    header = ident + struct.pack("<HHIIIIIHHHHHH", 0xFFA0, 8, 1, 0, 52, table_offset, 0x10, 52, 32,
                                 2, 40, count, count - 1)
    phdrs = (struct.pack("<8I", 1, TEXT_FILE_OFFSET, 0, 0, len(text), len(text), 5, 0x1000)
             + struct.pack("<8I", 1, data_file_offset, data_offset, data_offset, DATA_FILE_SIZE,
                           DATA_MEMORY_SIZE, 6, 0x1000))

    def section(n, kind, flags, addr, off, size, align, entry=0):
        return struct.pack("<10I", idx[n], kind, flags, addr, off, size, 0, 0, align, entry)

    sections = [struct.pack("<10I", *([0] * 10)),
                section(".text", 1, 6, 0, TEXT_FILE_OFFSET, len(text), 4),
                section(".rodata.sceModuleInfo", 1, 2, data_offset, data_file_offset, 52, 4),
                section(".data", 1, 3, data_offset + 0x34, data_file_offset + 0x34,
                        DATA_FILE_SIZE - 0x34, 4),
                section(".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0, relocation_offset,
                        len(relocs), 4, 8),
                section(".bss", 8, 3, data_offset + DATA_FILE_SIZE,
                        data_file_offset + DATA_FILE_SIZE, BSS_SIZE, 16),
                section(".shstrtab", 3, 0, 0, shstr_offset, len(names), 1)]
    blob = bytearray(table_offset + count * 40)
    blob[0:len(header)] = header
    blob[52:52 + len(phdrs)] = phdrs
    blob[TEXT_FILE_OFFSET:TEXT_FILE_OFFSET + len(text)] = text
    blob[data_file_offset:data_file_offset + len(data)] = data
    blob[relocation_offset:relocation_offset + len(relocs)] = relocs
    blob[shstr_offset:shstr_offset + len(names)] = names
    table = b"".join(sections)
    blob[table_offset:table_offset + len(table)] = table
    return bytes(blob)


def build_psp_header(instances: list[Instance]) -> bytes:
    words, _data_offset = layout(instances)
    header = bytearray(0x80)
    header[:4] = b"~PSP"
    header[0x27] = 2
    struct.pack_into("<I", header, 0x38, BSS_SIZE)
    struct.pack_into("<4I", header, 0x54, len(words) * 4, DATA_MEMORY_SIZE, 0, 0)
    return bytes(header)


def plan_text(instances: list[Instance], seed: int, states: int) -> str:
    """The harness input: one line per instance (index, address, word, memory base, offset, masks)."""
    lines = [f"seed {seed}", f"states {states}", f"scratch {SCRATCH:x} {SCRATCH_SIZE:x}"]
    for i in instances:
        lines.append(f"inst {i.index} {i.addr:x} {i.word:x} {i.base_reg} {i.simm & 0xFFFFFFFF:x} "
                     f"{i.gmask:x} {i.fmask:x}")
    return "\n".join(lines) + "\n"


def write_if_changed(path: Path, data: bytes) -> None:
    """Keep the timestamp of an unchanged output so Make does not regenerate the C."""
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def manifest(instances: list[Instance], totals: dict[str, int], c: Census, seed: int,
             cap: int | None, states: int) -> dict:
    return {
        "base": MODULE_BASE, "seed": seed, "cap": cap, "states": states,
        "instances": [{"index": i.index, "form": i.form.key, "name": i.name, "word": i.word,
                       "pattern": i.pattern, "slots": i.slots, "regs": i.regs, "delay": i.delay}
                      for i in instances],
        "forms": [{"key": f.key, "name": f.name, "op": f.op, "word": f.sample_word,
                   "product": totals.get(f.key), "memory": f.memory}
                  for f in c.forms],
        "excluded": {k: sorted(set(v)) for k, v in c.excluded.items()},
        "refused": len(c.refused),
    }


def generate(out_dir: Path, seed: int, cap: int | None, states: int, only: set[str] | None) -> int:
    c = census()
    instances, totals = all_instances(c, seed, cap, only)
    if not instances:
        print("form-sweep: no instances (unknown --only form?)", file=sys.stderr)
        return 2
    write_if_changed(out_dir / "guest.prx", build_prx(instances))
    write_if_changed(out_dir / "guest.psp", build_psp_header(instances))
    write_if_changed(out_dir / "plan.txt", plan_text(instances, seed, states).encode("ascii"))
    write_if_changed(out_dir / "manifest.json",
                     (json.dumps(manifest(instances, totals, c, seed, cap, states), indent=1,
                                 sort_keys=True) + "\n").encode("ascii"))
    print(f"form-sweep: {len(c.forms)} forms, {len(instances)} instances, seed {seed}, "
          f"{states} states -> {out_dir}")
    return 0


# ---------------------------------------------------------------------------------------
# Verdicts and report
# ---------------------------------------------------------------------------------------

_VERDICT = re.compile(r"^INST (\d+) (MATCH|MISMATCH|NOT_CHECKED)(?: (.*))?$")


def parse_verdicts(text: str) -> dict[int, tuple[str, str]]:
    """Harness output -> {instance index: (verdict, detail)}."""
    out: dict[int, tuple[str, str]] = {}
    for line in text.splitlines():
        m = _VERDICT.match(line.strip())
        if m:
            out[int(m.group(1))] = (m.group(2), m.group(3) or "")
    return out


def summarize(man: dict, verdicts: dict[int, tuple[str, str]]) -> tuple[list[str], dict]:
    """One report line per form, and the totals."""
    per_form: dict[str, dict] = {}
    for rec in man["instances"]:
        v = verdicts.get(rec["index"], ("MISSING", ""))
        row = per_form.setdefault(rec["form"], {"MATCH": 0, "MISMATCH": 0, "NOT_CHECKED": 0,
                                                 "MISSING": 0, "first": None, "why": None})
        row[v[0]] += 1
        if v[0] == "MISMATCH" and row["first"] is None:
            row["first"] = (rec["word"], rec["pattern"], v[1])
        if v[0] == "NOT_CHECKED" and row["why"] is None:
            row["why"] = (rec["word"], v[1])
    lines = []
    totals = {"MATCH": 0, "MISMATCH": 0, "NOT_CHECKED": 0, "MISSING": 0, "forms": 0,
              "forms_unchecked": 0}
    for form in man["forms"]:
        row = per_form.get(form["key"])
        if row is None:
            continue
        totals["forms"] += 1
        for k in ("MATCH", "MISMATCH", "NOT_CHECKED", "MISSING"):
            totals[k] += row[k]
        status = ("MISMATCH" if row["MISMATCH"] else "MISSING" if row["MISSING"]
                  else "NOT_CHECKED" if row["MATCH"] == 0 else "MATCH")
        if status == "NOT_CHECKED":
            totals["forms_unchecked"] += 1
        line = (f"{status:11s} {form['key']:9s} {form['name']:10s} match={row['MATCH']} "
                f"mismatch={row['MISMATCH']} not_checked={row['NOT_CHECKED']}")
        if row["first"]:
            word, pattern, detail = row["first"]
            line += f" first: op {word:#010x} [{pattern}] {detail}"
        if row["why"]:
            word, detail = row["why"]
            line += f" op {word:#010x} {detail}"
        lines.append(line)
    return lines, totals


def report(manifest_path: Path, results_path: Path) -> int:
    man = json.loads(manifest_path.read_text())
    verdicts = parse_verdicts(results_path.read_text())
    lines, totals = summarize(man, verdicts)
    print("\n".join(lines))
    for reason, names in man["excluded"].items():
        print(f"EXCLUDED    {','.join(names)}: {reason}")
    unchecked = [ln.split()[2] for ln in lines if ln.startswith("NOT_CHECKED")]
    print(f"form-sweep: {totals['forms']} forms, {totals['MATCH']} instances MATCH, "
          f"{totals['MISMATCH']} MISMATCH, {totals['NOT_CHECKED']} NOT_CHECKED, "
          f"{totals['MISSING']} MISSING; seed {man['seed']}, {man['states']} states")
    if unchecked:
        print("form-sweep: forms with no checked instance (NOT_CHECKED): " + ", ".join(unchecked))
    return 1 if totals["MISMATCH"] or totals["MISSING"] else 0


# ---------------------------------------------------------------------------------------
# Generated mutant (the sweep must be able to fail)
# ---------------------------------------------------------------------------------------

#: The form the mutant breaks and the defect: ``addu`` that reads rs after writing rd.
MUTANT_FORM = "00.21"
MUTANT_SOURCE = """# Generated by tools/form_sweep.py write-mutant. Not a tracked file.
# The real code generator with one deliberately wrong emission: addu writes rd first and
# then reads rs, so it is wrong exactly when rd is rs (and rs differs from rt).
import sys
sys.path.insert(0, {tools!r})
import codegen

_effect = codegen.effect


def effect(addr, w, *args, **kwargs):
    if (w >> 26) == 0 and (w & 0x3F) == 0x21:
        a, b, d = codegen.rs(w), codegen.rt(w), codegen.rd(w)
        stmt = (codegen.wr(d, codegen.R(b)) + " "
                + codegen.wr(d, "(" + codegen.R(a) + " + " + codegen.R(d) + ")"))
        return stmt, None, 0
    return _effect(addr, w, *args, **kwargs)


codegen.effect = effect
sys.exit(codegen.main(sys.argv))
"""


def write_mutant(out: Path) -> int:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(MUTANT_SOURCE.format(tools=str(ROOT / "tools")))
    print(f"form-sweep: wrote the mutant code generator {out}")
    return 0


def mutant_expected(man: dict) -> set[int]:
    """Instances of the mutated form where the defect changes the result: rd is rs, not $zero,
    and rt names a different register (or $zero)."""
    out = set()
    for rec in man["instances"]:
        regs = rec["regs"]
        if rec["form"] != MUTANT_FORM or rec["delay"]:
            continue
        if regs["rd"] != 0 and regs["rd"] == regs["rs"] and regs["rt"] != regs["rs"]:
            out.add(rec["index"])
    return out


def check_mutant(manifest_path: Path, results_path: Path) -> int:
    man = json.loads(manifest_path.read_text())
    verdicts = parse_verdicts(results_path.read_text())
    expected = mutant_expected(man)
    got = {i for i, (v, _d) in verdicts.items() if v == "MISMATCH"}
    unchecked = sorted(i for i, (v, _d) in verdicts.items() if v == "NOT_CHECKED")
    missing = [r["index"] for r in man["instances"] if r["index"] not in verdicts]
    print(f"form-sweep mutant: {len(man['instances'])} addu instances, "
          f"{len(expected)} with rd == rs != rt, {len(got)} MISMATCH")
    if not expected:
        print("form-sweep mutant: FAIL, the plan has no aliased instance to catch")
        return 1
    if unchecked or missing:
        print(f"form-sweep mutant: FAIL, unchecked {unchecked[:5]} missing {missing[:5]}")
        return 1
    if got != expected:
        print(f"form-sweep mutant: FAIL, mismatches differ from the aliased set: "
              f"missed {sorted(expected - got)[:8]} extra {sorted(got - expected)[:8]}")
        return 1
    print("form-sweep mutant: PASS, exactly the aliased instances mismatch "
          f"(e.g. {verdicts[min(got)][1]})")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("census", help="print the derived form list")
    gen = sub.add_parser("generate", help="write guest.prx, guest.psp, plan.txt, manifest.json")
    gen.add_argument("--out-dir", type=Path, required=True)
    gen.add_argument("--seed", type=int, default=1844)
    gen.add_argument("--cap", type=int, default=40, help="instances per form (0 = all)")
    gen.add_argument("--states", type=int, default=64)
    gen.add_argument("--only", action="append", default=[], help="restrict to a form key")
    mut = sub.add_parser("write-mutant", help="write the mutant code generator")
    mut.add_argument("--out", type=Path, required=True)
    chk = sub.add_parser("check-mutant", help="require the mutant's exact mismatch set")
    chk.add_argument("--manifest", type=Path, required=True)
    chk.add_argument("--results", type=Path, required=True)
    rep = sub.add_parser("report", help="summarise the harness output")
    rep.add_argument("--manifest", type=Path, required=True)
    rep.add_argument("--results", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "census":
        c = census()
        for f in c.forms:
            print(f.describe())
        print(len(c.forms), "forms")
        for reason, names in c.excluded.items():
            print("excluded:", reason, sorted(set(names)))
        return 0
    if args.command == "generate":
        return generate(args.out_dir, args.seed, args.cap or None, args.states,
                        set(args.only) or None)
    if args.command == "write-mutant":
        return write_mutant(args.out)
    if args.command == "check-mutant":
        return check_mutant(args.manifest, args.results)
    return report(args.manifest, args.results)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
